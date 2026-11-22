// The Metal grade-preview worker. Contract and threading: MetalGradePreview.h.
//
// Behaviour copied from the D3D11 grade worker, file:line on fc675541:
//  - one MonitorRenderWorker; the backend (device queue, pipelines, textures) is created on the
//    first non-empty request and released on an empty one (D3D11CompositorAdapter.cpp:223-238);
//  - per demand: no frame -> reason "source-unavailable" (:996-1001); status "ready", or "held"
//    when the input cache served the last frame (:1011-1014); the frame's epoch, id and capture
//    time are stamped on the observation (:1008-1010);
//  - the preview is the source drawn with the draft grade, or neutral for compareOriginal
//    (:1018-1027), through the SAME fragment functions Program uses (here: the shared
//    kMetalCompositorShaderSource), so a draft preview equals Program for the same grade;
//  - resources of an instance no longer demanded are dropped (:1083-1097).
// Mac differences (plan defaults): the worker waits on its own command buffer (P1); one stable
// IOSurface per editor (P2); preview size capped to 1920x1080 (P3); lowerThirdPreview answers
// native-appearance-preview-not-built (P5).

#include "compositor/MetalGradePreview.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL

#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

#include "compositor/AdvancedGrade.h"
#include "compositor/CompositorShaderParams.h"
#include "compositor/MetalCompositorShaders.h"
#include "core/BoundedAsyncLog.h"
#include "modules/MonitorRenderWorker.h"

namespace corevideo::modules {
namespace {

std::mutex& stallMutex() { static std::mutex m; return m; }
std::function<void()>& stallHook() { static std::function<void()> hook; return hook; }

// An IOSurface-backed BGRA render target shareable by global id (the shell's IOSurfaceLookup).
// kIOSurfaceIsGlobal is deprecated but REQUIRED for cross-process lookup by id
// (MetalCompositorAdapter.mm ensureTarget has the same note).
struct Surface {
  // The IOSurfaceRef is a CF object ARC does not manage; `owner` releases it when the last copy of
  // this Surface goes (an erased editor, a resize, the backend's release).
  std::shared_ptr<void> owner;
  IOSurfaceRef ref = nullptr;
  id<MTLTexture> texture = nil;
  uint32_t iosurfaceId = 0;
  int width = 0, height = 0;
  int64_t frames = 0;
  void release() {
    texture = nil;
    owner.reset(); ref = nullptr;
    iosurfaceId = 0; width = height = 0;
  }
  bool ensure(id<MTLDevice> device, int w, int h) {
    if (texture && width == w && height == h) return true;
    release();
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSDictionary* properties = @{
      (__bridge NSString*)kIOSurfaceWidth : @(w), (__bridge NSString*)kIOSurfaceHeight : @(h),
      (__bridge NSString*)kIOSurfaceBytesPerElement : @4,
      (__bridge NSString*)kIOSurfacePixelFormat : @((uint32_t)'BGRA'),
      (__bridge NSString*)kIOSurfaceIsGlobal : @YES,
    };
#pragma clang diagnostic pop
    ref = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (!ref) return false;
    owner = std::shared_ptr<void>(const_cast<void*>(static_cast<const void*>(ref)), [](void* p) { CFRelease(p); });
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:NSUInteger(w) height:NSUInteger(h) mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    texture = [device newTextureWithDescriptor:desc iosurface:ref plane:0];
    if (!texture) { release(); return false; }
    iosurfaceId = IOSurfaceGetID(ref); width = w; height = h;
    return true;
  }
};

// The worker's OWN copy of one demanded source's pixels (never the render thread's cache).
struct SourceUpload {
  id<MTLTexture> bgra = nil, y = nil, u = nil, v = nil;
  bool i420 = false;
  int width = 0, height = 0;
  int64_t frameId = -1;
  const void* bytes = nullptr;   // the immutable buffer last uploaded (held-frame dedup)
};

struct Instance {
  std::string sourceId;
  SourceUpload upload;
  Surface preview;
};

std::pair<int, int> sourceSize(const VideoFrame& frame) {
  if (frame.hasI420()) return {frame.i420Width, frame.i420Height};
  return {frame.pixelWidth > 0 ? frame.pixelWidth : frame.width, frame.pixelHeight > 0 ? frame.pixelHeight : frame.height};
}

// Fit within 1920x1080 keeping aspect (plan P3); never upscale.
std::pair<int, int> previewSize(int w, int h) {
  const double scale = std::min({1.0, double(MetalGradePreview::kMaxPreviewWidth) / std::max(1, w),
                                 double(MetalGradePreview::kMaxPreviewHeight) / std::max(1, h)});
  return {std::max(16, int(std::lround(w * scale))), std::max(16, int(std::lround(h * scale)))};
}

class Backend {
 public:
  explicit Backend(id<MTLDevice> device) : device_(device) {}

  void initialize() {
    if (!device_) throw std::runtime_error("no Metal device");
    queue_ = [device_ newCommandQueue];
    NSError* error = nil;
    id<MTLLibrary> library = [device_ newLibraryWithSource:[NSString stringWithUTF8String:kMetalCompositorShaderSource]
                                                   options:nil error:&error];
    if (!library) {
      // The compositor's own fallback: tiles effects off, everything else identical.
      NSString* clean = [@"#define COREVIDEO_DISABLE_TILES_EFFECT 1\n" stringByAppendingString:
          [NSString stringWithUTF8String:kMetalCompositorShaderSource]];
      library = [device_ newLibraryWithSource:clean options:nil error:&error];
    }
    if (!queue_ || !library) throw std::runtime_error("grade preview shader compile failed");
    id<MTLFunction> vertex = [library newFunctionWithName:@"compositorVertex"];
    textured_ = pipeline(library, vertex, @"compositorTextured");
    i420_ = pipeline(library, vertex, @"compositorI420");
    if (!textured_ || !i420_) throw std::runtime_error("grade preview pipeline creation failed");
    MTLSamplerDescriptor* sampler = [[MTLSamplerDescriptor alloc] init];
    sampler.minFilter = sampler.magFilter = MTLSamplerMinMagFilterLinear;
    sampler.sAddressMode = sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_ = [device_ newSamplerStateWithDescriptor:sampler];
  }

  MonitorRenderResult render(const MonitorRenderRequest& request, double& gpuMs) {
    MonitorRenderResult result;
    id<MTLCommandBuffer> commands = [queue_ commandBuffer];
    for (const auto& demand : request.gradePreviews) {
      GradePreviewSurface observation; observation.demand = demand;
      if (demand.lowerThirdPreview) {   // P5: the graphics workstream's canvas, not built on Metal
        observation.status = "unavailable"; observation.reason = "native-appearance-preview-not-built";
        result.gradePreviews.push_back(std::move(observation));
        continue;
      }
      const auto source = std::find_if(request.frames.begin(), request.frames.end(), [&](const VideoFrame& frame) {
        return frame.participantId == demand.sourceId;
      });
      if (source == request.frames.end() || !(source->hasPixels() || source->hasI420())) {
        observation.reason = "source-unavailable";
        result.gradePreviews.push_back(std::move(observation));
        continue;
      }
      auto& instance = instances_[demand.instanceId];
      if (instance.sourceId != demand.sourceId) { instance = Instance{}; instance.sourceId = demand.sourceId; }
      if (!upload(*source, instance.upload)) throw std::runtime_error("grade preview upload failed");
      observation.sourceEpoch = source->sourceEpoch;
      observation.sourceFrameId = source->frameId;
      observation.captureTimestamp100ns = source->captureTimestamp100ns;
      const bool held = std::find(request.unavailableInputs.begin(), request.unavailableInputs.end(),
          demand.sourceId) != request.unavailableInputs.end();
      observation.status = held ? "held" : "ready";
      observation.reason = held ? "source-unavailable" : "";

      const auto [w, h] = previewSize(instance.upload.width, instance.upload.height);
      if (!instance.preview.ensure(device_, w, h)) throw std::runtime_error("grade preview surface failed");
      draw(commands, instance.preview.texture, instance.upload, *source,
           demand.compareOriginal ? CompositorColorGrade{} : demand.grade);
      observation.texture.participantId = "grade:" + demand.instanceId;
      observation.texture.iosurfaceId = instance.preview.iosurfaceId;
      observation.texture.width = instance.preview.width;
      observation.texture.height = instance.preview.height;
      observation.texture.frameNumber = ++instance.preview.frames;
      observation.texture.sourceEpoch = source->sourceEpoch;
      result.gradePreviews.push_back(std::move(observation));
    }
    // Drop every instance no longer demanded (its surfaces and upload go with it).
    for (auto it = instances_.begin(); it != instances_.end();) {
      const bool demanded = std::any_of(request.gradePreviews.begin(), request.gradePreviews.end(),
          [&](const GradePreviewDemand& d) { return d.instanceId == it->first; });
      it = demanded ? std::next(it) : instances_.erase(it);
    }
    [commands commit];
    [commands waitUntilCompleted];   // the worker's OWN buffer on its OWN queue (P1)
    if (commands.status == MTLCommandBufferStatusError) throw std::runtime_error("grade preview GPU error");
    gpuMs = std::max(0.0, (commands.GPUEndTime - commands.GPUStartTime) * 1000.0);
    return result;
  }

 private:
  id<MTLRenderPipelineState> pipeline(id<MTLLibrary> library, id<MTLFunction> vertex, NSString* fragment) {
    id<MTLFunction> fn = [library newFunctionWithName:fragment];
    if (!fn || !vertex) return nil;
    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = vertex; desc.fragmentFunction = fn;
    auto* color = desc.colorAttachments[0];
    color.pixelFormat = MTLPixelFormatBGRA8Unorm;
    // Program's straight-alpha blend (MetalCompositorAdapter.mm makePipeline), over a black clear.
    color.blendingEnabled = YES;
    color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    NSError* error = nil;
    return [device_ newRenderPipelineStateWithDescriptor:desc error:&error];
  }

  id<MTLTexture> plane(MTLPixelFormat format, int w, int h) {
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
        width:NSUInteger(w) height:NSUInteger(h) mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead; desc.storageMode = MTLStorageModeShared;
    return [device_ newTextureWithDescriptor:desc];
  }
  static void fill(id<MTLTexture> texture, const uint8_t* bytes, int w, int h, int stride) {
    [texture replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(w), NSUInteger(h)) mipmapLevel:0
                 withBytes:bytes bytesPerRow:NSUInteger(stride)];
  }

  // Uploads only when the frame content changed (a held frame re-grades without a copy).
  bool upload(const VideoFrame& frame, SourceUpload& out) {
    const bool i420 = frame.hasI420();
    const auto [w, h] = sourceSize(frame);
    if (w <= 0 || h <= 0) return false;
    const void* bytes = i420 ? static_cast<const void*>(frame.i420.get()) : static_cast<const void*>(frame.pixels.get());
    if (out.i420 == i420 && out.width == w && out.height == h && out.frameId == frame.frameId && out.bytes == bytes &&
        (i420 ? out.y != nil : out.bgra != nil)) return true;
    if (out.i420 != i420 || out.width != w || out.height != h || (i420 ? out.y == nil : out.bgra == nil)) {
      out = SourceUpload{}; out.i420 = i420; out.width = w; out.height = h;
      if (i420) { out.y = plane(MTLPixelFormatR8Unorm, w, h); out.u = plane(MTLPixelFormatR8Unorm, w / 2, h / 2);
                  out.v = plane(MTLPixelFormatR8Unorm, w / 2, h / 2); }
      else out.bgra = plane(MTLPixelFormatBGRA8Unorm, w, h);
      if (i420 ? (!out.y || !out.u || !out.v) : !out.bgra) return false;
    }
    if (i420) {
      const uint8_t* y = frame.i420->data();
      const uint8_t* u = y + size_t(w) * size_t(h);
      const uint8_t* v = u + size_t(w / 2) * size_t(h / 2);
      fill(out.y, y, w, h, w); fill(out.u, u, w / 2, h / 2, w / 2); fill(out.v, v, w / 2, h / 2, w / 2);
    } else {
      fill(out.bgra, frame.pixels->data(), w, h, frame.pixelStride > 0 ? frame.pixelStride : w * 4);
    }
    out.frameId = frame.frameId; out.bytes = bytes;
    return true;
  }

  // One full-target draw of the source with `grade` (identity UV), the Program layer constants
  // for a full-canvas layer: white opaque, the grade, the frame's YUV parameters.
  void draw(id<MTLCommandBuffer> commands, id<MTLTexture> target, const SourceUpload& source,
            const VideoFrame& frame, const CompositorColorGrade& grade, const float uvOffset[2] = nullptr,
            const float uvScale[2] = nullptr) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    LayerShaderConstants constants{};
    constants.color[0] = constants.color[1] = constants.color[2] = constants.color[3] = 1.f;
    applyColorGradeParams(&constants, grade);
    constants.uvScale[0] = uvScale ? uvScale[0] : 1.f; constants.uvScale[1] = uvScale ? uvScale[1] : 1.f;
    constants.uvOffset[0] = uvOffset ? uvOffset[0] : 0.f; constants.uvOffset[1] = uvOffset ? uvOffset[1] : 0.f;
    applyYuvParams(&constants, yuvShaderParamsForFrame(&frame));
    const auto advanced = advancedGradeConstants(grade);
    [encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
    [encoder setFragmentBytes:&advanced length:sizeof(advanced) atIndex:1];
    [encoder setFragmentTexture:curveTexture(grade) atIndex:3];
    for (size_t i = 0; i < 8; ++i) [encoder setFragmentTexture:cubeTexture(grade, i) atIndex:4 + i];
    [encoder setFragmentSamplerState:sampler_ atIndex:0];
    if (source.i420) {
      [encoder setRenderPipelineState:i420_];
      [encoder setFragmentTexture:source.y atIndex:0];
      [encoder setFragmentTexture:source.u atIndex:1];
      [encoder setFragmentTexture:source.v atIndex:2];
    } else {
      [encoder setRenderPipelineState:textured_];
      [encoder setFragmentTexture:source.bgra atIndex:0];
    }
    MTLViewport viewport{0, 0, double(target.width), double(target.height), 0, 1};
    [encoder setViewport:viewport];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
  }

  // The worker's own copies of the curve / LUT textures (MetalCompositorAdapter.mm:1624-1656 build
  // the render thread's; sharing them would put two threads on one cache).
  id<MTLTexture> curveTexture(const CompositorColorGrade& grade) {
    if (!grade.advanced || grade.advanced->bypass || grade.advanced->operations.empty()) return nil;
    const auto& key = grade.advanced->content;
    if (const auto found = curves_.find(key); found != curves_.end()) return found->second;
    const auto samples = compileGradeCurves(grade);
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG32Float
        width:16 height:32 mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead; desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:desc];
    if (!texture) return nil;
    [texture replaceRegion:MTLRegionMake2D(0, 0, 16, 32) mipmapLevel:0 withBytes:samples.data() bytesPerRow:16 * 2 * sizeof(float)];
    if (curves_.size() >= 16) curves_.erase(curves_.begin());
    curves_.emplace(key, texture);
    return texture;
  }
  id<MTLTexture> cubeTexture(const CompositorColorGrade& grade, size_t index) {
    if (!grade.advanced || grade.advanced->bypass || index >= grade.advanced->operations.size()) return nil;
    const auto& cube = grade.advanced->operations[index].cube;
    if (!cube) return nil;
    if (const auto found = cubes_.find(cube->hash); found != cubes_.end()) return found->second;
    MTLTextureDescriptor* desc = [MTLTextureDescriptor new];
    desc.textureType = MTLTextureType3D; desc.pixelFormat = MTLPixelFormatRGBA32Float;
    desc.width = desc.height = desc.depth = NSUInteger(cube->size); desc.mipmapLevelCount = 1;
    desc.usage = MTLTextureUsageShaderRead; desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:desc];
    if (!texture) return nil;
    [texture replaceRegion:MTLRegionMake3D(0, 0, 0, cube->size, cube->size, cube->size) mipmapLevel:0 slice:0
                 withBytes:cube->rgba.data() bytesPerRow:cube->size * 16 bytesPerImage:cube->size * cube->size * 16];
    if (cubes_.size() >= 16) cubes_.erase(cubes_.begin());
    cubes_.emplace(cube->hash, texture);
    return texture;
  }

  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLRenderPipelineState> textured_ = nil, i420_ = nil;
  id<MTLSamplerState> sampler_ = nil;
  std::map<std::string, Instance> instances_;
  std::map<std::string, id<MTLTexture>> curves_, cubes_;
};

class MetalGradePreviewImpl final : public MetalGradePreview {
 public:
  MetalGradePreviewImpl() {
    auto backend = std::make_shared<std::unique_ptr<Backend>>();
    auto gpuMs = gpuMs_;
    auto overBudget = overBudget_;
    worker_ = std::make_unique<MonitorRenderWorker>([backend, gpuMs, overBudget](const MonitorRenderRequest& request) {
      {
        std::function<void()> stall;
        { std::lock_guard<std::mutex> lock(stallMutex()); stall = stallHook(); }
        if (stall) stall();
      }
      // A raw std::thread has no autorelease pool: without this, every job's autoreleased command
      // buffer (which retains its attachments) and descriptors would leak for the life of the
      // process, and a closed editor's IOSurface would never be freed.
      @autoreleasepool {
        if (request.gradePreviews.empty()) {
          backend->reset();   // demand ended: release device objects on their owner thread
          return MonitorRenderResult{};
        }
        if (!*backend) {
          auto created = std::make_unique<Backend>(MTLCreateSystemDefaultDevice());
          created->initialize();
          *backend = std::move(created);
        }
        double ms = 0;
        auto result = (*backend)->render(request, ms);
        gpuMs->store(ms, std::memory_order_relaxed);
        if (ms > kGpuBudgetMs) overBudget->store(true, std::memory_order_relaxed);
        return result;
      }
    });
  }
  ~MetalGradePreviewImpl() override { worker_.reset(); }

  void submit(MonitorRenderRequest request) override {
    if (request.gradePreviews.empty()) { worker_->submit(std::move(request)); return; }
    const int divisor = divisor_.load(std::memory_order_relaxed);
    const uint64_t n = ++offered_;
    if (divisor > 1 && n % uint64_t(divisor) != 0) { shed_.fetch_add(1, std::memory_order_relaxed); return; }
    if (overBudget_->exchange(false, std::memory_order_relaxed)) { shed_.fetch_add(1, std::memory_order_relaxed); return; }
    worker_->submit(std::move(request));
  }
  std::shared_ptr<const MonitorRenderResult> latest() const override { return worker_->latest(); }
  MonitorRenderDiagnostics diagnostics() const override { return worker_->diagnostics(); }
  void setMonitorShedDivisor(int divisor) override {
    divisor = std::clamp(divisor, 1, 8);
    if (divisor_.exchange(divisor, std::memory_order_relaxed) != divisor)
      ::corevideo::core::nativeLogf("[grade-preview] monitor shed divisor=%d: private previews and scopes forward 1 of %d requests\n",
          divisor, divisor);
  }
  uint64_t shedCount() const override { return shed_.load(std::memory_order_relaxed); }
  double lastGpuMs() const override { return gpuMs_->load(std::memory_order_relaxed); }
  void setLastGpuMsForTest(double ms) override {
    gpuMs_->store(ms, std::memory_order_relaxed);
    overBudget_->store(ms > kGpuBudgetMs, std::memory_order_relaxed);
  }

 private:
  std::shared_ptr<std::atomic<double>> gpuMs_ = std::make_shared<std::atomic<double>>(0.0);
  std::shared_ptr<std::atomic<bool>> overBudget_ = std::make_shared<std::atomic<bool>>(false);
  std::atomic<int> divisor_{1};
  std::atomic<uint64_t> offered_{0}, shed_{0};
  std::unique_ptr<MonitorRenderWorker> worker_;
};

}  // namespace

std::unique_ptr<MetalGradePreview> MetalGradePreview::create() { return std::make_unique<MetalGradePreviewImpl>(); }

void MetalGradePreview::setRenderStallForTest(std::function<void()> stall) {
  std::lock_guard<std::mutex> lock(stallMutex());
  stallHook() = std::move(stall);
}

}  // namespace corevideo::modules

#else  // gate

namespace corevideo::modules {
std::unique_ptr<MetalGradePreview> MetalGradePreview::create() { return nullptr; }
void MetalGradePreview::setRenderStallForTest(std::function<void()>) {}
}  // namespace corevideo::modules

#endif
