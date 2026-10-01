#pragma once

#include "core/BoundedAsyncLog.h"

// Windows-only, included by the D3D adapter after the SDK headers.
#include "compositor/ComPtrLite.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>

namespace corevideo::modules {

// Decouples a shell-facing keyed-mutex shared texture (multiview / preview) from
// the core render immediate context.
//
// Why this exists: publishing a shared texture with
// `renderContext->AcquireSync(0) -> CopyResource -> ReleaseSync(1)` couples the
// render context's GPU timeline to the CONSUMER's release fence. A keyed mutex
// orders GPU work, so the producer's next AcquireSync(0) inserts a GPU-side wait
// on the consumer's release. When the consumer (the WinUI shell present) sits
// behind a slow display or a saturated GPU, that latency propagates straight into
// the render context and stalls Program (see the render-stall / #516 + #526
// investigation). The 0 ms CPU timeout does not help: AcquireSync returns S_OK on
// the CPU and the wait is on the GPU.
//
// The fix mirrors D3DProgramBuffer: the render context only ever hands a frame to
// an INTERNAL, always-released keyed-mutex slot (bounded coupling to this fast
// export device). A dedicated export device/thread then pulls the slot into a
// private texture, releases the slot immediately, and publishes to the
// shell-facing output on its OWN timeline. The unbounded consumer coupling now
// lives on the export device; the render context never waits on the shell.
class D3DDecoupledExport {
  friend struct D3DDecoupledExportTestAccess;

 public:
  // #724. Creating an exporter means creating a D3D device: about 20 ms, measured.
  // Immediate does that on the caller's thread, as before. Deferred does it on the
  // exporter's own worker thread, so a render thread that meets a new source (a
  // clip taken to Program, a guest joining, a capture device connecting) does not
  // lose a frame to it. Until a deferred exporter is ready() it accepts no frame
  // and has no handle; its caller skips that export for a couple of frames.
  enum class Creation { Immediate, Deferred };

  D3DDecoupledExport(ID3D11Device* producer, int width, int height, const char* label,
                     Creation creation = Creation::Immediate)
      : width_(width), height_(height), label_(label) {
    if (creation == Creation::Immediate) {
      if (!initialize(producer)) { state_.store(kFailed, std::memory_order_release); return; }
      state_.store(kReady, std::memory_order_release);
      try {
        worker_ = std::thread([this] { try { exportLoop(); } catch (...) {} });
      } catch (...) {
        state_.store(kFailed, std::memory_order_release);
      }
      return;
    }
    // The device is free-threaded for resource creation; only the immediate context
    // belongs to the render thread, and initialize() never touches it. Hold a
    // reference for the worker: the caller's pointer is only promised for this call.
    producer->AddRef();
    try {
      worker_ = std::thread([this, producer] {
        bool created = false;
        try { created = initialize(producer); } catch (...) {}
        producer->Release();
        state_.store(created ? kReady : kFailed, std::memory_order_release);
        if (created) { try { exportLoop(); } catch (...) {} }
      });
    } catch (...) {
      producer->Release();
      state_.store(kFailed, std::memory_order_release);
    }
  }
  ~D3DDecoupledExport() {
    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  // Not failed. A deferred exporter still being created is valid, so a caller that
  // discards an invalid exporter does not discard one that is about to work.
  bool valid() const { return state_.load(std::memory_order_acquire) != kFailed; }
  // Created: submit() takes frames and handle() names the shared output.
  bool ready() const { return state_.load(std::memory_order_acquire) == kReady; }
  // For a caller that must have the export before it can continue. Bounded.
  bool waitUntilReady(std::chrono::milliseconds limit) const {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (state_.load(std::memory_order_acquire) == kPending) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    return ready();
  }
  // Resolution changes must not recreate the D3D device on the render thread.
  // Retire the worker before replacing resources; reuse its device/context.
  bool resize(ID3D11Device* producer, int width, int height) {
    if (dimensions(width, height)) return valid();
    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
    queue_.clear();
    slots_.clear();
    prepared_ = {}; output_ = {}; outputMutex_ = {};
    outputHandle_ = nullptr;
    publishedFrameNumber_ = std::make_shared<std::atomic<int64_t>>(-1);
    width_ = width; height_ = height;
    stopped_ = false;
    state_.store(kPending, std::memory_order_release);
    if (!initialize(producer)) { state_.store(kFailed, std::memory_order_release); return false; }
    state_.store(kReady, std::memory_order_release);
    try {
      worker_ = std::thread([this] { try { exportLoop(); } catch (...) {} });
    } catch (...) { state_.store(kFailed, std::memory_order_release); }
    return ready();
  }
  bool dimensions(int width, int height) const { return width == width_ && height == height_; }
  HANDLE handle() const { return ready() ? outputHandle_ : nullptr; }
  int width() const { return width_; }
  int height() const { return height_; }
  std::shared_ptr<std::atomic<int64_t>> publishedFrameNumber() const { return publishedFrameNumber_; }

  // Called on the render thread. Copies `src` (owned by the render device) into a
  // free internal slot with a single keyed-mutex acquire/release that only couples
  // to this fast export device. Does not flush: the render context's end-of-pass
  // Flush submits the copy, exactly as D3DProgramBuffer::submit relies on. Drops
  // the frame (bounded, non-blocking) when every slot is still in flight.
  bool submit(ID3D11DeviceContext* producer, ID3D11Texture2D* src, int64_t frameNumber = 0) {
    if (!ready() || !src) return false;
    Slot* slot = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopped_) return false;
      for (auto& candidate : slots_) {
        if (candidate->state == State::Free) { slot = candidate.get(); slot->state = State::Writing; break; }
      }
    }
    if (!slot) { ++dropped_; return false; }
    if (slot->producerMutex->AcquireSync(0, 0) != S_OK) {
      std::lock_guard<std::mutex> lock(mutex_);
      slot->state = State::Free;
      ++producerBusy_;
      return false;
    }
    producer->CopyResource(slot->texture.get(), src);
    slot->producerMutex->ReleaseSync(1);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      slot->state = State::Submitted;
      slot->frameNumber = frameNumber;
      queue_.push_back(slot);
    }
    changed_.notify_one();
    return true;
  }

 private:
  enum class State { Free, Writing, Submitted, Consuming };
  struct Slot {
    int64_t frameNumber = 0;
    State state = State::Free;
    ComPtrLite<ID3D11Texture2D> texture;        // on producer device
    ComPtrLite<ID3D11Texture2D> opened;         // same resource opened on export device
    ComPtrLite<IDXGIKeyedMutex> producerMutex;  // producer-device view (used on the render thread)
    ComPtrLite<IDXGIKeyedMutex> mutex;          // export-device view (used on the export thread)
  };
  static constexpr int kDepth = 3;

  static bool share(ID3D11Texture2D* texture, HANDLE& handle) {
    ComPtrLite<IDXGIResource> resource;
    return SUCCEEDED(texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(resource.put()))) &&
           SUCCEEDED(resource->GetSharedHandle(&handle)) && handle;
  }

  bool initialize(ID3D11Device* producer) {
    ComPtrLite<IDXGIDevice> dxgi;
    ComPtrLite<IDXGIAdapter> adapter;
    if (!exportDevice_ && (FAILED(producer->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi.put()))) ||
        FAILED(dxgi->GetAdapter(adapter.put())) ||
        FAILED(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            exportDevice_.put(), nullptr, exportContext_.put())))) {
      return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    for (int i = 0; i < kDepth; ++i) {
      auto slot = std::make_unique<Slot>();
      HANDLE handle = nullptr;
      if (FAILED(producer->CreateTexture2D(&desc, nullptr, slot->texture.put())) ||
          !share(slot->texture.get(), handle) ||
          FAILED(slot->texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(slot->producerMutex.put()))) ||
          FAILED(exportDevice_->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(slot->opened.put()))) ||
          FAILED(slot->opened->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(slot->mutex.put())))) {
        return false;
      }
      slots_.push_back(std::move(slot));
    }

    // Private staging on the export device: the slot is released back to the
    // producer right after this copy, so the slot's release fence never sits
    // behind the (shell-coupled) output publish below.
    D3D11_TEXTURE2D_DESC priv = desc;
    priv.MiscFlags = 0;
    if (FAILED(exportDevice_->CreateTexture2D(&priv, nullptr, prepared_.put()))) return false;

    // Shell-facing output, created on the export device (like the program
    // buffer's output). The shell opens this handle on its own device.
    HANDLE outHandle = nullptr;
    if (FAILED(exportDevice_->CreateTexture2D(&desc, nullptr, output_.put())) ||
        !share(output_.get(), outHandle) ||
        FAILED(output_->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(outputMutex_.put())))) {
      return false;
    }
    outputHandle_ = outHandle;
    creationThread_ = std::this_thread::get_id();
    return true;
  }

  // Publish prepared_ to the consumer texture. False means the encoder still
  // holds the key; the pixels stay in prepared_ and must be retried. Dropping
  // them here is how frames vanished with encoderExportShedFrames still zero.
  bool publishPrepared(int64_t frameNumber) {
    bool owned = outputMutex_->AcquireSync(0, 0) == S_OK;
    if (!owned) owned = outputMutex_->AcquireSync(1, 0) == S_OK;
    if (!owned) return false;
    exportContext_->CopyResource(output_.get(), prepared_.get());
    publishedFrameNumber_->store(frameNumber, std::memory_order_release);
    outputMutex_->ReleaseSync(1);
    ++published_;
    exportContext_->Flush();
    return true;
  }

  void exportLoop() {
    bool pending = false;
    int64_t pendingFrame = 0;
    for (;;) {
      Slot* slot = nullptr;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!pending) {
          changed_.wait(lock, [&] { return stopped_ || !queue_.empty(); });
        } else if (queue_.empty() && !stopped_) {
          // The newest frame is already in prepared_. Wait until the encoder
          // releases the output key or a newer slot arrives. Do not discard it.
          changed_.wait_for(lock, std::chrono::milliseconds(1), [&] {
            return stopped_ || !queue_.empty();
          });
        }
        if (stopped_ && queue_.empty() && !pending) return;
        if (!queue_.empty()) {
          slot = queue_.front();
          queue_.pop_front();
          slot->state = State::Consuming;
        } else if (stopped_) {
          return;
        }
      }

      if (slot) {
        // Pull the slot into private staging. AcquireSync(1) returns S_OK on the CPU
        // as soon as the producer's ReleaseSync(1) handed over the key; the fence
        // wait for the producer copy is on THIS export context, never the render one.
        const bool pulled = slot->mutex->AcquireSync(1, 0) == S_OK;
        const int64_t frameNumber = slot->frameNumber;
        if (pulled) {
          exportContext_->CopyResource(prepared_.get(), slot->opened.get());
          slot->mutex->ReleaseSync(0);  // slot free for the producer immediately
        } else {
          // Should not happen (producer released before enqueue); return the key so
          // the producer can never wedge on a slot we failed to take.
          slot->mutex->ReleaseSync(0);
        }
        {
          std::lock_guard<std::mutex> lock(mutex_);
          slot->state = State::Free;
        }
        changed_.notify_all();
        if (pulled) {
          pending = true;
          pendingFrame = frameNumber;
        }
      }

      if (!pending) continue;
      if (publishPrepared(pendingFrame)) {
        pending = false;
      } else {
        ++consumerBusy_;
        if (stopped_) return;
      }
    }
  }

  int width_, height_;
  const char* label_;
  // Written by whichever thread runs initialize(). Everything initialize() fills is
  // read by the caller only after this reads kReady.
  static constexpr int kPending = 0, kReady = 1, kFailed = 2;
  std::atomic<int> state_{kPending};
  std::thread::id creationThread_;
  bool stopped_ = false;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<std::unique_ptr<Slot>> slots_;
  std::deque<Slot*> queue_;
  ComPtrLite<ID3D11Device> exportDevice_;
  ComPtrLite<ID3D11DeviceContext> exportContext_;
  ComPtrLite<ID3D11Texture2D> prepared_;
  ComPtrLite<ID3D11Texture2D> output_;
  ComPtrLite<IDXGIKeyedMutex> outputMutex_;
  HANDLE outputHandle_ = nullptr;
  std::shared_ptr<std::atomic<int64_t>> publishedFrameNumber_ = std::make_shared<std::atomic<int64_t>>(-1);
  std::thread worker_;
  std::uint64_t published_ = 0, dropped_ = 0, producerBusy_ = 0, consumerBusy_ = 0;
};

}  // namespace corevideo::modules
