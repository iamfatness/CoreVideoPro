#include "core/BoundedAsyncLog.h"
#include "compositor/TilesDecorationParams.h"
// Metal GPU compositor adapter — the macOS twin of D3D11CompositorAdapter.
//
// Mirrors the D3D11 adapter's structure deliberately (resolveLayers ->
// drawLayer per layer -> readbacks) so the two stay reviewable side by side;
// the portable geometry/raster helpers (CompositorLayout.h, OverlayTileRaster,
// ProgramFramePreview) are shared, not duplicated. M1 scope (see
// docs/mac-port-phase3-metal-compositor.md): program render() at full layer
// parity. renderMultiview/renderPreview/takeVcamNv12 keep their interface
// defaults until M2/M3.
//
// Differences from the D3D11 adapter, all deliberate:
//  - Overlay layers composite CoreTextOverlayRaster's prepared texture (the
//    CompositorOverlayRaster twin, Metal graphics backend G1): rastered with
//    Core Text on a preparation worker, premultiplied, uploaded once, cached
//    by content. The render thread never rasters; a cold overlay draws the
//    Windows fallback (band + accent bar) until its texture is published.
//  - The render target uses storageModeShared (Apple-silicon unified memory),
//    so the CPU readbacks are a plain getBytes after waitUntilCompleted — no
//    staging-texture dance. fullProgramReadback fills ProgramFrame::
//    programFullBgra (the recording/encoder feed). The virtual camera has its
//    own GPU NV12 tap under COREVIDEO_WITH_MAC_VIRTUALCAM (vcamNv12Tap).
//  - programPixelSignature hashes the preview pixels (the stub's semantics)
//    rather than a center-pixel staging read.

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL

#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

#include "compositor/CompositorFaultInjection.h"
#include "compositor/CompositorLayout.h"
#include "compositor/CompositorShaderParams.h"
#include "compositor/AdvancedGrade.h"
#include "compositor/MetalCompositorShaders.h"
#include "modules/Interfaces.h"
#include "compositor/CoreTextOverlayRaster.h"
#include "compositor/OverlayRasterRetention.h"
#include "modules/OverlayTileRaster.h"
#include "modules/ProgramFramePreview.h"

namespace corevideo::modules {
namespace {

bool frameHasContent(const VideoFrame& frame) {
  return frame.hasPixels() || frame.hasI420();
}

uint32_t previewPixelSignature(const ProgramFramePreviewPixels& preview) {
  uint32_t hash = 2166136261u;  // FNV-1a 32
  for (const uint8_t byte : preview.bgra) {
    hash ^= byte;
    hash *= 16777619u;
  }
  return hash;
}

class MetalCompositor final : public ICompositor {
 public:
  ~MetalCompositor() override {
#if COREVIDEO_WITH_MAC_VIRTUALCAM
    stopVcamTap();
#endif
    tapStop_.store(true, std::memory_order_release);
    if (tapWake_) {
      dispatch_semaphore_signal(tapWake_);
    }
    if (tapThread_.joinable()) {
      tapThread_.join();
    }
    program_.release();
    multiview_.release();
    preview_.release();
  }

  std::string rendererName() const override { return "metal"; }

  ProgramFrame render(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) override {
    ++frameNumber_;
    retainedOverlayRasters_.prune(static_cast<uint64_t>(frameNumber_));
    const auto deterministicPlan = sortCompositorRenderPlan(renderPlan);
    ProgramFrame frame;
    frame.width = deterministicPlan.width;
    frame.height = deterministicPlan.height;
    frame.layerCount = deterministicPlan.layers.empty() ? static_cast<int>(frames.size())
                                                        : static_cast<int>(deterministicPlan.layers.size());
    frame.frameNumber = frameNumber_;
    frame.renderPlanId = deterministicPlan.renderPlanId;
    frame.renderer = "metal";
    frame.health = deterministicPlan.warnings.empty() ? "live" : "degraded";
    frame.warnings = deterministicPlan.warnings;
    frame.renderPlanSignature = renderPlanSignature(deterministicPlan);

    if (!ensurePipeline()) {
      frame.health = "degraded";
      frame.warnings.push_back("Metal compositor unavailable: " + pipelineError_);
      return frame;
    }
    if (!ensureTarget(program_, deterministicPlan.width, deterministicPlan.height)) {
      frame.health = "degraded";
      return frame;
    }
    targetWidth_ = program_.width;
    targetHeight_ = program_.height;
    // The LIGHT display tick (skipCpuReadback) does NO readbacks and NO GPU
    // wait — an 8MB full-res readback under coreMutex at 60fps held the lock
    // at ~86%+ duty and starved every RPC request (sampled root cause of the
    // shell's request timeouts). fullProgramReadback is honored on the
    // throttled FULL renders only, which is also what feeds the encoder.
    const bool fullTick = !renderPlan.skipCpuReadback;
    overlayRaster_.beginFrame();
    renderPassInto(program_, deterministicPlan, frames, /*waitForCompletion=*/fullTick);
    // Same frame warnings as D3D11CompositorAdapter::render: an overlay still
    // preparing (or failed) is named on the Program frame and degrades health.
    if (overlayRaster_.pendingThisFrame()) frame.warnings.push_back("graphics-preparing");
    if (overlayRaster_.failedThisFrame()) frame.warnings.push_back("graphics-preparation-failed");
    if (!frame.warnings.empty()) frame.health = "degraded";
    overlayRaster_.beginFrame();
#if COREVIDEO_WITH_MAC_VIRTUALCAM
    // EVERY tick, light ticks included: the camera is a 60 fps per-frame
    // output, and the light display tick IS the 60 Hz tick. No CPU wait here.
    if (renderPlan.vcamNv12Tap) {
      dispatchVcamTap();
    }
#endif

    frame.gpuComposed = true;
    frame.sharedTexture.iosurfaceId = program_.iosurfaceId;
    frame.sharedTexture.width = program_.width;
    frame.sharedTexture.height = program_.height;
    frame.sharedTexture.frameNumber = frame.frameNumber;
    // While an output/encoder session keeps every tick "full", cap the CPU
    // cost: readbacks every 2nd call (a 30fps encoder feed from a 60fps
    // render) and MOVE the full-res buffer instead of copying it. The
    // remaining alloc churn is bounded; the proper GPU-tap analogue is the
    // follow-up perf pass (docs/mac-port-phase4-swiftui-shell.md).
    // GPU→CPU readbacks from the full-res IOSurface target cost ~20ms for
    // 8MB (uncached memory — the repo's "minimize readback bytes" law; this
    // saturated coreMutex whenever the encoder was armed). The GPU downscales
    // program → 320x180 and the CPU reads 57KB. programFullBgra is
    // deliberately NOT filled: the encoder falls back to the preview exactly
    // like the Windows sink today; the full-res feed returns with the
    // GPU-tap analogue (documented follow-up in the Phase 4 spec).
    if (fullTick) {
      readPreviewViaGpuDownscale(frame.preview);
      frame.programPixelSignature = previewPixelSignature(frame.preview);
      if (renderPlan.fullProgramReadback) {
        harvestAndKickFullResTap(frame);
      }
    }

    // Evict cached source textures no pass has sampled recently (participant
    // left / source unrouted). Same 300-program-frame policy as D3D11.
    constexpr int64_t kSourceTexEvictAfterFrames = 300;
    for (auto it = sourceTextures_.begin(); it != sourceTextures_.end();) {
      it = (frameNumber_ - it->second.lastUsedFrame > kSourceTexEvictAfterFrames)
               ? sourceTextures_.erase(it)
               : std::next(it);
    }
    return frame;
  }

  CompositorSourceTexStats sourceTexStats() const override { return stats_; }

  bool wantsFullProgramReadbackForRecording() const override { return true; }

#if COREVIDEO_WITH_MAC_VIRTUALCAM
  // ── macOS virtual camera: GPU NV12 tap (roadmap M9.2) ──────────────────────
  // The sink runs on the vcam tap thread holding NO compositor lock. Setting or
  // clearing it waits for any in-flight call (MediaCore's teardown barrier).
  void setVcamFrameSink(VcamFrameSink sink) override {
    std::lock_guard<std::mutex> lock(vcamSinkMutex_);
    vcamSink_ = std::move(sink);
    identifiedVcamSink_ = {};
    vcamSinkPresent_.store(static_cast<bool>(vcamSink_), std::memory_order_release);
  }
  void setIdentifiedVcamFrameSink(IdentifiedVcamFrameSink sink) override {
    std::lock_guard<std::mutex> lock(vcamSinkMutex_);
    vcamSink_ = {};
    identifiedVcamSink_ = std::move(sink);
    vcamSinkPresent_.store(static_cast<bool>(identifiedVcamSink_), std::memory_order_release);
  }
  [[nodiscard]] bool publishesVcamFrames() const override { return true; }
  void setVcamMirror(bool mirror) override { vcamMirror_.store(mirror, std::memory_order_release); }
  [[nodiscard]] uint64_t vcamDispatchSkipped() const override {
    return vcamShared_->skipped.load(std::memory_order_acquire);
  }
  // Carried fix (Task 7): a failed preparation is permanent for this
  // compositor (the camera can never produce a frame), so it is reported, not
  // just logged once. MediaCore folds it into the virtualCamera snapshot.
  [[nodiscard]] std::string vcamTapWarning() const override {
    if (vcamPrepState_.load(std::memory_order_acquire) != kVcamPrepFailed) return {};
    return "CAMERA_TAP_PREP_FAILED: the GPU camera tap could not be prepared; restart CoreVideo Pro.";
  }
#endif

  // Second compositor pass: the whole multiview grid into its own
  // IOSurface-backed texture (the OBS/broadcast-multiviewer model). Mirrors
  // the D3D11 renderMultiview: saves/restores the program pass's canvas dims
  // so the draw helpers and the next render() are unaffected.
  ProgramFrameSharedTexture renderMultiview(const CompositorRenderPlan& renderPlan,
                                            const std::vector<VideoFrame>& frames) override {
    return renderSecondaryPass(multiview_, renderPlan, frames);
  }

  // Third compositor pass: the PREVIEW scene into its own IOSurface-backed
  // texture, identical machinery to renderMultiview.
  ProgramFrameSharedTexture renderPreview(const CompositorRenderPlan& renderPlan,
                                          const std::vector<VideoFrame>& frames) override {
    return renderSecondaryPass(preview_, renderPlan, frames);
  }

 private:
  struct ResolvedLayer {
    CompositorRenderPlanLayer plan;
    uint32_t color = 0xff444444;
    const VideoFrame* frame = nullptr;
  };

  struct SourceTex {
    id<MTLTexture> bgra;
    id<MTLTexture> y;
    id<MTLTexture> u;
    id<MTLTexture> v;
    int64_t frameId = -1;
    bool isI420 = false;
    int width = 0;
    int height = 0;
    int64_t lastUsedFrame = 0;
  };

  // A render pass target backed by an IOSurface, so the composited result is
  // shareable cross-process by global ID (the macOS sibling of the keyed-mutex
  // DXGI shared texture; the shell resolves it with IOSurfaceLookup). The
  // IOSurfaceRef is a CF object ARC does not manage — released explicitly on
  // recreate and in the destructor.
  struct PassTarget {
    id<MTLTexture> texture;
    IOSurfaceRef iosurface = nullptr;
    uint32_t iosurfaceId = 0;
    int width = 0;
    int height = 0;

    void release() {
      texture = nil;
      if (iosurface) {
        CFRelease(iosurface);
        iosurface = nullptr;
      }
      iosurfaceId = 0;
      width = 0;
      height = 0;
    }
  };

  // ── pipeline ───────────────────────────────────────────────────────────────

  bool ensurePipeline() {
    if (pipelineReady_) {
      return true;
    }
    if (pipelineFailed_) {
      return false;
    }
    device_ = MTLCreateSystemDefaultDevice();
    if (!device_) {
      return fail("no Metal device");
    }
    queue_ = [device_ newCommandQueue];
    NSError* error = nil;
    id<MTLLibrary> library =
        [device_ newLibraryWithSource:[NSString stringWithUTF8String:kMetalCompositorShaderSource]
                              options:nil
                                error:&error];
    if (!library) {
      tilesEffectsAvailable_ = false;
      NSString* clean = [@"#define COREVIDEO_DISABLE_TILES_EFFECT 1\n" stringByAppendingString:
          [NSString stringWithUTF8String:kMetalCompositorShaderSource]];
      library = [device_ newLibraryWithSource:clean options:nil error:&error];
      if (library) ::corevideo::core::nativeLogf("[tiles] shader effect unavailable; rendering clean tiles.\n");
    }
    if (!library) {
      return fail(std::string("shader compile failed: ") +
                  (error ? error.localizedDescription.UTF8String : "no diagnostics"));
    }
    id<MTLFunction> vertexFn = [library newFunctionWithName:@"compositorVertex"];
    if (!vertexFn) {
      return fail("missing compositorVertex");
    }
    solidPipeline_ = makePipeline(library, vertexFn, @"compositorSolid", /*premultiplied=*/false);
    texturedPipeline_ = makePipeline(library, vertexFn, @"compositorTextured", false);
    i420Pipeline_ = makePipeline(library, vertexFn, @"compositorI420", false);
    overlayPipeline_ = makePipeline(library, vertexFn, @"compositorOverlay", /*premultiplied=*/true);
    if (!solidPipeline_ || !texturedPipeline_ || !i420Pipeline_ || !overlayPipeline_) {
      return fail(pipelineError_.empty() ? "pipeline creation failed" : pipelineError_);
    }
    MTLSamplerDescriptor* samplerDesc = [[MTLSamplerDescriptor alloc] init];
    samplerDesc.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDesc.magFilter = MTLSamplerMinMagFilterLinear;
    samplerDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_ = [device_ newSamplerStateWithDescriptor:samplerDesc];
    pipelineReady_ = true;
    return true;
  }

  bool fail(const std::string& why) {
    pipelineFailed_ = true;
    pipelineError_ = why;
    ::corevideo::core::nativeLogf("[compositor] Metal pipeline unavailable: %s\n", why.c_str());
    return false;
  }

  id<MTLRenderPipelineState> makePipeline(id<MTLLibrary> library,
                                          id<MTLFunction> vertexFn,
                                          NSString* fragmentName,
                                          bool premultiplied) {
    id<MTLFunction> fragmentFn = [library newFunctionWithName:fragmentName];
    if (!fragmentFn) {
      pipelineError_ = std::string("missing fragment fn ") + fragmentName.UTF8String;
      return nil;
    }
    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = vertexFn;
    desc.fragmentFunction = fragmentFn;
    auto* color = desc.colorAttachments[0];
    color.pixelFormat = MTLPixelFormatBGRA8Unorm;
    color.blendingEnabled = YES;
    // Straight alpha (SrcAlpha, 1-SrcAlpha) for content; premultiplied
    // (One, 1-SrcAlpha) for the overlay raster — the same two blend states the
    // D3D11 adapter keeps.
    color.sourceRGBBlendFactor = premultiplied ? MTLBlendFactorOne : MTLBlendFactorSourceAlpha;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    NSError* error = nil;
    id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!state && error) {
      pipelineError_ = error.localizedDescription.UTF8String;
    }
    return state;
  }

  bool ensureTarget(PassTarget& target, int width, int height) {
    width = std::max(16, width);
    height = std::max(16, height);
    if (target.texture && target.width == width && target.height == height) {
      return true;
    }
    target.release();
    // kIOSurfaceIsGlobal is deprecated but REQUIRED for the shell's
    // IOSurfaceLookup-by-ID: without it the lookup fails cross-process and
    // every monitor renders black while the core composites perfectly.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSDictionary* properties = @{
      (__bridge NSString*)kIOSurfaceWidth : @(width),
      (__bridge NSString*)kIOSurfaceHeight : @(height),
      (__bridge NSString*)kIOSurfaceBytesPerElement : @4,
      (__bridge NSString*)kIOSurfacePixelFormat : @((uint32_t)'BGRA'),
      (__bridge NSString*)kIOSurfaceIsGlobal : @YES,
    };
#pragma clang diagnostic pop
    target.iosurface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (!target.iosurface) {
      return false;
    }
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:static_cast<NSUInteger>(width)
                                                          height:static_cast<NSUInteger>(height)
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    // IOSurface-backed textures live in unified/shared memory: getBytes
    // readbacks stay cheap, and the surface is shareable by global ID.
    desc.storageMode = MTLStorageModeShared;
    target.texture = [device_ newTextureWithDescriptor:desc
                                             iosurface:target.iosurface
                                                 plane:0];
    if (!target.texture) {
      target.release();
      return false;
    }
    target.iosurfaceId = IOSurfaceGetID(target.iosurface);
    target.width = width;
    target.height = height;
    return true;
  }

  // Program → small BGRA target (plain shared storage, not IOSurface) with
  // the textured pipeline at identity grade; CPU reads ~57KB.
  void readPreviewViaGpuDownscale(ProgramFramePreviewPixels& preview) {
    int previewWidth = 0;
    int previewHeight = 0;
    computeProgramFramePreviewSize(targetWidth_, targetHeight_, previewWidth, previewHeight);
    if (previewWidth <= 0 || previewHeight <= 0) {
      return;
    }
    if (!previewScaleTex_ || previewScaleWidth_ != previewWidth ||
        previewScaleHeight_ != previewHeight) {
      MTLTextureDescriptor* desc = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                       width:static_cast<NSUInteger>(previewWidth)
                                      height:static_cast<NSUInteger>(previewHeight)
                                   mipmapped:NO];
      desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      desc.storageMode = MTLStorageModeShared;
      previewScaleTex_ = [device_ newTextureWithDescriptor:desc];
      previewScaleWidth_ = previewWidth;
      previewScaleHeight_ = previewHeight;
    }
    if (!previewScaleTex_) {
      return;
    }
    @autoreleasepool {
      id<MTLCommandBuffer> commandBuffer = [queue_ commandBuffer];
      MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = previewScaleTex_;
      pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLRenderCommandEncoder> encoder =
          [commandBuffer renderCommandEncoderWithDescriptor:pass];
      LayerShaderConstants constants{};
      constants.color[0] = constants.color[1] = constants.color[2] = constants.color[3] = 1.f;
      constants.uvScale[0] = constants.uvScale[1] = 1.f;
      [encoder setRenderPipelineState:texturedPipeline_];
      [encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
      [encoder setFragmentTexture:program_.texture atIndex:0];
      [encoder setFragmentSamplerState:sampler_ atIndex:0];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [encoder endEncoding];
      [commandBuffer commit];
      [commandBuffer waitUntilCompleted];
    }
    preview.width = previewScaleWidth_;
    preview.height = previewScaleHeight_;
    preview.bgra.resize(static_cast<size_t>(previewScaleWidth_) * previewScaleHeight_ * 4u);
    [previewScaleTex_ getBytes:preview.bgra.data()
                   bytesPerRow:static_cast<NSUInteger>(previewScaleWidth_) * 4
                    fromRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(previewScaleWidth_),
                                               static_cast<NSUInteger>(previewScaleHeight_))
                   mipmapLevel:0];
  }

  // The GPU-tap analogue (Windows vcamTapLoop economics, Metal-shaped):
  // the render tick ONLY kicks a GPU blit program->tap texture; a dedicated
  // tap THREAD does the CPU readback off every core lock and publishes the
  // finished buffer; the next full tick SWAPS it into programFullBgra
  // (vector swap — zero copies under coreMutex). One frame of latency,
  // no stalls. (First attempt did getBytes inline under coreMutex and
  // starved requests again — 8MB CPU reads never belong on the core lock.)
  void harvestAndKickFullResTap(ProgramFrame& frame) {
    if (!tapTex_ || tapWidth_ != targetWidth_ || tapHeight_ != targetHeight_) {
      MTLTextureDescriptor* desc = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                       width:static_cast<NSUInteger>(targetWidth_)
                                      height:static_cast<NSUInteger>(targetHeight_)
                                   mipmapped:NO];
      desc.usage = MTLTextureUsageShaderRead;
      desc.storageMode = MTLStorageModeShared;
      tapTex_ = [device_ newTextureWithDescriptor:desc];
      tapWidth_ = targetWidth_;
      tapHeight_ = targetHeight_;
    }
    if (!tapTex_) {
      return;
    }
    ensureTapThread();
    {
      std::lock_guard<std::mutex> lock(tapMutex_);
      if (tapPublishedGeneration_ != tapConsumedGeneration_ && !tapPublished_.empty()) {
        frame.programFullBgra.width = tapWidth_;
        frame.programFullBgra.height = tapHeight_;
        frame.programFullBgra.bgra.swap(tapPublished_);
        tapConsumedGeneration_ = tapPublishedGeneration_;
      }
    }
    @autoreleasepool {
      id<MTLCommandBuffer> commandBuffer = [queue_ commandBuffer];
      id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
      [blit copyFromTexture:program_.texture toTexture:tapTex_];
      [blit endEncoding];
      __block dispatch_semaphore_t wake = tapWake_;
      [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
        dispatch_semaphore_signal(wake);
      }];
      [commandBuffer commit];
    }
  }

  void ensureTapThread() {
    if (tapThread_.joinable()) {
      return;
    }
    tapWake_ = dispatch_semaphore_create(0);
    tapStop_ = false;
    tapThread_ = std::thread([this] {
      std::vector<uint8_t> scratch;
      while (!tapStop_.load(std::memory_order_acquire)) {
        dispatch_semaphore_wait(tapWake_, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 4));
        if (tapStop_.load(std::memory_order_acquire)) {
          break;
        }
        id<MTLTexture> texture = tapTex_;
        const int width = tapWidth_;
        const int height = tapHeight_;
        if (!texture || width <= 0 || height <= 0) {
          continue;
        }
        scratch.resize(static_cast<size_t>(width) * height * 4u);
        [texture getBytes:scratch.data()
              bytesPerRow:static_cast<NSUInteger>(width) * 4
               fromRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(width),
                                          static_cast<NSUInteger>(height))
              mipmapLevel:0];
        std::lock_guard<std::mutex> lock(tapMutex_);
        tapPublished_.swap(scratch);
        ++tapPublishedGeneration_;
      }
    });
  }

#if COREVIDEO_WITH_MAC_VIRTUALCAM
  // ── vcam GPU NV12 tap ──────────────────────────────────────────────────────
  //
  // Render thread: encode vcamBgraToNv12 (program texture -> one of four shared
  // NV12 MTLBuffers) into its own command buffer right after the program pass
  // and commit; never waits. The completion handler marks the dispatch done
  // and wakes the tap thread. Tap thread: copies the finished NV12 ONCE into an
  // immutable shared buffer (the only CPU work: a 3,110,400-byte memcpy), frees
  // the GPU buffer, then calls the sink. Dispatches are delivered strictly in
  // dispatch order (FIFO), never by completion-handler arrival order.
  //
  // Everything the completion handler touches lives in VcamTapShared, owned by
  // shared_ptr, so a handler firing after the compositor is gone touches
  // nothing freed.
  //
  // FOUR buffers, not the plan's two (fix round 1, controller ruling): the
  // program pass lets 3 command buffers be in flight without blocking
  // (renderPassInto), so under GPU backlog up to 3 tap dispatches can be
  // pending at once, plus one being copied. Two buffers skipped a frame Program
  // still delivered as soon as GPU latency passed ~2 frames — and every skip is
  // a lost camera frame under the per-frame delivery rule. ~12 MB.
  //
  // The kernel compile and the buffer allocation run on a PREPARATION thread
  // the first time a plan asks for the tap (i.e. the tick after the camera is
  // enabled), never on the render thread: a shader compile there cost the
  // first tick ~34 ms. Until preparation finishes, ticks do not dispatch and
  // are NOT counted as skips — the camera stream has not started yet, so the
  // first delivered programSequence simply starts later (no gap inside the
  // stream).
  static constexpr int kVcamWidth = 1920;
  static constexpr int kVcamHeight = 1080;
  static constexpr size_t kVcamNv12Bytes = static_cast<size_t>(kVcamWidth) * kVcamHeight * 3 / 2;
  static constexpr int kVcamBufferCount = 4;
  static constexpr uint64_t kVcamLogEveryFrames = 600;

  enum class VcamSlotState { Free, Gpu, Copying };

  struct VcamPending {
    int slot = 0;
    int64_t sequence = 0;
    bool completed = false;
    bool gpuError = false;
    double gpuMs = 0.0;
  };

  struct VcamTapShared {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<VcamPending>> fifo;
    VcamSlotState slots[kVcamBufferCount] = {};  // all Free
    bool stop = false;
    std::atomic<uint64_t> skipped{0};
  };

  struct VcamNv12ParamsHost {
    uint32_t srcWidth;
    uint32_t srcHeight;
    uint32_t mirror;
    uint32_t exact;
  };

  enum VcamPrepState : int { kVcamPrepNotStarted = 0, kVcamPrepRunning, kVcamPrepReady, kVcamPrepFailed };

  // Runs on vcamPrepThread_ only. Writes vcamPipeline_/vcamBuffers_, which the
  // render and tap threads read only after observing kVcamPrepReady (acquire).
  bool buildVcamPipeline(id<MTLDevice> device) {
    @autoreleasepool {
      if (::corevideo::compositor::vcamTapPrepFailureArmed()) {
        ::corevideo::core::nativeLogf("[virtualcam] mac gpu tap unavailable: forced by test\n");
        return false;
      }
      NSError* error = nil;
      id<MTLLibrary> library =
          [device newLibraryWithSource:[NSString stringWithUTF8String:kVcamNv12KernelSource]
                               options:nil
                                 error:&error];
      id<MTLFunction> function = library ? [library newFunctionWithName:@"vcamBgraToNv12"] : nil;
      id<MTLComputePipelineState> pipeline =
          function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
      bool ok = pipeline != nil;
      for (int i = 0; i < kVcamBufferCount && ok; ++i) {
        vcamBuffers_[i] = [device newBufferWithLength:kVcamNv12Bytes options:MTLResourceStorageModeShared];
        ok = vcamBuffers_[i] != nil;
      }
      if (!ok) {
        ::corevideo::core::nativeLogf("[virtualcam] mac gpu tap unavailable: %s\n",
                                      error ? error.localizedDescription.UTF8String : "no diagnostics");
        return false;
      }
      vcamPipeline_ = pipeline;
      return true;
    }
  }

  // Render thread. Never blocks: kicks preparation once, then reports whether
  // the tap may dispatch this tick.
  bool vcamTapReady() {
    const int state = vcamPrepState_.load(std::memory_order_acquire);
    if (state == kVcamPrepReady) {
      return true;
    }
    if (state == kVcamPrepNotStarted) {
      vcamPrepState_.store(kVcamPrepRunning, std::memory_order_release);
      id<MTLDevice> device = device_;
      vcamPrepThread_ = std::thread([this, device] {
        const bool ok = buildVcamPipeline(device);
        vcamPrepState_.store(ok ? kVcamPrepReady : kVcamPrepFailed, std::memory_order_release);
      });
    }
    return false;
  }

  void dispatchVcamTap() {
    if (!vcamSinkPresent_.load(std::memory_order_acquire) || !program_.texture) {
      return;
    }
    if (!vcamTapReady()) {
      return;
    }
    ensureVcamTapThread();
    auto shared = vcamShared_;
    auto pending = std::make_shared<VcamPending>();
    pending->sequence = frameNumber_;
    {
      std::lock_guard<std::mutex> lock(shared->mutex);
      int slot = -1;
      for (int i = 0; i < kVcamBufferCount; ++i) {
        if (shared->slots[i] == VcamSlotState::Free) {
          slot = i;
          break;
        }
      }
      if (slot < 0) {
        // Every buffer busy (GPU still writing / tap still copying). This is a
        // lost camera frame — counted, logged, never hidden.
        const uint64_t skipped = shared->skipped.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (skipped == 1 || skipped % 60 == 0) {
          ::corevideo::core::nativeLogf("[virtualcam] mac gpu tap skipped dispatch (all NV12 buffers busy) frame %lld skipped %llu\n",
                                        static_cast<long long>(frameNumber_),
                                        static_cast<unsigned long long>(skipped));
        }
        return;
      }
      shared->slots[slot] = VcamSlotState::Gpu;
      pending->slot = slot;
      shared->fifo.push_back(pending);
    }
    const VcamNv12ParamsHost params{
        static_cast<uint32_t>(program_.width), static_cast<uint32_t>(program_.height),
        vcamMirror_.load(std::memory_order_acquire) ? 1u : 0u,
        (program_.width == kVcamWidth && program_.height == kVcamHeight) ? 1u : 0u};
    @autoreleasepool {
      id<MTLCommandBuffer> commandBuffer = [queue_ commandBuffer];
      id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
      [encoder setComputePipelineState:vcamPipeline_];
      [encoder setTexture:program_.texture atIndex:0];
      [encoder setBuffer:vcamBuffers_[pending->slot] offset:0 atIndex:0];
      [encoder setBytes:&params length:sizeof(params) atIndex:1];
      [encoder dispatchThreads:MTLSizeMake(kVcamWidth / 2, kVcamHeight / 2, 1)
          threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
      [encoder endEncoding];
      [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        pending->completed = true;
        pending->gpuError = completed.status != MTLCommandBufferStatusCompleted;
        pending->gpuMs = (completed.GPUEndTime - completed.GPUStartTime) * 1000.0;
        shared->cv.notify_all();
      }];
      [commandBuffer commit];
    }
  }

  void ensureVcamTapThread() {
    if (vcamThread_.joinable()) {
      return;
    }
    auto shared = vcamShared_;
    // Started only after preparation is Ready, so vcamBuffers_ is final here.
    vcamThread_ = std::thread([this, shared] {
      uint64_t windowFrames = 0;
      double gpuSum = 0, gpuMax = 0, copySum = 0, copyMax = 0;
      int64_t windowFirstSequence = 0;
      auto windowStart = std::chrono::steady_clock::now();
      for (;;) {
        std::shared_ptr<VcamPending> next;
        {
          std::unique_lock<std::mutex> lock(shared->mutex);
          shared->cv.wait(lock, [&] {
            return shared->stop || (!shared->fifo.empty() && shared->fifo.front()->completed);
          });
          if (shared->stop) {
            return;
          }
          next = shared->fifo.front();
          shared->fifo.pop_front();
          shared->slots[next->slot] = VcamSlotState::Copying;
        }
        ICompositor::VcamFrameBuffer frame;
        double copyMs = 0;
        if (!next->gpuError) {
          const auto copyStart = std::chrono::steady_clock::now();
          const auto* bytes = static_cast<const uint8_t*>(vcamBuffers_[next->slot].contents);
          frame = std::make_shared<const std::vector<uint8_t>>(bytes, bytes + kVcamNv12Bytes);
          copyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - copyStart).count();
        }
        {
          std::lock_guard<std::mutex> lock(shared->mutex);
          shared->slots[next->slot] = VcamSlotState::Free;
        }
        if (!frame) {
          ::corevideo::core::nativeLogf("[virtualcam] mac gpu tap command buffer failed; frame %lld lost\n",
                                        static_cast<long long>(next->sequence));
          continue;
        }
        {
          std::lock_guard<std::mutex> lock(vcamSinkMutex_);
          if (identifiedVcamSink_) {
            identifiedVcamSink_(frame, kVcamWidth, kVcamHeight, next->sequence, 0);
          } else if (vcamSink_) {
            vcamSink_(frame, kVcamWidth, kVcamHeight);
          }
        }
        if (windowFrames == 0) {
          windowFirstSequence = next->sequence;
          windowStart = std::chrono::steady_clock::now();
        }
        ++windowFrames;
        gpuSum += next->gpuMs;
        gpuMax = std::max(gpuMax, next->gpuMs);
        copySum += copyMs;
        copyMax = std::max(copyMax, copyMs);
        if (windowFrames == kVcamLogEveryFrames) {
          ::corevideo::core::nativeVerboseLogf(
              "[virtualcam] mac gpu tap gpu %.3f ms copy %.3f ms skipped %llu (600 frames) max gpu %.3f ms copy %.3f ms seq %lld-%lld span %.1f ms\n",
              gpuSum / windowFrames, copySum / windowFrames,
              static_cast<unsigned long long>(shared->skipped.load(std::memory_order_acquire)), gpuMax, copyMax,
              static_cast<long long>(windowFirstSequence), static_cast<long long>(next->sequence),
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - windowStart).count());
          windowFrames = 0;
          gpuSum = gpuMax = copySum = copyMax = 0;
        }
      }
    });
  }

  void stopVcamTap() {
    if (vcamPrepThread_.joinable()) {
      vcamPrepThread_.join();
    }
    {
      std::lock_guard<std::mutex> lock(vcamShared_->mutex);
      vcamShared_->stop = true;
    }
    vcamShared_->cv.notify_all();
    if (vcamThread_.joinable()) {
      vcamThread_.join();
    }
  }
#endif  // COREVIDEO_WITH_MAC_VIRTUALCAM

  void readTargetBgra() {
    readbackBgra_.resize(static_cast<size_t>(program_.width) * static_cast<size_t>(program_.height) * 4u);
    [program_.texture getBytes:readbackBgra_.data()
                   bytesPerRow:static_cast<NSUInteger>(program_.width) * 4
                    fromRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(program_.width),
                                               static_cast<NSUInteger>(program_.height))
                   mipmapLevel:0];
  }

  // Shared pass body: clear + resolve + draw every layer into the target.
  // Callers set targetWidth_/targetHeight_ (the draw helpers' canvas dims)
  // BEFORE calling and restore them afterwards for secondary passes.
  void renderPassInto(PassTarget& target,
                      const CompositorRenderPlan& deterministicPlan,
                      const std::vector<VideoFrame>& frames,
                      bool waitForCompletion) {
    @autoreleasepool {
      id<MTLCommandBuffer> commandBuffer = [queue_ commandBuffer];
      MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = target.texture;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0.047, 0.067, 0.094, 1.0);
      id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
      const auto layers = resolveLayers(deterministicPlan, frames);
      for (const auto& layer : layers) {
        drawLayer(encoder, layer, deterministicPlan);
      }
      [encoder endEncoding];
      // Bounded in-flight pacing (classic triple buffering): the light 60fps
      // display tick must not CPU-block (a ~26ms wait held coreMutex at ~99%
      // duty and starved every RPC request), but committing with NO pacing
      // floods the queue unboundedly and a later readback wait queues behind
      // thousands of buffers (observed: first request answered, all later
      // ones starved). Acquire one of 3 in-flight slots before commit; the
      // completion handler releases it.
      dispatch_semaphore_wait(inflight_, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));
      dispatch_semaphore_t inflight = inflight_;
      [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
        dispatch_semaphore_signal(inflight);
      }];
      [commandBuffer commit];
      // Block fully only when the caller needs completed pixels (CPU
      // readbacks / secondary-surface consumers).
      if (waitForCompletion) {
        [commandBuffer waitUntilCompleted];
      }
    }
  }

  ProgramFrameSharedTexture renderSecondaryPass(PassTarget& target,
                                                const CompositorRenderPlan& renderPlan,
                                                const std::vector<VideoFrame>& frames) {
    ProgramFrameSharedTexture out;
    if (!ensurePipeline()) {
      return out;
    }
    const auto deterministicPlan = sortCompositorRenderPlan(renderPlan);
    if (deterministicPlan.width <= 0 || deterministicPlan.height <= 0) {
      return out;
    }
    if (!ensureTarget(target, deterministicPlan.width, deterministicPlan.height)) {
      return out;
    }
    const int savedWidth = targetWidth_;
    const int savedHeight = targetHeight_;
    targetWidth_ = target.width;
    targetHeight_ = target.height;
    // Monitor passes share the raster (and its cache) but report nothing.
    overlayRaster_.beginFrame();
    renderPassInto(target, deterministicPlan, frames, /*waitForCompletion=*/true);
    overlayRaster_.beginFrame();
    targetWidth_ = savedWidth;
    targetHeight_ = savedHeight;
    out.iosurfaceId = target.iosurfaceId;
    out.width = target.width;
    out.height = target.height;
    out.frameNumber = frameNumber_;
    return out;
  }

  // ── layer resolution (mirrors D3D11CompositorAdapter::resolveLayers) ───────

  static const VideoFrame* frameForParticipant(const std::vector<VideoFrame>& frames,
                                               const std::string& participantId) {
    for (const auto& frame : frames) {
      if (frame.participantId == participantId && frameHasContent(frame)) {
        return &frame;
      }
    }
    return nullptr;
  }

  static void warnUnmatchedCaptureLayer(const std::string& sourceKey,
                                        const std::vector<VideoFrame>& frames) {
    const bool warnable = sourceKey.rfind("capture:", 0) == 0 || sourceKey.rfind("media:", 0) == 0;
    if (!warnable) {
      return;
    }
    static std::unordered_map<std::string, int64_t> lastWarnMs;
    const int64_t nowMs = static_cast<int64_t>([NSDate date].timeIntervalSince1970 * 1000.0);
    auto& last = lastWarnMs[sourceKey];
    if (nowMs - last < 5000) {
      return;
    }
    last = nowMs;
    std::string available;
    for (const auto& frame : frames) {
      if (!available.empty()) {
        available += ", ";
      }
      available += frame.participantId;
    }
    ::corevideo::core::nativeLogf("[compositor] layer %s has NO matching frame (available: %s)\n",
                 sourceKey.c_str(), available.c_str());
  }

  std::vector<ResolvedLayer> resolveLayers(const CompositorRenderPlan& renderPlan,
                                           const std::vector<VideoFrame>& frames) const {
    std::vector<ResolvedLayer> layers;
    if (!renderPlan.layers.empty()) {
      layers.reserve(renderPlan.layers.size());
      int videoIndex = 0;
      const int videoLayerCount = static_cast<int>(std::count_if(
          renderPlan.layers.begin(), renderPlan.layers.end(),
          [](const CompositorRenderPlanLayer& layer) { return !compositorLayerIsOverlay(layer); }));
      for (auto planLayer : renderPlan.layers) {
        ResolvedLayer layer;
        layer.plan = std::move(planLayer);
        if (layer.plan.rect.width <= 0.f || layer.plan.rect.height <= 0.f) {
          if (compositorLayerIsOverlay(layer.plan)) {
            const auto layout = compositorLayerIsLowerThird(layer.plan) ? compositor::lowerThirdOverlay()
                                                                        : compositor::topRightOverlay();
            layer.plan.rect = {layout.x, layout.y, layout.width, layout.height};
          } else {
            const auto layout = compositor::gridCell((std::max)(1, videoLayerCount), videoIndex);
            layer.plan.rect = {layout.x, layout.y, layout.width, layout.height};
            ++videoIndex;
          }
        }
        if (compositorLayerIsOverlay(layer.plan)) {
          layer.color = 0xff2a3548;
        } else if (!layer.plan.participantId.empty()) {
          // bus health on air (#535 slice 4a): the ONE resolution rule, not a
          // per-kind placeholder. See CompositorLayout.h slateColorFor/blackOnStalled.
          layer.color = compositor::slateColorFor(layer.plan.sourceHealth);
          layer.frame = frameForParticipant(frames, layer.plan.participantId);
          if (layer.frame == nullptr) {
            warnUnmatchedCaptureLayer(layer.plan.participantId, frames);
          }
        } else if (!layer.plan.mediaAssetId.empty()) {
          layer.color = compositor::slateColorFor(layer.plan.sourceHealth);
          const std::string frameSourceId =
              layer.plan.sourceId.empty() ? "media:" + layer.plan.mediaAssetId : layer.plan.sourceId;
          layer.frame = frameForParticipant(frames, frameSourceId);
          if (layer.frame == nullptr) {
            warnUnmatchedCaptureLayer(frameSourceId, frames);
          }
        }
        // hasFillColor: a DELIBERATELY SOURCELESS solid layer — the Tiles wall
        // background (`tiles-bg:<layerId>`) and fade-to-black
        // (ProgramBlackout::append). Mirrors the D3D11 resolveLayers branch and
        // ProgramFramePreview.cpp: never resolves to a frame; paints the parsed
        // fillColor instead of the default mid-grey ResolvedLayer::color. The
        // former KNOWN GAP (Mac FTB faded to 0x44 grey) is closed; proven by
        // MetalCompositor.FadeToBlackProducesBlackPixelsAndRestoresHeldVideo and
        // MetalCompositor.SourcelessFillColorLayerPaintsItsParsedColour.
        else if (layer.plan.hasFillColor) {
          layer.color = compositor::parseHexColorRgba(layer.plan.fillColor, 0xff808080u);
        } else if (videoIndex > 0 && videoIndex - 1 < static_cast<int>(frames.size())) {
          // Legacy positional fallback (a layer with no participantId/mediaAssetId
          // at all, matched by index) — I-4: pink is retired for every kind, so
          // this resolves by the same one rule as every other layer.
          const auto& fallbackFrame = frames[static_cast<size_t>(videoIndex - 1)];
          layer.color = compositor::slateColorFor(layer.plan.sourceHealth);
          if (frameHasContent(fallbackFrame)) {
            layer.frame = &fallbackFrame;
          }
        }

        // Content frame present but the source is stalled and the operator's
        // per-source policy is "black": draw solid black, frame ignored. Must
        // run after the frame resolution above and before drawLayer, which
        // branches solid-vs-textured on layer.frame — mirrors the D3D11/CPU
        // preview rule exactly.
        if (!compositorLayerIsOverlay(layer.plan) && compositor::suppressFrameFor(layer.plan.sourceHealth)) {
          layer.frame = nullptr;
          layer.color = compositor::kFailedSlateRgba;
        } else if (!compositorLayerIsOverlay(layer.plan) && layer.frame != nullptr && frameHasContent(*layer.frame) &&
            compositor::blackOnStalled(layer.plan.sourceHealth, layer.plan.dropoutPolicy)) {
          layer.frame = nullptr;
          layer.color = compositor::kDropoutBlackRgba;
        }
        layers.push_back(std::move(layer));
      }
      return layers;
    }

    layers.reserve(frames.size());
    const int count = static_cast<int>(frames.size());
    for (int index = 0; index < count; ++index) {
      ResolvedLayer layer;
      layer.plan.layerId = "fallback:" + std::to_string(index);
      layer.plan.kind = "participant-video";
      layer.plan.participantId = frames[static_cast<size_t>(index)].participantId;
      layer.plan.sourceId = "zoom:" + layer.plan.participantId;
      layer.plan.order = index;
      const auto layout = compositor::gridCell((std::max)(1, count), index);
      layer.plan.rect = {layout.x, layout.y, layout.width, layout.height};
      // bus health on air (#535 slice 4a), I-4: this is the empty-render-plan
      // improvised grid fallback (no CompositorRenderPlanLayer at all, so no
      // sourceHealth to read) — pink is retired for every kind, so this
      // resolves to the warming slate like every other frameless layer.
      layer.color = compositor::kWarmingSlateRgba;
      if (frameHasContent(frames[static_cast<size_t>(index)])) {
        layer.frame = &frames[static_cast<size_t>(index)];
      }
      layers.push_back(std::move(layer));
    }
    return layers;
  }

  // ── draw helpers (mirrors the D3D11 viewport/scissor/constants flow) ───────

  void setViewportFromRect(id<MTLRenderCommandEncoder> encoder, const compositor::LayerRect& rect) {
    MTLViewport viewport{};
    viewport.originX = rect.x * static_cast<double>(targetWidth_);
    viewport.originY = rect.y * static_cast<double>(targetHeight_);
    viewport.width = std::max(0.f, rect.width) * static_cast<double>(targetWidth_);
    viewport.height = std::max(0.f, rect.height) * static_cast<double>(targetHeight_);
    viewport.znear = 0.0;
    viewport.zfar = 1.0;
    [encoder setViewport:viewport];
  }

  void setScissorFromRect(id<MTLRenderCommandEncoder> encoder, const compositor::LayerRect& rect) {
    const long left = std::clamp<long>(
        static_cast<long>(std::floor(rect.x * static_cast<float>(targetWidth_))), 0, targetWidth_);
    const long top = std::clamp<long>(
        static_cast<long>(std::floor(rect.y * static_cast<float>(targetHeight_))), 0, targetHeight_);
    const long right = std::clamp<long>(
        static_cast<long>(std::ceil((rect.x + rect.width) * static_cast<float>(targetWidth_))), 0,
        targetWidth_);
    const long bottom = std::clamp<long>(
        static_cast<long>(std::ceil((rect.y + rect.height) * static_cast<float>(targetHeight_))), 0,
        targetHeight_);
    MTLScissorRect scissor{};
    scissor.x = static_cast<NSUInteger>(left);
    scissor.y = static_cast<NSUInteger>(top);
    scissor.width = static_cast<NSUInteger>(std::max(0L, right - left));
    scissor.height = static_cast<NSUInteger>(std::max(0L, bottom - top));
    [encoder setScissorRect:scissor];
  }

  void resetScissor(id<MTLRenderCommandEncoder> encoder) {
    MTLScissorRect scissor{0, 0, static_cast<NSUInteger>(targetWidth_),
                           static_cast<NSUInteger>(targetHeight_)};
    [encoder setScissorRect:scissor];
  }

  void setLayerConstants(id<MTLRenderCommandEncoder> encoder,
                         const ResolvedLayer& layer,
                         const CompositorRenderPlan& renderPlan,
                         uint32_t colorArgb,
                         float alpha,
                         float uvScaleX,
                         float uvScaleY,
                         float uvOffsetX,
                         float uvOffsetY) {
    LayerShaderConstants constants{};
    constants.color[0] = static_cast<float>((colorArgb >> 16) & 0xff) / 255.f;
    constants.color[1] = static_cast<float>((colorArgb >> 8) & 0xff) / 255.f;
    constants.color[2] = static_cast<float>(colorArgb & 0xff) / 255.f;
    constants.color[3] = alpha;
    const auto grade = layer.plan.hasColorGrade ? layer.plan.colorGrade : renderPlan.colorGrade;
    applyColorGradeParams(&constants, grade);
    const auto advanced = advancedGradeConstants(grade);
    [encoder setFragmentBytes:&advanced length:sizeof(advanced) atIndex:1];
    [encoder setFragmentTexture:gradeCurveTexture(grade) atIndex:3];
    for (size_t i=0;i<8;++i) [encoder setFragmentTexture:gradeCubeTexture(grade,i) atIndex:4+i];
    constants.uvScale[0] = uvScaleX;
    constants.uvScale[1] = uvScaleY;
    constants.uvOffset[0] = uvOffsetX;
    constants.uvOffset[1] = uvOffsetY;
    // Chroma key is per-layer only — there is deliberately no render-plan-wide
    // fallback, because keying every layer including backgrounds and overlays
    // is never what an operator means.
    if (layer.plan.hasChromaKey) {
      const auto& key = layer.plan.chromaKey;
      constants.chromaKeyColor[0] = key.keyR;
      constants.chromaKeyColor[1] = key.keyG;
      constants.chromaKeyColor[2] = key.keyB;
      constants.chromaKeyColor[3] = 1.f;  // enable
      constants.chromaKeyParams[0] = key.similarity;
      constants.chromaKeyParams[1] = key.smoothness;
      constants.chromaKeyParams[2] = key.spill;
    }
    applyYuvParams(&constants, yuvShaderParamsForFrame(layer.frame));
    if (tilesEffectsAvailable_) applyTilesDecoration(constants, layer.plan, targetWidth_, targetHeight_);
    [encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
  }

  void drawSolidQuad(id<MTLRenderCommandEncoder> encoder,
                     const ResolvedLayer& layer,
                     const CompositorRenderPlan& renderPlan,
                     const compositor::LayerRect& rect,
                     uint32_t colorArgb,
                     float alpha) {
    if (rect.width <= 0.f || rect.height <= 0.f || alpha <= 0.f) {
      return;
    }
    setViewportFromRect(encoder, rect);
    setLayerConstants(encoder, layer, renderPlan, colorArgb, alpha, 1.f, 1.f, 0.f, 0.f);
    [encoder setRenderPipelineState:solidPipeline_];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }

  void drawBorderPass(id<MTLRenderCommandEncoder> encoder,
                      const ResolvedLayer& layer,
                      const CompositorRenderPlan& renderPlan,
                      const compositor::LayerRect& rect,
                      const compositor::BorderFraming& border,
                      float alpha) {
    if (!border.visible || alpha <= 0.f) {
      return;
    }
    const float smaller = std::min(rect.width, rect.height);
    float stroke = border.thickness * smaller;
    stroke = std::max(stroke, 1.f / static_cast<float>(std::max(targetWidth_, targetHeight_)));
    const float strokeX = std::min(stroke, rect.width * 0.5f);
    const float strokeY = std::min(stroke, rect.height * 0.5f);
    const uint32_t color = border.colorRgba;
    const float borderAlpha =
        std::clamp(alpha * (static_cast<float>((color >> 24) & 0xff) / 255.f), 0.f, 1.f);
    drawSolidQuad(encoder, layer, renderPlan, {rect.x, rect.y, rect.width, strokeY}, color, borderAlpha);
    drawSolidQuad(encoder, layer, renderPlan,
                  {rect.x, rect.y + rect.height - strokeY, rect.width, strokeY}, color, borderAlpha);
    drawSolidQuad(encoder, layer, renderPlan, {rect.x, rect.y, strokeX, rect.height}, color, borderAlpha);
    drawSolidQuad(encoder, layer, renderPlan,
                  {rect.x + rect.width - strokeX, rect.y, strokeX, rect.height}, color, borderAlpha);
  }

  // ── texture uploads + per-source cache ─────────────────────────────────────

  id<MTLTexture> makeTexture(MTLPixelFormat format, int width, int height) {
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                           width:static_cast<NSUInteger>(width)
                                                          height:static_cast<NSUInteger>(height)
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    ++stats_.textureCreates;
    return [device_ newTextureWithDescriptor:desc];
  }

  static void uploadRegion(id<MTLTexture> texture, const uint8_t* bytes, int width, int height, int stride) {
    [texture replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(width),
                                           static_cast<NSUInteger>(height))
               mipmapLevel:0
                 withBytes:bytes
               bytesPerRow:static_cast<NSUInteger>(stride)];
  }

  // Returns the cache entry for a frame with a stable source id, uploading only
  // when the frame content advanced (one upload per new frame regardless of how
  // many passes draw it). Frames without a stable id take a scratch upload.
  SourceTex* acquireSourceTex(const VideoFrame& frame) {
    if (frame.participantId.empty()) {
      return nullptr;
    }
    auto& tex = sourceTextures_[frame.participantId];
    tex.lastUsedFrame = frameNumber_;
    const bool isI420 = frame.hasI420();
    const int width = isI420 ? frame.i420Width : frame.pixelWidth;
    const int height = isI420 ? frame.i420Height : frame.pixelHeight;
    const bool needsCreate =
        tex.isI420 != isI420 || tex.width != width || tex.height != height ||
        (isI420 ? tex.y == nil : tex.bgra == nil);
    if (needsCreate) {
      tex = SourceTex{};
      tex.lastUsedFrame = frameNumber_;
      tex.isI420 = isI420;
      tex.width = width;
      tex.height = height;
      if (isI420) {
        tex.y = makeTexture(MTLPixelFormatR8Unorm, width, height);
        tex.u = makeTexture(MTLPixelFormatR8Unorm, width / 2, height / 2);
        tex.v = makeTexture(MTLPixelFormatR8Unorm, width / 2, height / 2);
      } else {
        tex.bgra = makeTexture(MTLPixelFormatBGRA8Unorm, width, height);
      }
    }
    if ((isI420 && (!tex.y || !tex.u || !tex.v)) || (!isI420 && !tex.bgra)) {
      return nullptr;
    }
    if (tex.frameId == frame.frameId && !needsCreate) {
      ++stats_.cacheHits;
      return &tex;
    }
    if (isI420) {
      const uint8_t* yPlane = frame.i420->data();
      const uint8_t* uPlane = yPlane + static_cast<size_t>(width) * static_cast<size_t>(height);
      const uint8_t* vPlane =
          uPlane + static_cast<size_t>(width / 2) * static_cast<size_t>(height / 2);
      uploadRegion(tex.y, yPlane, width, height, width);
      uploadRegion(tex.u, uPlane, width / 2, height / 2, width / 2);
      uploadRegion(tex.v, vPlane, width / 2, height / 2, width / 2);
    } else {
      uploadRegion(tex.bgra, frame.pixels->data(), width, height, frame.pixelStride);
    }
    tex.frameId = frame.frameId;
    ++stats_.cachedUploads;
    return &tex;
  }

  bool uploadScratchTex(const VideoFrame& frame, SourceTex& out) {
    out.isI420 = frame.hasI420();
    if (out.isI420) {
      out.width = frame.i420Width;
      out.height = frame.i420Height;
      out.y = makeTexture(MTLPixelFormatR8Unorm, out.width, out.height);
      out.u = makeTexture(MTLPixelFormatR8Unorm, out.width / 2, out.height / 2);
      out.v = makeTexture(MTLPixelFormatR8Unorm, out.width / 2, out.height / 2);
      if (!out.y || !out.u || !out.v) {
        return false;
      }
      const uint8_t* yPlane = frame.i420->data();
      const uint8_t* uPlane = yPlane + static_cast<size_t>(out.width) * static_cast<size_t>(out.height);
      const uint8_t* vPlane =
          uPlane + static_cast<size_t>(out.width / 2) * static_cast<size_t>(out.height / 2);
      uploadRegion(out.y, yPlane, out.width, out.height, out.width);
      uploadRegion(out.u, uPlane, out.width / 2, out.height / 2, out.width / 2);
      uploadRegion(out.v, vPlane, out.width / 2, out.height / 2, out.width / 2);
      return true;
    }
    out.width = frame.pixelWidth;
    out.height = frame.pixelHeight;
    out.bgra = makeTexture(MTLPixelFormatBGRA8Unorm, out.width, out.height);
    if (!out.bgra) {
      return false;
    }
    uploadRegion(out.bgra, frame.pixels->data(), out.width, out.height, frame.pixelStride);
    return true;
  }

  // ── layer draw ─────────────────────────────────────────────────────────────

  void drawLayer(id<MTLRenderCommandEncoder> encoder,
                 const ResolvedLayer& layer,
                 const CompositorRenderPlan& renderPlan) {
    const compositor::LayerRect rect{layer.plan.rect.x, layer.plan.rect.y, layer.plan.rect.width,
                                     layer.plan.rect.height};
    const float layerAlpha = compositorLayerOpacity(layer.plan);

    if (layer.plan.tilesDecoration.enabled && layer.plan.tilesDecoration.glowPass) {
      if (!tilesEffectsAvailable_) return;
      const auto expanded = tilesGlowRect(layer.plan, targetWidth_, targetHeight_);
      if (layer.plan.hasClipRect)
        setScissorFromRect(encoder, {layer.plan.clipRect.x, layer.plan.clipRect.y, layer.plan.clipRect.width, layer.plan.clipRect.height});
      drawSolidQuad(encoder, layer, renderPlan, expanded, 0xffffffffu, layerAlpha);
      resetScissor(encoder);
      return;
    }

    if (layer.plan.hasOverlayContent && compositorLayerIsOverlay(layer.plan)) {
      drawOverlayLayer(encoder, layer, renderPlan, rect, layerAlpha);
      return;
    }

    int sourceWidth = renderPlan.width > 0 ? renderPlan.width : 16;
    int sourceHeight = renderPlan.height > 0 ? renderPlan.height : 9;
    if (layer.frame != nullptr) {
      sourceWidth = layer.frame->naturalWidth > 0
                        ? layer.frame->naturalWidth
                        : layer.frame->width > 0
                              ? layer.frame->width
                              : layer.frame->pixelWidth > 0 ? layer.frame->pixelWidth : sourceWidth;
      sourceHeight = layer.frame->naturalHeight > 0
                         ? layer.frame->naturalHeight
                         : layer.frame->height > 0
                               ? layer.frame->height
                               : layer.frame->pixelHeight > 0 ? layer.frame->pixelHeight : sourceHeight;
    }
    const compositor::LayerRect aspectRect{0.f, 0.f, rect.width * static_cast<float>(targetWidth_),
                                           rect.height * static_cast<float>(targetHeight_)};
    const auto framing = compositor::computeSourceFraming(sourceWidth, sourceHeight, aspectRect,
                                                          layer.plan.fitMode, layer.plan.sourceScale,
                                                          layer.plan.sourceOffsetX,
                                                          layer.plan.sourceOffsetY, layer.plan.sourceCropLeftPercent, layer.plan.sourceCropRightPercent);

    if (framing.hasLetterbox) {
      drawSolidQuad(encoder, layer, renderPlan, rect, 0xff05080cu, layerAlpha);
    }

    const float imageFracX = aspectRect.width > 0.f ? framing.imageX / aspectRect.width : 0.f;
    const float imageFracY = aspectRect.height > 0.f ? framing.imageY / aspectRect.height : 0.f;
    const float imageFracW = aspectRect.width > 0.f ? framing.imageW / aspectRect.width : 1.f;
    const float imageFracH = aspectRect.height > 0.f ? framing.imageH / aspectRect.height : 1.f;
    const compositor::LayerRect imageRect{rect.x + imageFracX * rect.width,
                                          rect.y + imageFracY * rect.height, imageFracW * rect.width,
                                          imageFracH * rect.height};

    const auto crop = compositor::sourceCropInterval(layer.plan.sourceCropLeftPercent, layer.plan.sourceCropRightPercent);
    // Main content pass: viewport = full rendered source layer; scissor = slot
    // (or the explicit clipRect for remapped composites).
    setViewportFromRect(encoder, imageRect);
    setLayerConstants(encoder, layer, renderPlan, layer.color, layerAlpha, crop.width, 1.f, crop.left, 0.f);

    const bool isI420 = layer.frame != nullptr && layer.frame->hasI420();
    SourceTex* sourceTex = layer.frame != nullptr ? acquireSourceTex(*layer.frame) : nullptr;
    // Frames without a stable source id (no participantId) can't ride the
    // cache; give them a transient upload so their pixels still render (the
    // D3D11 shared-scratch analog).
    SourceTex scratch;
    if (sourceTex == nullptr && layer.frame != nullptr && frameHasContent(*layer.frame) &&
        uploadScratchTex(*layer.frame, scratch)) {
      sourceTex = &scratch;
      ++stats_.scratchUploads;
    }
    if (sourceTex != nullptr) {
      [encoder setFragmentSamplerState:sampler_ atIndex:0];
      if (isI420) {
        [encoder setRenderPipelineState:i420Pipeline_];
        [encoder setFragmentTexture:sourceTex->y atIndex:0];
        [encoder setFragmentTexture:sourceTex->u atIndex:1];
        [encoder setFragmentTexture:sourceTex->v atIndex:2];
      } else {
        [encoder setRenderPipelineState:texturedPipeline_];
        [encoder setFragmentTexture:sourceTex->bgra atIndex:0];
      }
    } else {
      [encoder setRenderPipelineState:solidPipeline_];
    }
    const compositor::LayerRect clip = layer.plan.hasClipRect
        ? compositor::LayerRect{layer.plan.clipRect.x, layer.plan.clipRect.y,
                                layer.plan.clipRect.width, layer.plan.clipRect.height}
        : rect;
    setScissorFromRect(encoder, clip);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    resetScissor(encoder);

    // bus health on air (#535 slice 4a, R4): a failed OR stalled-with-no-frame
    // source's slate carries its display name so an operator can tell WHICH
    // source died at a glance (D3D11 drawFailedSlateName parity).
    if (layer.frame == nullptr &&
        (layer.plan.sourceHealth == "failed" || layer.plan.sourceHealth == "stalled")) {
      drawFailedSlateName(encoder, layer, renderPlan, rect, layerAlpha);
    }

    const auto border = compositor::computeBorderFraming(layer.plan.borderStyle, layer.plan.borderColor,
                                                         layer.plan.borderThickness);
    drawBorderPass(encoder, layer, renderPlan, rect, border, layerAlpha);
  }

  // The slate name as a SMALL LABEL anchored bottom-left of the layer, not
  // the full-canvas lower-third band a bare overlay content would raster.
  // Mirrors D3D11CompositorAdapter::drawFailedSlateName: height 8% of the
  // layer; width capped at 40% of it or a rough text-width ESTIMATE (~55% of
  // the line height per glyph plus breathing room); brandColor = the slate
  // colour so the lower-third accent bar is invisible against it; the raster
  // paints no band for plain content, so there is no plate. Prepared by the
  // same async raster; while it prepares the colour-only slate shows.
  void drawFailedSlateName(id<MTLRenderCommandEncoder> encoder,
                           const ResolvedLayer& layer,
                           const CompositorRenderPlan& renderPlan,
                           const compositor::LayerRect& rect,
                           float layerAlpha) {
    const std::string& name = layer.plan.sourceDisplayName;
    if (name.empty()) {
      return;
    }
    const float labelHeight = rect.height * 0.08f;
    if (labelHeight <= 0.f || rect.width <= 0.f) {
      return;
    }
    const float labelHeightPx = labelHeight * static_cast<float>(targetHeight_);
    const float estTextWidthPx = labelHeightPx * 0.55f * static_cast<float>(name.size()) + labelHeightPx;
    const float estTextWidthFrac =
        targetWidth_ > 0 ? estTextWidthPx / static_cast<float>(targetWidth_) : rect.width;
    const float labelWidth = std::min(rect.width * 0.4f, estTextWidthFrac);
    if (labelWidth <= 0.f) {
      return;
    }
    const compositor::LayerRect labelRect{rect.x, rect.y + rect.height - labelHeight, labelWidth, labelHeight};
    char hexBuf[8];
    std::snprintf(hexBuf, sizeof(hexBuf), "#%06x", compositor::kFailedSlateRgba & 0xffffffu);
    // Built per draw, as on Windows. Every string is short (small-string
    // storage) unless the display name is long; the raster lookup is a hit.
    CompositorOverlayContent content;
    content.title = name;
    content.brandColor = hexBuf;
    id<MTLTexture> texture =
        overlayRaster_.rasterOverlayTexture(device_, content, labelRect, targetWidth_, targetHeight_);
    if (!texture) {
      return;
    }
    const compositor::LayerRect clip = layer.plan.hasClipRect
        ? compositor::LayerRect{layer.plan.clipRect.x, layer.plan.clipRect.y,
                                layer.plan.clipRect.width, layer.plan.clipRect.height}
        : rect;
    drawOverlayTexture(encoder, layer, renderPlan, texture, labelRect, clip, layerAlpha);
    resetScissor(encoder);
  }

  void drawOverlayLayer(id<MTLRenderCommandEncoder> encoder,
                        const ResolvedLayer& layer,
                        const CompositorRenderPlan& renderPlan,
                        const compositor::LayerRect& rect,
                        float layerAlpha) {
    const auto& overlay = layer.plan.overlay;
    const auto key = compositor::computeOverlayKeyTransform(overlay.keyPhase, overlay.keyProgress,
                                                            overlay.keyPosition, rect.height);
    if (!key.visible || key.alpha <= 0.001f) {
      return;
    }
    const float alpha = std::clamp(layerAlpha * key.alpha, 0.f, 1.f);
    if (alpha <= 0.f) {
      return;
    }

    compositor::LayerRect animated = rect;
    animated.x += key.slideX;
    animated.y += key.slideY;
    const float centerX = animated.x + animated.width * 0.5f;
    const float centerY = animated.y + animated.height * 0.5f;
    animated.width *= key.contentScale;
    animated.height *= key.contentScale;
    animated.x = centerX - animated.width * 0.5f;
    animated.y = centerY - animated.height * 0.5f;

    const uint32_t background = compositor::parseHexColorRgba(overlay.brandBackgroundColor, 0xff0c1118u);
    const uint32_t accent = compositor::parseHexColorRgba(overlay.brandColor, 0xff44c1a1u);
    const compositor::LayerRect clip = layer.plan.hasClipRect
        ? compositor::LayerRect{layer.plan.clipRect.x, layer.plan.clipRect.y,
                                layer.plan.clipRect.width, layer.plan.clipRect.height}
        : compositor::LayerRect{0.f, 0.f, 1.f, 1.f};

    // Instance revisions become visible only after their complete raster
    // exists (strict); legacy overlays draw the fallback while they prepare.
    id<MTLTexture> overlayTex = overlayRaster_.rasterOverlayTexture(
        device_, overlay, rect, targetWidth_, targetHeight_, {}, !layer.plan.graphicsInstanceId.empty(),
        layer.plan.graphicsPreparedResource);
    if (overlay.holdLastRaster) {
      // #1073 Tiles name line (D3D11CompositorAdapter.cpp parity): a reflow
      // resizes every band at once and preparation is bounded, so "preparing"
      // can last several frames on a big wall. Draw the layer's previous
      // raster (the viewport scales it to the new band) until the new one is
      // ready, and never the bare accent bar: a tick with no text is worse
      // than a name a few frames old. The lease keeps the texture alive.
      const int widthPx = static_cast<int>(std::lround(double(rect.width) * targetWidth_));
      const int heightPx = static_cast<int>(std::lround(double(rect.height) * targetHeight_));
      const void* drawable = retainedOverlayRasters_.resolve(
          layer.plan.layerId, overlayTex ? (__bridge const void*)overlayTex : nullptr,
          overlayTex ? overlayRaster_.lease() : nullptr, widthPx, heightPx,
          static_cast<uint64_t>(frameNumber_));
      overlayTex = drawable ? (__bridge id<MTLTexture>)drawable : nil;
      if (!overlayTex) return;
    }
    if (!layer.plan.graphicsInstanceId.empty() && !overlayTex) return;

    // Brand band quad: only when the raster does not paint the band itself
    // (an appearance-styled lower third paints its own rounded band). Drawing
    // both doubled the band and showed the quad round the rounded corners.
    setScissorFromRect(encoder, clip);
    if (!overlay.appearance.enabled || overlay.isCaption) {
      drawSolidQuad(encoder, layer, renderPlan, animated, background, alpha);
    }

    // Composite the prepared raster over the band. Sizing/caching uses the
    // un-animated rect so a build-in/out sweep scales the SAME cached texture.
    if (overlayTex) {
      drawOverlayTexture(encoder, layer, renderPlan, overlayTex, animated, clip, alpha);
    } else {
      setScissorFromRect(encoder, clip);
      if (overlay.isCaption) {
        drawSolidQuad(encoder, layer, renderPlan,
                      {animated.x, animated.y, animated.width, animated.height * 0.10f}, accent, alpha);
      } else {
        drawSolidQuad(encoder, layer, renderPlan,
                      {animated.x, animated.y, animated.width * 0.04f, animated.height}, accent, alpha);
      }
    }
    resetScissor(encoder);
  }

  // One premultiplied (One, 1-SrcAlpha) draw of a prepared overlay texture
  // into `destination`, scissored to `clip`.
  void drawOverlayTexture(id<MTLRenderCommandEncoder> encoder,
                          const ResolvedLayer& layer,
                          const CompositorRenderPlan& renderPlan,
                          id<MTLTexture> texture,
                          const compositor::LayerRect& destination,
                          const compositor::LayerRect& clip,
                          float alpha) {
    setScissorFromRect(encoder, clip);
    setViewportFromRect(encoder, destination);
    setLayerConstants(encoder, layer, renderPlan, 0xffffffffu, alpha, 1.f, 1.f, 0.f, 0.f);
    [encoder setRenderPipelineState:overlayPipeline_];
    [encoder setFragmentTexture:texture atIndex:0];
    [encoder setFragmentSamplerState:sampler_ atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }

  // ── members ────────────────────────────────────────────────────────────────

  std::map<std::string, id<MTLTexture>> gradeCurveTextures_;
  id<MTLTexture> gradeCurveTexture(const CompositorColorGrade& grade) {
    if (!grade.advanced || grade.advanced->bypass || grade.advanced->operations.empty()) return nil;
    const auto& key = grade.advanced->content;
    auto found = gradeCurveTextures_.find(key);
    if (found != gradeCurveTextures_.end()) return found->second;
    auto samples = compileGradeCurves(grade);
    auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG32Float width:16 height:32 mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:desc];
    if (!texture) return nil;
    [texture replaceRegion:MTLRegionMake2D(0,0,16,32) mipmapLevel:0 withBytes:samples.data() bytesPerRow:16*2*sizeof(float)];
    if (gradeCurveTextures_.size() >= 64) gradeCurveTextures_.erase(gradeCurveTextures_.begin());
    gradeCurveTextures_.emplace(key, texture);
    return texture;
  }
  std::map<std::string,id<MTLTexture>> gradeCubeTextures_;
  id<MTLTexture> gradeCubeTexture(const CompositorColorGrade& grade, size_t index) {
    if (!grade.advanced || grade.advanced->bypass || index >= grade.advanced->operations.size()) return nil;
    const auto& cube = grade.advanced->operations[index].cube;
    if (!cube) return nil;
    const auto found = gradeCubeTextures_.find(cube->hash);
    if (found != gradeCubeTextures_.end()) return found->second;
    MTLTextureDescriptor* desc = [MTLTextureDescriptor new];
    desc.textureType = MTLTextureType3D; desc.pixelFormat = MTLPixelFormatRGBA32Float;
    desc.width = desc.height = desc.depth = cube->size; desc.mipmapLevelCount = 1;
    desc.usage = MTLTextureUsageShaderRead; desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:desc];
    if (!texture) return nil;
    [texture replaceRegion:MTLRegionMake3D(0,0,0,cube->size,cube->size,cube->size) mipmapLevel:0 slice:0 withBytes:cube->rgba.data() bytesPerRow:cube->size*16 bytesPerImage:cube->size*cube->size*16];
    if (gradeCubeTextures_.size() >= 64) gradeCubeTextures_.erase(gradeCubeTextures_.begin());
    gradeCubeTextures_.emplace(cube->hash,texture); return texture;
  }
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLRenderPipelineState> solidPipeline_ = nil;
  id<MTLRenderPipelineState> texturedPipeline_ = nil;
  id<MTLRenderPipelineState> i420Pipeline_ = nil;
  id<MTLRenderPipelineState> overlayPipeline_ = nil;
  id<MTLSamplerState> sampler_ = nil;
  dispatch_semaphore_t inflight_ = dispatch_semaphore_create(3);
  PassTarget program_;
  PassTarget multiview_;
  PassTarget preview_;
  // Canvas dims the draw helpers are currently pointed at (the program pass's
  // dims except inside a secondary pass, which saves/restores them).
  int targetWidth_ = 0;
  int targetHeight_ = 0;
  bool pipelineReady_ = false;
  bool pipelineFailed_ = false;
  bool tilesEffectsAvailable_ = true;
  std::string pipelineError_;
  int64_t frameNumber_ = 0;
  uint64_t fullTickCounter_ = 0;
  id<MTLTexture> previewScaleTex_ = nil;
  int previewScaleWidth_ = 0;
  int previewScaleHeight_ = 0;
  id<MTLTexture> tapTex_ = nil;
  int tapWidth_ = 0;
  int tapHeight_ = 0;
  std::thread tapThread_;
  std::atomic<bool> tapStop_{false};
  dispatch_semaphore_t tapWake_ = nil;
  std::mutex tapMutex_;
  std::vector<uint8_t> tapPublished_;
  uint64_t tapPublishedGeneration_ = 0;
  uint64_t tapConsumedGeneration_ = 0;
  std::vector<uint8_t> readbackBgra_;
#if COREVIDEO_WITH_MAC_VIRTUALCAM
  id<MTLComputePipelineState> vcamPipeline_ = nil;
  id<MTLBuffer> vcamBuffers_[kVcamBufferCount] = {};
  std::atomic<int> vcamPrepState_{kVcamPrepNotStarted};
  std::thread vcamPrepThread_;
  std::shared_ptr<VcamTapShared> vcamShared_ = std::make_shared<VcamTapShared>();
  std::thread vcamThread_;
  std::atomic<bool> vcamMirror_{false};
  std::atomic<bool> vcamSinkPresent_{false};
  std::mutex vcamSinkMutex_;
  VcamFrameSink vcamSink_;
  IdentifiedVcamFrameSink identifiedVcamSink_;
#endif
  std::unordered_map<std::string, SourceTex> sourceTextures_;
  CompositorSourceTexStats stats_;
  // Previous ready raster per holdLastRaster layer (bridged texture pointer
  // kept alive by the raster lease stored beside it).
  compositor::OverlayRasterRetention<const void> retainedOverlayRasters_;
  // Last member: its preparation worker joins first on destruction. It holds
  // its own strong device reference per request, so device_ order is moot.
  CoreTextOverlayRaster overlayRaster_;
};

}  // namespace

std::unique_ptr<ICompositor> createMetalCompositor() {
  return std::make_unique<MetalCompositor>();
}

}  // namespace corevideo::modules

#else  // gate

#include <memory>

#include "modules/Interfaces.h"

namespace corevideo::modules {

std::unique_ptr<ICompositor> createMetalCompositor() {
  return nullptr;
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL
