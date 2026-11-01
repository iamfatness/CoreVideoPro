#pragma once

// NDI graphics key: the off-render-thread readback of the graphics-only
// composite (D3D11Compositor::renderGraphicsKeyPass).
//
// Same shape as the virtual-camera tap, for the same reason: a GPU->CPU Map on
// the render context stalls the render thread until the GPU drains, which caps
// the program rate well below what the GPU can do. So the render thread does
// ONE cheap GPU->GPU CopyResource into a keyed-mutex shared texture and moves
// on; a second D3D device with its own immediate context (no multithread tax,
// no coreMutex) opens that texture, copies it into a staging surface, Maps it
// on its own thread, converts premultiplied -> straight alpha
// (GraphicsKeyPixels.h) and publishes the frame into a short ring tagged with
// the compositor frame number that produced it (GraphicsKeyRing), so a
// buffered Program can be matched to the key that was on air with it.
//
// No shader pass: the key leaves as BGRA, the format NDI delivers key+fill in.
// Output buffers come from a small recycled pool (a 4K key is 33 MB; a fresh
// allocation per frame at 60 Hz is ~2 GB/s of allocator churn). A slow reader
// skips frames; it never blocks the render.
//
// Failure discipline: a rig that cannot be built is retried for the same size
// only every GraphicsKeyRetryLatch::kRetryAfterMs, and each failing size is
// logged once — it must never rebuild a D3D device on the render thread every
// tick. The latch lives outside stop(), which only tears the rig down.

#include "core/BoundedAsyncLog.h"
#include "core/FrameAllocation.h"
#include "compositor/ComPtrLite.h"
#include "modules/GraphicsKeyPixels.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // keep std::min/std::max usable in every includer (tests use them)
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace corevideo::modules {

class D3DGraphicsKeyTap {
 public:
  using Clock = std::function<std::int64_t()>;
  // Ring entries + MediaCore's current frame + the sender's in-flight send +
  // one being filled, with a little slack.
  static constexpr std::size_t kPoolMax = GraphicsKeyRing::kCapacity + 4;

  static std::int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  explicit D3DGraphicsKeyTap(Clock clock = &D3DGraphicsKeyTap::steadyNowMs) : clock_(std::move(clock)) {
    ring_.arm(clock_());
  }
  ~D3DGraphicsKeyTap() { stop(); }
  D3DGraphicsKeyTap(const D3DGraphicsKeyTap&) = delete;
  D3DGraphicsKeyTap& operator=(const D3DGraphicsKeyTap&) = delete;

  // RENDER THREAD. Builds (or rebuilds on a size change) the shared texture on
  // the producer device, the reader device, its staging surface and the reader
  // thread. False when the rig is not available this tick: a failed size is
  // not re-attempted until the latch's retry interval has passed.
  bool ensure(ID3D11Device* producer, int width, int height) {
    if (shared1_ && width_ == width && height_ == height) return true;
    if (!latch_.shouldAttempt(width, height, clock_())) return false;
    ++buildAttempts_;
    stop();
    width_ = width;
    height_ = height;
    if (!producer || width <= 0 || height <= 0) return fail("producer device");

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(producer->CreateTexture2D(&desc, nullptr, shared1_.put()))) return fail("producer shared texture");
    ComPtrLite<IDXGIResource> dxgi;
    HANDLE handle = nullptr;
    if (FAILED(shared1_->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(dxgi.put()))) ||
        FAILED(dxgi->GetSharedHandle(&handle)) || handle == nullptr ||
        FAILED(shared1_->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(mutex1_.put())))) {
      return fail("producer shared handle");
    }

    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, device2_.put(), &level, context2_.put()))) {
      return fail("reader device");
    }
    if (FAILED(device2_->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(shared2_.put()))) ||
        FAILED(shared2_->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(mutex2_.put())))) {
      return fail("reader shared texture");
    }
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.MiscFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device2_->CreateTexture2D(&staging, nullptr, staging2_.put()))) return fail("reader staging");

    latch_.noteSuccess();
    mapErrorLogged_ = false;
    stop_.store(false, std::memory_order_release);
    newFrame_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { loop(); });
    return true;
  }

  // RENDER THREAD. GPU->GPU copy only; non-blocking. When the reader still
  // holds the texture this frame is skipped (the next one lands instead).
  void submit(ID3D11DeviceContext* producerContext, ID3D11Texture2D* keyTarget, std::int64_t frameNumber) {
    if (!producerContext || !keyTarget || !shared1_ || !mutex1_) return;
    if (mutex1_->AcquireSync(0, 0) != S_OK) {
      skippedSubmits_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    producerContext->CopyResource(shared1_.get(), keyTarget);
    // Written before the release: the reader can only acquire key 1 after it,
    // and the producer cannot write again until the reader releases key 0.
    pendingFrameNumber_.store(frameNumber, std::memory_order_release);
    mutex1_->ReleaseSync(1);
    newFrame_.store(true, std::memory_order_release);
    cv_.notify_one();
  }

  // OUTPUT WORKER (via the compositor): the key for one Program frame.
  bool select(std::int64_t programFrameNumber, std::int64_t nowMs, GraphicsKeySelection& out) const {
    return ring_.select(programFrameNumber, nowMs, out);
  }

  void stop() {
    stop_.store(true, std::memory_order_release);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    staging2_ = {};
    shared2_ = {};
    mutex2_ = {};
    context2_ = {};
    device2_ = {};
    mutex1_ = {};
    shared1_ = {};
    width_ = 0;
    height_ = 0;
  }

  [[nodiscard]] bool active() const { return thread_.joinable(); }
  [[nodiscard]] std::uint64_t framesPublished() const { return published_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t skippedSubmits() const { return skippedSubmits_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t buildAttempts() const { return buildAttempts_; }
  [[nodiscard]] std::uint64_t failureLogs() const { return failureLogs_; }

 private:
  bool fail(const char* stage) {
    const int w = width_, h = height_;
    stop();
    if (latch_.noteFailure(w, h, clock_())) {
      ++failureLogs_;
      ::corevideo::core::nativeLogf("[graphics-key-tap] rig build failed at %s (%dx%d); retrying every %llds\n",
          stage, w, h, static_cast<long long>(GraphicsKeyRetryLatch::kRetryAfterMs / 1000));
    }
    return false;
  }

  // Reader thread only. A pool entry is free when the pool holds its only
  // reference (the ring, MediaCore and the sender have all let go).
  core::SharedFrameBytes acquireBuffer(std::size_t bytes) {
    for (auto it = pool_.begin(); it != pool_.end();) {
      if (it->use_count() == 1 && (*it)->size() != bytes) it = pool_.erase(it);  // size changed
      else ++it;
    }
    for (auto& entry : pool_) {
      if (entry.use_count() == 1) return entry;
    }
    if (pool_.size() >= kPoolMax) return nullptr;  // every buffer is still held: skip this frame
    auto fresh = core::tryMakeFrameBuffer(bytes);
    if (fresh) pool_.push_back(fresh);
    return fresh;
  }

  void loop() {
    // Throughput, not latency: like the vcam tap, lose slices to the OS audio
    // engine before anything else does; a skipped key frame is invisible.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    static core::FrameAllocationFailures allocationFailures("graphics-key-tap");
    while (!stop_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lk(cvMutex_);
        cv_.wait_for(lk, std::chrono::milliseconds(16), [this] {
          return newFrame_.load(std::memory_order_acquire) || stop_.load(std::memory_order_acquire);
        });
        newFrame_.store(false, std::memory_order_release);
      }
      if (stop_.load(std::memory_order_acquire)) break;
      if (!mutex2_ || !shared2_ || !staging2_ || !context2_) continue;
      if (mutex2_->AcquireSync(1, 8) != S_OK) continue;  // producer still writing; try later
      const std::int64_t frameNumber = pendingFrameNumber_.load(std::memory_order_acquire);
      ID3D11DeviceContext* ctx = context2_.get();
      ctx->CopyResource(staging2_.get(), shared2_.get());
      // Release as soon as the copy is ISSUED (it queues in order on this
      // context) so the render thread can produce the next frame while we map.
      mutex2_->ReleaseSync(0);
      const int w = width_;
      const int h = height_;
      auto pixels = acquireBuffer(graphicsKeyFrameBytes(w, h));
      if (!pixels) {
        allocationFailures.note(graphicsKeyFrameBytes(w, h));
        continue;  // the ring keeps its last fresh frame; staleness covers a long gap
      }
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (FAILED(ctx->Map(staging2_.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        if (!mapErrorLogged_) {
          ::corevideo::core::nativeLogf("[graphics-key-tap] staging Map failed; skipping frames (key goes transparent after %lld ms)\n",
              static_cast<long long>(kGraphicsKeyStaleAfterMs));
          mapErrorLogged_ = true;
        }
        continue;
      }
      unpremultiplyBgra(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, w, h, pixels->data());
      ctx->Unmap(staging2_.get(), 0);
      GraphicsKeyFrame frame;
      frame.bgra = pixels;
      frame.width = w;
      frame.height = h;
      frame.frameNumber = frameNumber;
      frame.publishedMs = clock_();
      ring_.publish(std::move(frame));
      published_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  Clock clock_;
  GraphicsKeyRing ring_;
  GraphicsKeyRetryLatch latch_;   // survives stop(): the backoff must outlive a torn-down rig
  std::uint64_t buildAttempts_ = 0;
  std::uint64_t failureLogs_ = 0;
  int width_ = 0;
  int height_ = 0;
  ComPtrLite<ID3D11Texture2D> shared1_;
  ComPtrLite<IDXGIKeyedMutex> mutex1_;
  ComPtrLite<ID3D11Device> device2_;
  ComPtrLite<ID3D11DeviceContext> context2_;
  ComPtrLite<ID3D11Texture2D> shared2_;
  ComPtrLite<IDXGIKeyedMutex> mutex2_;
  ComPtrLite<ID3D11Texture2D> staging2_;
  std::vector<core::SharedFrameBytes> pool_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> newFrame_{false};
  std::atomic<std::int64_t> pendingFrameNumber_{-1};
  std::mutex cvMutex_;
  std::condition_variable cv_;
  std::atomic<std::uint64_t> published_{0};
  std::atomic<std::uint64_t> skippedSubmits_{0};
  bool mapErrorLogged_ = false;
};

}  // namespace corevideo::modules
