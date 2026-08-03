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
//  - Overlay layers composite the PORTABLE CPU raster (rasterizeOverlayTileBgra,
//    the deterministic 5x7-font band) uploaded as a texture — premultiplied at
//    upload so the overlay pipeline's premultiplied blend math matches D2D's
//    output on Windows. A CoreText raster is a later increment.
//  - The render target uses storageModeShared (Apple-silicon unified memory),
//    so the CPU readbacks are a plain getBytes after waitUntilCompleted — no
//    staging-texture dance. fullProgramReadback fills ProgramFrame::
//    programFullBgra directly (there is no vcam tap on macOS yet).
//  - programPixelSignature hashes the preview pixels (the stub's semantics)
//    rather than a center-pixel staging read.

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL

#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "compositor/CompositorLayout.h"
#include "compositor/CompositorShaderParams.h"
#include "compositor/MetalCompositorShaders.h"
#include "modules/Interfaces.h"
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
    program_.release();
    multiview_.release();
    preview_.release();
  }

  std::string rendererName() const override { return "metal"; }

  ProgramFrame render(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) override {
    ++frameNumber_;
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
    renderPassInto(program_, deterministicPlan, frames);

    frame.gpuComposed = true;
    frame.sharedTexture.iosurfaceId = program_.iosurfaceId;
    frame.sharedTexture.width = program_.width;
    frame.sharedTexture.height = program_.height;
    frame.sharedTexture.frameNumber = frame.frameNumber;
    if (!renderPlan.skipCpuReadback || renderPlan.fullProgramReadback) {
      readTargetBgra();
      if (!renderPlan.skipCpuReadback) {
        downscaleBgraNearestNeighbor(
            readbackBgra_.data(), targetWidth_, targetHeight_, targetWidth_ * 4, frame.preview);
        frame.programPixelSignature = previewPixelSignature(frame.preview);
      }
      if (renderPlan.fullProgramReadback) {
        frame.programFullBgra.width = targetWidth_;
        frame.programFullBgra.height = targetHeight_;
        frame.programFullBgra.bgra = readbackBgra_;
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

  struct OverlayTex {
    id<MTLTexture> texture;
    uint64_t signature = 0;
    int width = 0;
    int height = 0;
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
    std::fprintf(stderr, "[compositor] Metal pipeline unavailable: %s\n", why.c_str());
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
    NSDictionary* properties = @{
      (__bridge NSString*)kIOSurfaceWidth : @(width),
      (__bridge NSString*)kIOSurfaceHeight : @(height),
      (__bridge NSString*)kIOSurfaceBytesPerElement : @4,
      (__bridge NSString*)kIOSurfacePixelFormat : @((uint32_t)'BGRA'),
    };
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
                      const std::vector<VideoFrame>& frames) {
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
      [commandBuffer commit];
      // Synchronous for now: the program readbacks and the IOSurface consumers
      // (tests today, the shell in Phase 4) both need completed pixels.
      [commandBuffer waitUntilCompleted];
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
    renderPassInto(target, deterministicPlan, frames);
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
    std::fprintf(stderr,
                 "[compositor] layer %s has NO matching frame (available: %s)\n",
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
          layer.color = compositor::colorFromParticipantId(layer.plan.participantId);
          layer.frame = frameForParticipant(frames, layer.plan.participantId);
          if (layer.frame == nullptr) {
            warnUnmatchedCaptureLayer(layer.plan.participantId, frames);
          }
        } else if (!layer.plan.mediaAssetId.empty()) {
          layer.color = compositor::colorFromParticipantId("media:" + layer.plan.mediaAssetId);
          const std::string frameSourceId =
              layer.plan.sourceId.empty() ? "media:" + layer.plan.mediaAssetId : layer.plan.sourceId;
          layer.frame = frameForParticipant(frames, frameSourceId);
          if (layer.frame == nullptr) {
            warnUnmatchedCaptureLayer(frameSourceId, frames);
          }
        } else if (videoIndex > 0 && videoIndex - 1 < static_cast<int>(frames.size())) {
          const auto& fallbackFrame = frames[static_cast<size_t>(videoIndex - 1)];
          layer.color = compositor::colorFromParticipantId(fallbackFrame.participantId);
          if (frameHasContent(fallbackFrame)) {
            layer.frame = &fallbackFrame;
          }
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
      layer.color = compositor::colorFromParticipantId(layer.plan.participantId);
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
    constants.exposure = grade.exposure * 0.1f;
    constants.contrast = grade.contrast * 0.1f;
    constants.saturation = grade.saturation * 0.1f;
    constants.temperature = grade.temperature * 0.1f;
    constants.uvScale[0] = uvScaleX;
    constants.uvScale[1] = uvScaleY;
    constants.uvOffset[0] = uvOffsetX;
    constants.uvOffset[1] = uvOffsetY;
    applyYuvParams(&constants, yuvShaderParamsForFrame(layer.frame));
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
                                                          layer.plan.sourceOffsetY);

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

    // Main content pass: viewport = full rendered source layer; scissor = slot
    // (or the explicit clipRect for remapped composites).
    setViewportFromRect(encoder, imageRect);
    setLayerConstants(encoder, layer, renderPlan, layer.color, layerAlpha, 1.f, 1.f, 0.f, 0.f);

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

    const auto border = compositor::computeBorderFraming(layer.plan.borderStyle, layer.plan.borderColor,
                                                         layer.plan.borderThickness);
    drawBorderPass(encoder, layer, renderPlan, rect, border, layerAlpha);
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

    setScissorFromRect(encoder, clip);
    drawSolidQuad(encoder, layer, renderPlan, animated, background, alpha);

    // Composite the portable CPU raster over the band. Sizing/caching uses the
    // un-animated rect so a build-in/out sweep scales the SAME cached texture.
    if (id<MTLTexture> overlayTex = rasterOverlayTexture(overlay, rect)) {
      setScissorFromRect(encoder, clip);
      setViewportFromRect(encoder, animated);
      setLayerConstants(encoder, layer, renderPlan, 0xffffffffu, alpha, 1.f, 1.f, 0.f, 0.f);
      [encoder setRenderPipelineState:overlayPipeline_];
      [encoder setFragmentTexture:overlayTex atIndex:0];
      [encoder setFragmentSamplerState:sampler_ atIndex:0];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
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

  // CPU raster -> premultiplied BGRA texture, cached by content signature (the
  // signature excludes keyPhase/keyProgress so an animation sweep reuses the
  // cached tile, matching the D3D11 D2D raster cache semantics).
  id<MTLTexture> rasterOverlayTexture(const CompositorOverlayContent& overlay,
                                      const compositor::LayerRect& rect) {
    const int widthPx =
        std::max(2, static_cast<int>(std::lround(rect.width * static_cast<float>(targetWidth_))));
    const int heightPx =
        std::max(2, static_cast<int>(std::lround(rect.height * static_cast<float>(targetHeight_))));
    const uint64_t signature = overlayContentSignature(overlay, widthPx, heightPx);
    if (overlayTex_.texture && overlayTex_.signature == signature &&
        overlayTex_.width == widthPx && overlayTex_.height == heightPx) {
      return overlayTex_.texture;
    }
    std::vector<uint8_t> bgra;
    if (!rasterizeOverlayTileBgra(overlay, widthPx, heightPx, bgra)) {
      return nil;
    }
    // The raster is straight alpha; the overlay pipeline expects premultiplied
    // content (parity with D2D's output on Windows).
    for (size_t offset = 0; offset + 3 < bgra.size(); offset += 4) {
      const uint32_t alpha = bgra[offset + 3];
      bgra[offset + 0] = static_cast<uint8_t>((bgra[offset + 0] * alpha + 127) / 255);
      bgra[offset + 1] = static_cast<uint8_t>((bgra[offset + 1] * alpha + 127) / 255);
      bgra[offset + 2] = static_cast<uint8_t>((bgra[offset + 2] * alpha + 127) / 255);
    }
    id<MTLTexture> texture = makeTexture(MTLPixelFormatBGRA8Unorm, widthPx, heightPx);
    if (!texture) {
      return nil;
    }
    uploadRegion(texture, bgra.data(), widthPx, heightPx, widthPx * 4);
    overlayTex_ = OverlayTex{texture, signature, widthPx, heightPx};
    return texture;
  }

  // ── members ────────────────────────────────────────────────────────────────

  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLRenderPipelineState> solidPipeline_ = nil;
  id<MTLRenderPipelineState> texturedPipeline_ = nil;
  id<MTLRenderPipelineState> i420Pipeline_ = nil;
  id<MTLRenderPipelineState> overlayPipeline_ = nil;
  id<MTLSamplerState> sampler_ = nil;
  PassTarget program_;
  PassTarget multiview_;
  PassTarget preview_;
  // Canvas dims the draw helpers are currently pointed at (the program pass's
  // dims except inside a secondary pass, which saves/restores them).
  int targetWidth_ = 0;
  int targetHeight_ = 0;
  bool pipelineReady_ = false;
  bool pipelineFailed_ = false;
  std::string pipelineError_;
  int64_t frameNumber_ = 0;
  std::vector<uint8_t> readbackBgra_;
  std::unordered_map<std::string, SourceTex> sourceTextures_;
  OverlayTex overlayTex_;
  CompositorSourceTexStats stats_;
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
