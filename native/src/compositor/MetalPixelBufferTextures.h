#pragma once
// Zero-copy media frames into the Metal compositor (plan 2026-11-21 N4).
// Objective-C++ only: included by MetalCompositorAdapter.mm and the Metal tests.
//
// A media frame from the macOS zero-copy decoder carries no CPU pixels: its
// picture is the decoder's own IOSurface-backed CVPixelBuffer
// (modules/PixelBufferVideoFrame.h). This class wraps that surface as an
// MTLTexture through a CVMetalTextureCache - no replaceRegion, no CPU copy -
// and owns the one rule that makes it safe:
//
//   EVERY FRAME A PASS SAMPLES IS HELD BY THAT PASS UNTIL ITS COMMAND BUFFER
//   COMPLETES ON THE GPU.
//
// The compositor's per-source cache keeps "the current frame" for a source,
// but it replaces that entry on the render thread the moment the next frame
// arrives - while up to three earlier passes (the in-flight semaphore) may
// still be sampling the old surface. If the cache entry were the only holder,
// the old buffer would go back to the decoder's pool, the decoder would write
// the next picture into it, and an in-flight pass would show a recycled or
// torn frame. So bind() leases the binding (the frame's shared_ptr AND its
// CVMetalTexture) to the CURRENT pass, and the pass's completed handler
// releases the lease. A pass that samples sources must call beginPass() on its
// command buffer before encoding.
#import <Metal/Metal.h>
#include <CoreVideo/CoreVideo.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "modules/Interfaces.h"

namespace corevideo::compositor {

// One source's wrapped frame. Holding it holds the decoder's buffer.
struct PixelBufferBinding {
  std::shared_ptr<const modules::GpuVideoFrame> frame;
  CVMetalTextureRef cvTexture = nullptr;  // retained; released in the destructor
  id<MTLTexture> texture = nil;
  PixelBufferBinding() = default;
  PixelBufferBinding(const PixelBufferBinding&) = delete;
  PixelBufferBinding& operator=(const PixelBufferBinding&) = delete;
  ~PixelBufferBinding();
};

class MetalPixelBufferTextures {
 public:
  explicit MetalPixelBufferTextures(id<MTLDevice> device);
  ~MetalPixelBufferTextures();
  MetalPixelBufferTextures(const MetalPixelBufferTextures&) = delete;
  MetalPixelBufferTextures& operator=(const MetalPixelBufferTextures&) = delete;

  [[nodiscard]] bool available() const { return cache_ != nullptr; }

  // A frame whose only picture is an AppleIOSurfaceBgra image (no CPU payload).
  static bool canSample(const modules::VideoFrame& frame);

  // The binding to draw `frame` with: `current` when it already wraps the same
  // image (a held frame re-drawn), else a new wrap. Either way the returned
  // binding is leased to the current pass. nullptr = the surface could not be
  // wrapped (the layer then falls back to its slate, never to stale pixels).
  std::shared_ptr<PixelBufferBinding> bind(const modules::VideoFrame& frame,
                                           const std::shared_ptr<PixelBufferBinding>& current);

  // Starts a new lease bag for `commandBuffer` and registers the completed
  // handler that releases it. Also flushes the texture cache's stale entries.
  void beginPass(id<MTLCommandBuffer> commandBuffer);

  // Observation for tests: passes whose leases have been released.
  [[nodiscard]] uint64_t passesCompletedForTest() const { return completed_->load(); }

  // TEST SEAM (plan default P7, the CompositorFaultInjection.h shape): while
  // armed, every pass's command buffer waits on `event` reaching `value`
  // BEFORE any of its commands run, so a test can hold the GPU and prove a
  // sampled surface is not recycled meanwhile. No env var, command, config key
  // or wire field reaches it; disarmed it costs one relaxed atomic load per
  // pass.
  static void armPassGateForTest(id<MTLSharedEvent> event, uint64_t value);
  static void disarmPassGateForTest();

 private:
  struct PassLeases {
    std::mutex mutex;
    std::vector<std::shared_ptr<PixelBufferBinding>> held;
  };
  CVMetalTextureCacheRef cache_ = nullptr;
  std::shared_ptr<PassLeases> pass_;
  std::shared_ptr<std::atomic<uint64_t>> completed_ = std::make_shared<std::atomic<uint64_t>>(0);
};

}  // namespace corevideo::compositor
