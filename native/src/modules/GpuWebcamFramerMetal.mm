// macOS OH Framer (roadmap M9.2 Task 9; docs/virtual-camera-macos-spec.md).
// The Metal implementation of GpuWebcamFramer. GpuWebcamFramer.cpp stays the
// Windows D3D11 / stub TU and compiles to nothing in this configuration; CMake
// adds this file only for Apple + COREVIDEO_WITH_MAC_VIRTUALCAM.
//
// Per-pixel work is the webcamFramer kernel (shaders/WebcamFramer.metal, a
// line-for-line port of WebcamFramer.hlsl). The CPU does two bulk memcpys per
// frame (upload, readback) on the camera worker, which is also where apply()
// waits for the command buffer — never the render thread.
#include "modules/GpuWebcamFramer.h"
#if defined(__APPLE__) && COREVIDEO_WITH_MAC_VIRTUALCAM
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include "WebcamFramerMetalSource.h"
#include "modules/WebcamFramerAppleDecode.h"

namespace corevideo::modules {
namespace {
[[noreturn]] void fail(const std::string& operation) {
  throw std::runtime_error("OH Framer: " + operation + " failed. Clean video continues.");
}
struct FramerDimensions {  // == the HLSL Dimensions cbuffer, 8 x 32-bit
  uint32_t width, height, words, mirror, meterShow, meterReady;
  int32_t meterTenths;
  uint32_t reserved;
};
static_assert(sizeof(FramerDimensions) == 32, "matches the HLSL Dimensions cbuffer");
}  // namespace

struct GpuWebcamFramer::Impl {
  std::string error;
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLComputePipelineState> pipeline = nil;
  id<MTLTexture> asset = nil;
  id<MTLBuffer> input = nil, output = nil;
  int width = 0, height = 0;
  std::shared_ptr<std::vector<uint8_t>> pixels;

  void initialize() {
    @autoreleasepool {
      id<MTLDevice> created = MTLCreateSystemDefaultDevice();
      if (!created) fail("GPU device creation (no Metal device)");
      id<MTLCommandQueue> createdQueue = [created newCommandQueue];
      if (!createdQueue) fail("GPU queue creation");
      MTLCompileOptions* options = [MTLCompileOptions new];
      // IEEE float, like fxc's output: the oracle values are exact bytes, and
      // fast-math reassociation is not a licence to drift by one.
      if (@available(macOS 15.0, *)) {
        options.mathMode = MTLMathModeSafe;
      } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
      }
      NSError* compileError = nil;
      id<MTLLibrary> library =
          [created newLibraryWithSource:[NSString stringWithUTF8String:kWebcamFramerMetalSource]
                                options:options
                                  error:&compileError];
      id<MTLFunction> function = library ? [library newFunctionWithName:@"webcamFramer"] : nil;
      if (!function) fail("GPU shader");
      id<MTLComputePipelineState> createdPipeline =
          [created newComputePipelineStateWithFunction:function error:&compileError];
      if (!createdPipeline) fail("GPU shader");
      std::vector<uint8_t> rgba;
      int w = 0, h = 0;
      if (const std::string failed = decodeWebcamFramerRgba(rgba, w, h); !failed.empty()) fail(failed);
      if (w != 1920 || h != 1080) throw std::runtime_error("OH Framer: invalid built-in dimensions.");
      MTLTextureDescriptor* td =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                             width:w height:h mipmapped:NO];
      td.usage = MTLTextureUsageShaderRead;
      td.storageMode = MTLStorageModeShared;
      id<MTLTexture> texture = [created newTextureWithDescriptor:td];
      if (!texture) fail("overlay texture");
      // Written once here and never again: immutable for the framer's lifetime.
      [texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba.data()
                 bytesPerRow:static_cast<NSUInteger>(w) * 4];
      device = created; queue = createdQueue; pipeline = createdPipeline; asset = texture;
    }
  }
  void resize(int w, int h) {
    if (width == w && height == h) return;
    input = nil; output = nil;
    const NSUInteger bytes = static_cast<NSUInteger>(w) * h * 3 / 2;
    input = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (!input) fail("input buffer");
    output = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (!output) fail("output buffer");
    width = w; height = h;
  }
};

GpuWebcamFramer::GpuWebcamFramer() : impl_(std::make_unique<Impl>()) {}
GpuWebcamFramer::~GpuWebcamFramer() = default;
const std::string& GpuWebcamFramer::warning() const { return impl_->error; }
bool GpuWebcamFramer::prepare(int w, int h) {
  if (!impl_->error.empty()) return false;
  try {
    if (w<2 || h<2 || w>7680 || h>4320 || (w&1) || (h&1) || (static_cast<size_t>(w)*h*3/2)%4)
      throw std::runtime_error("OH Framer: invalid camera dimensions. Clean video continues.");
    if (!impl_->device) impl_->initialize();
    impl_->resize(w, h);
    const auto size = static_cast<size_t>(w)*h*3/2;
    if (!impl_->pixels || impl_->pixels->size() != size) impl_->pixels = std::make_shared<std::vector<uint8_t>>(size);
    return true;
  } catch (const std::exception& e) { impl_->error = e.what(); return false; }
}
std::shared_ptr<const std::vector<uint8_t>> GpuWebcamFramer::apply(
    const std::shared_ptr<const std::vector<uint8_t>>& clean, int w, int h, bool mirror, WebcamLoudnessOverlay loudness) {
  if (!impl_->error.empty()) return {};
  if (!clean || w<2 || h<2 || w>7680 || h>4320 || (w&1) || (h&1)
      || (static_cast<size_t>(w)*h*3/2)%4 || clean->size() != static_cast<size_t>(w)*h*3/2) {
    impl_->error = "OH Framer: invalid NV12 frame. Clean video continues.";
    return {};
  }
  try {
    if (!prepare(w, h)) return {};
    auto& s = *impl_;
    std::memcpy(s.input.contents, clean->data(), clean->size());
    const bool ready = loudness.ready && std::isfinite(loudness.lufs);
    const int tenths = !ready ? -1200 : loudness.lufs <= -119 ? -1200 :
        static_cast<int>(std::round((std::max)(-99.9, (std::min)(99.9, loudness.lufs)) * 10));
    const FramerDimensions dims{static_cast<uint32_t>(w), static_cast<uint32_t>(h),
        static_cast<uint32_t>(clean->size() / 4), mirror ? 1u : 0u,
        loudness.show ? 1u : 0u, ready ? 1u : 0u, tenths, 0};
    @autoreleasepool {
      id<MTLCommandBuffer> commands = [s.queue commandBuffer];
      id<MTLComputeCommandEncoder> encoder = commands ? [commands computeCommandEncoder] : nil;
      if (!encoder) fail("GPU dispatch");
      [encoder setComputePipelineState:s.pipeline];
      [encoder setBuffer:s.input offset:0 atIndex:0];
      [encoder setBuffer:s.output offset:0 atIndex:1];
      [encoder setBytes:&dims length:sizeof(dims) atIndex:2];
      [encoder setTexture:s.asset atIndex:0];
      const NSUInteger group = std::min<NSUInteger>(64, s.pipeline.maxTotalThreadsPerThreadgroup);
      [encoder dispatchThreads:MTLSizeMake(dims.words, 1, 1) threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
      [encoder endEncoding];
      [commands commit];
      [commands waitUntilCompleted];  // camera worker only
      if (commands.status != MTLCommandBufferStatusCompleted) fail("GPU readback");
    }
    // Reuse storage only when no consumer retains the previous immutable result.
    if (!s.pixels || s.pixels.use_count() != 1 || s.pixels->size() != clean->size())
      s.pixels = std::make_shared<std::vector<uint8_t>>(clean->size());
    auto result = s.pixels;
    std::memcpy(result->data(), s.output.contents, result->size());
    return result;
  } catch (const std::exception& e) { impl_->error = e.what(); return {}; }
}
}  // namespace corevideo::modules
#endif
