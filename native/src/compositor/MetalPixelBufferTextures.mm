#include "compositor/MetalPixelBufferTextures.h"

#if COREVIDEO_WITH_METAL

#include "core/BoundedAsyncLog.h"
#include "modules/PixelBufferVideoFrame.h"

namespace corevideo::compositor {

namespace {
struct PassGate {
  std::atomic<bool> armed{false};
  std::mutex mutex;
  id<MTLSharedEvent> event = nil;  // guarded by mutex
  uint64_t value = 0;              // guarded by mutex
};
PassGate& passGate() {
  static PassGate gate;
  return gate;
}
}  // namespace

PixelBufferBinding::~PixelBufferBinding() {
  texture = nil;
  if (cvTexture) CFRelease(cvTexture);
}

MetalPixelBufferTextures::MetalPixelBufferTextures(id<MTLDevice> device) {
  if (device && CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device, nullptr, &cache_) != kCVReturnSuccess) {
    cache_ = nullptr;
    ::corevideo::core::nativeLogf("[compositor] CVMetalTextureCacheCreate failed; zero-copy media frames cannot be drawn\n");
  }
}

MetalPixelBufferTextures::~MetalPixelBufferTextures() {
  pass_.reset();
  if (cache_) CFRelease(cache_);
}

bool MetalPixelBufferTextures::canSample(const modules::VideoFrame& frame) {
  return frame.hasGpuPixels() && !frame.hasPixels() && !frame.hasI420() &&
         frame.gpuPixels->backend == modules::GpuVideoFrame::Backend::AppleIOSurfaceBgra;
}

std::shared_ptr<PixelBufferBinding> MetalPixelBufferTextures::bind(
    const modules::VideoFrame& frame, const std::shared_ptr<PixelBufferBinding>& current) {
  if (!cache_ || !canSample(frame)) return nullptr;
  std::shared_ptr<PixelBufferBinding> binding;
  if (current && current->frame == frame.gpuPixels && current->texture) {
    binding = current;  // a held frame drawn again: same surface, same texture
  } else {
    // backend AppleIOSurfaceBgra is only ever minted by makePixelBufferVideoFrame,
    // which guarantees an IOSurface-backed 32BGRA buffer.
    const auto* image = static_cast<const modules::PixelBufferVideoFrame*>(frame.gpuPixels.get());
    CVPixelBufferRef buffer = image->pixelBuffer();
    CVMetalTextureRef cvTexture = nullptr;
    const auto width = CVPixelBufferGetWidth(buffer), height = CVPixelBufferGetHeight(buffer);
    if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache_, buffer, nullptr,
                                                  MTLPixelFormatBGRA8Unorm, width, height, 0,
                                                  &cvTexture) != kCVReturnSuccess || !cvTexture) {
      return nullptr;
    }
    binding = std::make_shared<PixelBufferBinding>();
    binding->frame = frame.gpuPixels;
    binding->cvTexture = cvTexture;
    binding->texture = CVMetalTextureGetTexture(cvTexture);
    if (!binding->texture) return nullptr;
  }
  if (pass_) {
    std::lock_guard<std::mutex> lock(pass_->mutex);
    pass_->held.push_back(binding);
  }
  return binding;
}

void MetalPixelBufferTextures::beginPass(id<MTLCommandBuffer> commandBuffer) {
  auto pass = std::make_shared<PassLeases>();
  pass_ = pass;
  auto completed = completed_;
  [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
    // The GPU is done with every surface this pass sampled: let them go back
    // to their pools. Swapped out under the lock, destroyed outside it.
    std::vector<std::shared_ptr<PixelBufferBinding>> release;
    {
      std::lock_guard<std::mutex> lock(pass->mutex);
      release.swap(pass->held);
    }
    release.clear();
    completed->fetch_add(1);
  }];
  auto& gate = passGate();
  if (gate.armed.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> lock(gate.mutex);
    if (gate.event) [commandBuffer encodeWaitForEvent:gate.event value:gate.value];
  }
  if (cache_) CVMetalTextureCacheFlush(cache_, 0);
}

void MetalPixelBufferTextures::armPassGateForTest(id<MTLSharedEvent> event, uint64_t value) {
  auto& gate = passGate();
  std::lock_guard<std::mutex> lock(gate.mutex);
  gate.event = event;
  gate.value = value;
  gate.armed.store(event != nil);
}

void MetalPixelBufferTextures::disarmPassGateForTest() {
  auto& gate = passGate();
  std::lock_guard<std::mutex> lock(gate.mutex);
  gate.armed.store(false);
  gate.event = nil;
  gate.value = 0;
}

}  // namespace corevideo::compositor

#endif  // COREVIDEO_WITH_METAL
