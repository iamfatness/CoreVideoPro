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
// (GraphicsKeyPixels.h) and publishes the latest buffer for the output worker.
//
// Unlike the vcam tap there is no shader pass: the key leaves as BGRA, which
// is the format NDI delivers key+fill in, so the readback is ~8 MB of finished
// pixels at 1080p and the only CPU work is the per-pixel divide by alpha.
// Frames are latest-wins; a slow reader skips frames, never blocks the render.

#include "core/BoundedAsyncLog.h"
#include "core/FrameAllocation.h"
#include "compositor/ComPtrLite.h"
#include "modules/GraphicsKeyPixels.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace corevideo::modules {

class D3DGraphicsKeyTap {
 public:
  D3DGraphicsKeyTap() = default;
  ~D3DGraphicsKeyTap() { stop(); }
  D3DGraphicsKeyTap(const D3DGraphicsKeyTap&) = delete;
  D3DGraphicsKeyTap& operator=(const D3DGraphicsKeyTap&) = delete;

  // RENDER THREAD. Builds (or rebuilds on a size change) the shared texture on
  // the producer device, the reader device, its staging surface and the reader
  // thread. Returns false when the rig cannot be built; the failure is logged
  // once per rig so a TDR/driver refusal is visible and never spams.
  bool ensure(ID3D11Device* producer, int width, int height) {
    if (shared1_ && width_ == width && height_ == height) return true;
    stop();
    if (!producer || width <= 0 || height <= 0) return false;
    width_ = width;
    height_ = height;

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

    stop_.store(false, std::memory_order_release);
    newFrame_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { loop(); });
    return true;
  }

  // RENDER THREAD. GPU->GPU copy only; non-blocking. When the reader still
  // holds the texture this frame is skipped (the next one lands instead).
  void submit(ID3D11DeviceContext* producerContext, ID3D11Texture2D* keyTarget) {
    if (!producerContext || !keyTarget || !shared1_ || !mutex1_) return;
    if (mutex1_->AcquireSync(0, 0) != S_OK) {
      ++skippedSubmits_;
      return;
    }
    producerContext->CopyResource(shared1_.get(), keyTarget);
    mutex1_->ReleaseSync(1);
    newFrame_.store(true, std::memory_order_release);
    cv_.notify_one();
  }

  // OUTPUT WORKER. The newest straight-alpha BGRA frame since the last take.
  bool take(std::shared_ptr<const std::vector<uint8_t>>& out, int& width, int& height) {
    std::lock_guard<std::mutex> lock(latestMutex_);
    if (!latest_ || latestGen_ == takenGen_) return false;
    takenGen_ = latestGen_;
    out = latest_;
    width = latestW_;
    height = latestH_;
    return true;
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
    errorLogged_ = false;
  }

  [[nodiscard]] bool active() const { return thread_.joinable(); }
  [[nodiscard]] std::uint64_t framesPublished() const { return published_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t skippedSubmits() const { return skippedSubmits_; }

 private:
  bool fail(const char* stage) {
    ::corevideo::core::nativeLogf("[graphics-key-tap] rig build failed at %s (%dx%d)\n", stage, width_, height_);
    stop();
    return false;
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
      ID3D11DeviceContext* ctx = context2_.get();
      ctx->CopyResource(staging2_.get(), shared2_.get());
      // Release as soon as the copy is ISSUED (it queues in order on this
      // context) so the render thread can produce the next frame while we map.
      mutex2_->ReleaseSync(0);
      const int w = width_;
      const int h = height_;
      auto pixels = core::tryMakeFrameBuffer(graphicsKeyFrameBytes(w, h));
      if (!pixels) {
        allocationFailures.note(graphicsKeyFrameBytes(w, h));
        continue;
      }
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (FAILED(ctx->Map(staging2_.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        if (!errorLogged_) {
          ::corevideo::core::nativeLogf("[graphics-key-tap] staging Map failed; skipping frames\n");
          errorLogged_ = true;
        }
        continue;
      }
      unpremultiplyBgra(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, w, h, pixels->data());
      ctx->Unmap(staging2_.get(), 0);
      {
        std::lock_guard<std::mutex> lock(latestMutex_);
        latest_ = std::move(pixels);
        latestW_ = w;
        latestH_ = h;
        ++latestGen_;
      }
      published_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  int width_ = 0;
  int height_ = 0;
  ComPtrLite<ID3D11Texture2D> shared1_;
  ComPtrLite<IDXGIKeyedMutex> mutex1_;
  ComPtrLite<ID3D11Device> device2_;
  ComPtrLite<ID3D11DeviceContext> context2_;
  ComPtrLite<ID3D11Texture2D> shared2_;
  ComPtrLite<IDXGIKeyedMutex> mutex2_;
  ComPtrLite<ID3D11Texture2D> staging2_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> newFrame_{false};
  std::mutex cvMutex_;
  std::condition_variable cv_;
  std::mutex latestMutex_;
  std::shared_ptr<const std::vector<uint8_t>> latest_;
  int latestW_ = 0;
  int latestH_ = 0;
  std::uint64_t latestGen_ = 0;
  std::uint64_t takenGen_ = 0;
  std::atomic<std::uint64_t> published_{0};
  std::uint64_t skippedSubmits_ = 0;
  bool errorLogged_ = false;
};

}  // namespace corevideo::modules
