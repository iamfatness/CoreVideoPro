#pragma once

#include "core/BoundedAsyncLog.h"

// Windows-only, included by the D3D adapter after the SDK headers.
#include "compositor/ComPtrLite.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

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
  D3DDecoupledExport(ID3D11Device* producer, int width, int height, const char* label)
      : width_(width), height_(height), label_(label) {
    if (!initialize(producer)) { initialized_ = false; return; }
    try {
      worker_ = std::thread([this] { try { exportLoop(); } catch (...) {} });
    } catch (...) {
      initialized_ = false;
    }
  }
  ~D3DDecoupledExport() {
    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  bool valid() const { return initialized_; }
  bool dimensions(int width, int height) const { return width == width_ && height == height_; }
  HANDLE handle() const { return outputHandle_; }
  int width() const { return width_; }
  int height() const { return height_; }

  // Called on the render thread. Copies `src` (owned by the render device) into a
  // free internal slot with a single keyed-mutex acquire/release that only couples
  // to this fast export device. Does not flush: the render context's end-of-pass
  // Flush submits the copy, exactly as D3DProgramBuffer::submit relies on. Drops
  // the frame (bounded, non-blocking) when every slot is still in flight.
  bool submit(ID3D11DeviceContext* producer, ID3D11Texture2D* src) {
    if (!initialized_ || !src) return false;
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
      queue_.push_back(slot);
    }
    changed_.notify_one();
    return true;
  }

 private:
  enum class State { Free, Writing, Submitted, Consuming };
  struct Slot {
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
    if (FAILED(producer->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi.put()))) ||
        FAILED(dxgi->GetAdapter(adapter.put())) ||
        FAILED(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            exportDevice_.put(), nullptr, exportContext_.put()))) {
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
    initialized_ = true;
    return true;
  }

  void exportLoop() {
    for (;;) {
      Slot* slot = nullptr;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return stopped_ || !queue_.empty(); });
        if (stopped_) return;
        slot = queue_.front();
        queue_.pop_front();
        slot->state = State::Consuming;
      }

      // Pull the slot into private staging. AcquireSync(1) returns S_OK on the CPU
      // as soon as the producer's ReleaseSync(1) handed over the key; the fence
      // wait for the producer copy is on THIS export context, never the render one.
      const bool pulled = slot->mutex->AcquireSync(1, 0) == S_OK;
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
      if (!pulled) continue;

      // Publish to the shell-facing output on the export timeline. Non-blocking:
      // if the consumer still holds the texture, skip (the shell keeps its last
      // frame) rather than stall. Reclaim our own released key when there is no
      // consumer at all, so the output never wedges on frame 1 (same rule as the
      // per-participant export).
      bool owned = outputMutex_->AcquireSync(0, 0) == S_OK;
      if (!owned) owned = outputMutex_->AcquireSync(1, 0) == S_OK;
      if (owned) {
        exportContext_->CopyResource(output_.get(), prepared_.get());
        outputMutex_->ReleaseSync(1);
        ++published_;
      } else {
        ++consumerBusy_;
      }
      exportContext_->Flush();
    }
  }

  int width_, height_;
  const char* label_;
  bool initialized_ = false, stopped_ = false;
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
  std::thread worker_;
  std::uint64_t published_ = 0, dropped_ = 0, producerBusy_ = 0, consumerBusy_ = 0;
};

}  // namespace corevideo::modules
