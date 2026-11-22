// Metal compositor adapter tests (M1). Compiled only under
// COREVIDEO_WITH_METAL; a missing Metal device is an explicit skip. Shader
// compilation and pipeline failures on an available device must fail.

#if COREVIDEO_WITH_METAL

#include <gtest/gtest.h>

#include <IOSurface/IOSurface.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "compositor/CompositorFaultInjection.h"
#include "compositor/CompositorLayout.h"
#include "compositor/CompositorShaderParams.h"
#include "modules/Interfaces.h"
#include "core/ProgramBlackout.h"
#include "modules/ProgramFramePreview.h"
#include "modules/VirtualCameraFrame.h"

namespace {

using namespace corevideo::modules;

std::unique_ptr<ICompositor> makeCompositorOrSkipReason(std::string& skipReason) {
  auto compositor = createMetalCompositor();
  if (!compositor) {
    skipReason = "createMetalCompositor returned null under COREVIDEO_WITH_METAL";
    return nullptr;
  }
  CompositorRenderPlan probe;
  probe.renderPlanId = "metal-probe";
  const auto frame = compositor->render(probe, {});
  for (const auto& warning : frame.warnings) {
    if (warning.rfind("Metal compositor unavailable", 0) == 0) {
      skipReason = warning;
      return nullptr;
    }
  }
  if (!frame.gpuComposed) {
    skipReason = "Metal compositor did not GPU-compose the probe frame";
    return nullptr;
  }
  return compositor;
}

// The vendored gtest predates GTEST_SKIP, so a machine without a usable Metal
// device logs the reason and early-returns green (the repo's established
// skip pattern — see EncoderRecordingSessionTest).
#define MAKE_COMPOSITOR_OR_SKIP(var)                                          \
  std::string skipReason_;                                                    \
  auto var = makeCompositorOrSkipReason(skipReason_);                         \
  if (!var) {                                                                 \
    std::fprintf(stderr, "[metal-test] unavailable: %s\n", skipReason_.c_str()); \
    ASSERT_TRUE(skipReason_.find("no Metal device") != std::string::npos); \
    return;                                                                   \
  }

std::array<int, 4> previewAt(const ProgramFramePreviewPixels& preview, float fx, float fy) {
  const int x = std::min(preview.width - 1, std::max(0, static_cast<int>(fx * preview.width)));
  const int y = std::min(preview.height - 1, std::max(0, static_cast<int>(fy * preview.height)));
  const size_t offset = (static_cast<size_t>(y) * preview.width + x) * 4u;
  return {preview.bgra[offset + 0], preview.bgra[offset + 1], preview.bgra[offset + 2],
          preview.bgra[offset + 3]};
}

CompositorRenderPlanLayer solidLayer(const std::string& id, float x, float y, float w, float h,
                                     const char* sourceHealth = "") {
  CompositorRenderPlanLayer layer;
  layer.layerId = "layer:" + id;
  layer.kind = "participant-video";
  layer.participantId = id;
  layer.sourceId = "zoom:" + id;
  layer.rect = {x, y, w, h};
  layer.sourceHealth = sourceHealth;
  return layer;
}

VideoFrame bgraFrame(const std::string& participantId, int width, int height,
                     uint8_t b, uint8_t g, uint8_t r) {
  VideoFrame frame;
  frame.participantId = participantId;
  frame.width = width;
  frame.height = height;
  frame.frameId = 1;
  auto pixels = std::make_shared<std::vector<uint8_t>>();
  pixels->resize(static_cast<size_t>(width) * height * 4u);
  for (size_t offset = 0; offset < pixels->size(); offset += 4) {
    (*pixels)[offset + 0] = b;
    (*pixels)[offset + 1] = g;
    (*pixels)[offset + 2] = r;
    (*pixels)[offset + 3] = 0xff;
  }
  frame.pixels = std::move(pixels);
  frame.pixelWidth = width;
  frame.pixelHeight = height;
  frame.pixelStride = width * 4;
  return frame;
}

VideoFrame i420Frame(const std::string& participantId, int width, int height,
                     uint8_t yValue, uint8_t uValue, uint8_t vValue,
                     bool fullRange, bool bt601) {
  VideoFrame frame;
  frame.participantId = participantId;
  frame.width = width;
  frame.height = height;
  frame.frameId = 1;
  const size_t yLen = static_cast<size_t>(width) * height;
  auto planes = std::make_shared<std::vector<uint8_t>>();
  planes->resize(yLen + (yLen / 4) * 2);
  std::fill(planes->begin(), planes->begin() + yLen, yValue);
  std::fill(planes->begin() + yLen, planes->begin() + yLen + yLen / 4, uValue);
  std::fill(planes->begin() + yLen + yLen / 4, planes->end(), vValue);
  frame.i420 = std::move(planes);
  frame.i420Width = width;
  frame.i420Height = height;
  frame.i420FullRange = fullRange;
  frame.i420Bt601 = bt601;
  return frame;
}

// CPU mirror of the I420 shader math (no grade), for expected-value checks.
std::array<int, 3> expectedRgbForYuv(uint8_t yValue, uint8_t uValue, uint8_t vValue,
                                     bool fullRange, bool bt601) {
  VideoFrame probe = i420Frame("probe", 2, 2, yValue, uValue, vValue, fullRange, bt601);
  const auto params = yuvShaderParamsForFrame(&probe);
  const float Y = (yValue / 255.f - params.yOffset) * params.yScale;
  const float U = (uValue / 255.f - 0.5f) * params.chromaScale;
  const float V = (vValue / 255.f - 0.5f) * params.chromaScale;
  const auto clamp01 = [](float v) { return std::min(1.f, std::max(0.f, v)); };
  const float r = clamp01(Y + params.rV * V);
  const float g = clamp01(Y - params.gU * U - params.gV * V);
  const float b = clamp01(Y + params.bU * U);
  return {static_cast<int>(std::lround(r * 255.f)), static_cast<int>(std::lround(g * 255.f)),
          static_cast<int>(std::lround(b * 255.f))};
}

TEST(MetalCompositor, RendersDeterministicallyAndReportsMetalRenderer) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "plan-deterministic";
  // bus health on air (#535 slice 4a): both layers are frameless, so without
  // a health distinction they would both resolve to the SAME warming slate
  // and EXPECT_NE below would pass whether or not the two cells actually
  // render distinctly. "beta" is marked sourceHealth="failed" (->
  // kFailedSlateRgba) so the two placeholders stay visually distinct by
  // health, not by a per-id colour.
  plan.layers.push_back(solidLayer("alpha", 0.0f, 0.0f, 0.5f, 1.0f));
  plan.layers.push_back(solidLayer("beta", 0.5f, 0.0f, 0.5f, 1.0f, "failed"));

  const auto first = compositor->render(plan, {});
  const auto second = compositor->render(plan, {});
  EXPECT_EQ(first.renderer, "metal");
  EXPECT_TRUE(first.gpuComposed);
  EXPECT_GT(first.preview.width, 0);
  EXPECT_EQ(first.programPixelSignature, second.programPixelSignature);
  // The two participants must render DIFFERENT deterministic colors.
  const auto left = previewAt(first.preview, 0.25f, 0.5f);
  const auto right = previewAt(first.preview, 0.75f, 0.5f);
  EXPECT_NE(left, right);
  // Name what they now are: the warming slate and the failed slate.
  const uint32_t warming = ::corevideo::compositor::kWarmingSlateRgba;
  const uint32_t failed = ::corevideo::compositor::kFailedSlateRgba;
  EXPECT_NEAR(left[2], static_cast<int>((warming >> 16) & 0xff), 2);
  EXPECT_NEAR(left[1], static_cast<int>((warming >> 8) & 0xff), 2);
  EXPECT_NEAR(left[0], static_cast<int>(warming & 0xff), 2);
  EXPECT_NEAR(right[2], static_cast<int>((failed >> 16) & 0xff), 2);
  EXPECT_NEAR(right[1], static_cast<int>((failed >> 8) & 0xff), 2);
  EXPECT_NEAR(right[0], static_cast<int>(failed & 0xff), 2);
}

TEST(MetalCompositor, SolidFallbackMatchesCpuPreviewColors) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "plan-cpu-parity";
  plan.layers.push_back(solidLayer("parity-a", 0.0f, 0.0f, 0.5f, 1.0f));
  plan.layers.push_back(solidLayer("parity-b", 0.5f, 0.0f, 0.5f, 1.0f));

  const auto gpu = compositor->render(plan, {});

  ProgramFrame cpuFrame;
  cpuFrame.width = plan.width;
  cpuFrame.height = plan.height;
  ProgramFramePreviewPixels cpuPreview;
  fillSyntheticProgramFramePreview(cpuPreview, sortCompositorRenderPlan(plan), {}, cpuFrame);

  for (const float fx : {0.25f, 0.75f}) {
    const auto gpuPixel = previewAt(gpu.preview, fx, 0.5f);
    const auto cpuPixel = previewAt(cpuPreview, fx, 0.5f);
    for (int channel = 0; channel < 3; ++channel) {
      EXPECT_NEAR(gpuPixel[channel], cpuPixel[channel], 2)
          << "fx=" << fx << " channel=" << channel;
    }
  }
}

TEST(MetalCompositor, BgraFrameRendersItsPixels) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "plan-bgra";
  plan.layers.push_back(solidLayer("cam", 0.0f, 0.0f, 1.0f, 1.0f));
  const auto frame = bgraFrame("cam", 320, 180, 10, 200, 60);

  const auto gpu = compositor->render(plan, {frame});
  const auto pixel = previewAt(gpu.preview, 0.5f, 0.5f);
  EXPECT_NEAR(pixel[0], 10, 2);
  EXPECT_NEAR(pixel[1], 200, 2);
  EXPECT_NEAR(pixel[2], 60, 2);
}

TEST(MetalCompositor, AdvancedPrimariesCurvesCubeAndBypassAffectNativePixels) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "advanced-metal";
  plan.layers.push_back(solidLayer("cam", 0, 0, 1, 1));
  plan.layers[0].hasColorGrade = true;
  const auto source = bgraFrame("cam", 320, 180, 153, 102, 51);
  auto check = [&](const std::shared_ptr<AdvancedGradeDocument>& doc,
                   int r, int g, int b) {
    plan.layers[0].colorGrade.advanced = doc;
    const auto rendered = compositor->render(plan, {source});
    ASSERT_TRUE(rendered.gpuComposed);
    const auto pixel = previewAt(rendered.preview, .5f, .5f);
    EXPECT_NEAR(pixel[2], r, 3);
    EXPECT_NEAR(pixel[1], g, 3);
    EXPECT_NEAR(pixel[0], b, 3);
  };
  auto primaries = std::make_shared<AdvancedGradeDocument>();
  primaries->content = "metal-gray-primary";
  GradeOperation primary;
  primary.kind = "primaries"; primary.saturation = 0;
  primaries->operations.push_back(primary);
  // Independent Rec.709 luma: .2126*.2 + .7152*.4 + .0722*.6.
  check(primaries, 95, 95, 95);

  auto curve = std::make_shared<AdvancedGradeDocument>();
  curve->content = "metal-master-half";
  GradeOperation operation;
  operation.kind = "curves";
  operation.curves = {{{0,0},{1,.5f}}, {{0,0},{1,1}},
                      {{0,0},{1,1}}, {{0,0},{1,1}}};
  curve->operations.push_back(operation);
  check(curve, 26, 51, 77);

  auto cube = std::make_shared<GradeCubeLut>();
  cube->size = 2; cube->hash = "metal-red-blue-swap";
  for (int b=0; b<2; ++b) for (int g=0; g<2; ++g) for (int r=0; r<2; ++r)
    cube->rgba.insert(cube->rgba.end(), {float(b),float(g),float(r),1.f});
  auto lut = std::make_shared<AdvancedGradeDocument>();
  lut->content = "metal-cube-swap";
  operation = {}; operation.kind = "cube"; operation.cube = cube;
  lut->operations.push_back(operation);
  check(lut, 153, 102, 51);
  lut->bypass = true;
  check(lut, 51, 102, 153);

  // The same primaries shader also consumes normalized Zoom I420 pixels.
  plan.layers[0].participantId = "zoom-guest";
  plan.layers[0].colorGrade.advanced = primaries;
  const auto yuv = i420Frame("zoom-guest",320,180,120,90,190,true,false);
  const auto rgb = expectedRgbForYuv(120,90,190,true,false);
  const auto rendered = compositor->render(plan,{yuv});
  ASSERT_TRUE(rendered.gpuComposed);
  const auto pixel = previewAt(rendered.preview,.5f,.5f);
  const int gray = int(std::lround(.2126*rgb[0]+.7152*rgb[1]+.0722*rgb[2]));
  for(int channel=0;channel<3;++channel) EXPECT_NEAR(pixel[channel],gray,4);
}

// Grading plan 2026-11-16, Task 3 (G1 pixel proof). Expected values are worked by hand from
// applyGrade (MetalCompositorShaders.h) with the 0.1 axis scale and look offsets from
// ColorGradeParams.h; see the plan's table. The neutral look moves a patch by at most 2.55
// levels, so it is asserted at tolerance 1; every other row at 2.
TEST(MetalCompositor, BasicGradeAxesAndNamedLooksMoveAKnownPatch) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  struct Case { const char* name; uint8_t r, g, b; const char* lut; float e, c, s, t; int er, eg, eb, tol; };
  const Case cases[] = {
      {"identity",        192, 96, 64,  "none",            0, 0, 0, 0,  192, 96, 64,  2},
      {"exposure+2",      192, 96, 64,  "none",            2, 0, 0, 0,  243, 147, 115, 2},
      {"contrast+5",      192, 96, 64,  "none",            0, 5, 0, 0,  224, 80, 32,  2},
      {"saturation+5",    192, 96, 64,  "none",            0, 0, 5, 0,  227, 83, 35,  2},
      {"temperature+20",  192, 96, 64,  "none",            0, 0, 0, 20, 218, 96, 38,  2},
      {"warm-film",       192, 96, 64,  "warm-film",       0, 0, 0, 0,  201, 93, 56,  2},
      {"punch",           192, 96, 64,  "punch",           0, 0, 0, 0,  212, 88, 46,  2},
      {"cool-broadcast",  200, 60, 230, "cool-broadcast",  0, 0, 0, 0,  201, 57, 236, 2},
      {"neutral",         250, 128, 6,  "neutral",         0, 0, 0, 0,  252, 128, 4,  1},
  };
  int64_t nextFrameId = 0;
  for (const auto& c : cases) {
    CompositorRenderPlan plan;
    plan.renderPlanId = std::string("basic-") + c.name;
    plan.layers.push_back(solidLayer("cam", 0, 0, 1, 1));
    plan.layers[0].hasColorGrade = true;
    plan.layers[0].colorGrade = CompositorColorGrade{c.e, c.c, c.s, c.t, c.lut};
    // A new frameId per case: the adapter re-uploads a source texture only when the frame id
    // moves (MetalCompositorAdapter.mm acquireSourceTex), and a patch change must reach the GPU.
    auto source = bgraFrame("cam", 320, 180, c.b, c.g, c.r);
    source.frameId = ++nextFrameId;
    const auto rendered = compositor->render(plan, {source});
    ASSERT_TRUE(rendered.gpuComposed) << c.name;
    const auto px = previewAt(rendered.preview, .5f, .5f);   // B, G, R, A
    EXPECT_NEAR(px[2], c.er, c.tol) << c.name << " R";
    EXPECT_NEAR(px[1], c.eg, c.tol) << c.name << " G";
    EXPECT_NEAR(px[0], c.eb, c.tol) << c.name << " B";
  }
}

TEST(MetalCompositor, APerLayerGradeReplacesTheProgramLookOnItsLayerOnly) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "per-layer";
  plan.layers.push_back(solidLayer("left", 0, 0, .5f, 1));
  plan.layers.push_back(solidLayer("right", .5f, 0, .5f, 1));
  const auto left = bgraFrame("left", 160, 180, 64, 96, 192);
  const auto right = bgraFrame("right", 160, 180, 64, 96, 192);

  // 1. A graded layer beside an ungraded one, no Program look.
  plan.layers[0].hasColorGrade = true;
  plan.layers[0].colorGrade = CompositorColorGrade{2, 0, 0, 0, "none"};
  auto out = compositor->render(plan, {left, right});
  auto l = previewAt(out.preview, .25f, .5f), r = previewAt(out.preview, .75f, .5f);
  EXPECT_NEAR(l[2], 243, 2); EXPECT_NEAR(l[1], 147, 2); EXPECT_NEAR(l[0], 115, 2);
  EXPECT_NEAR(r[2], 192, 2); EXPECT_NEAR(r[1], 96, 2);  EXPECT_NEAR(r[0], 64, 2);

  // 2. Program look on, left layer carries a NEUTRAL per-layer grade: left ignores the look
  //    (the D1/D6 copy depends on this), right takes it.
  plan.colorGrade = CompositorColorGrade{2, 0, 0, 0, "none"};
  plan.layers[0].colorGrade = CompositorColorGrade{};
  out = compositor->render(plan, {left, right});
  l = previewAt(out.preview, .25f, .5f); r = previewAt(out.preview, .75f, .5f);
  EXPECT_NEAR(l[2], 192, 2); EXPECT_NEAR(l[1], 96, 2);  EXPECT_NEAR(l[0], 64, 2);
  EXPECT_NEAR(r[2], 243, 2); EXPECT_NEAR(r[1], 147, 2); EXPECT_NEAR(r[0], 115, 2);
}

TEST(MetalCompositor, I420FullRangeBt709ConvertMatchesReference) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "plan-i420-709";
  plan.layers.push_back(solidLayer("zoom-guest", 0.0f, 0.0f, 1.0f, 1.0f));
  const auto frame = i420Frame("zoom-guest", 320, 180, 120, 90, 190, true, false);

  const auto gpu = compositor->render(plan, {frame});
  const auto pixel = previewAt(gpu.preview, 0.5f, 0.5f);
  const auto expected = expectedRgbForYuv(120, 90, 190, true, false);
  EXPECT_NEAR(pixel[2], expected[0], 3);  // R
  EXPECT_NEAR(pixel[1], expected[1], 3);  // G
  EXPECT_NEAR(pixel[0], expected[2], 3);  // B
}

TEST(MetalCompositor, I420StudioSwingBt601ExpandsRange) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "plan-i420-601";
  plan.layers.push_back(solidLayer("uvc-cam", 0.0f, 0.0f, 1.0f, 1.0f));
  // Studio-swing black (Y=16) must expand to full black, not washed-out gray.
  const auto frame = i420Frame("uvc-cam", 320, 180, 16, 128, 128, false, true);

  const auto gpu = compositor->render(plan, {frame});
  const auto pixel = previewAt(gpu.preview, 0.5f, 0.5f);
  EXPECT_LE(pixel[0], 3);
  EXPECT_LE(pixel[1], 3);
  EXPECT_LE(pixel[2], 3);
}

TEST(MetalCompositor, OverlayRastersNonUniformBand) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "plan-overlay";
  CompositorRenderPlanLayer overlay;
  overlay.layerId = "overlay:lower-third";
  overlay.kind = "overlay-lower-third";
  overlay.rect = {0.05f, 0.78f, 0.55f, 0.16f};
  overlay.hasOverlayContent = true;
  overlay.overlay.title = "ADA OTIENO";
  overlay.overlay.org = "AETHELRED LABS";
  overlay.overlay.keyPhase = "on-air";
  overlay.overlay.keyProgress = 1.f;
  plan.layers.push_back(overlay);

  // G1: the overlay raster prepares off the render tick, so the first frames
  // legitimately draw the band + accent fallback. Sample once it is ready.
  ProgramFrame gpu;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  do {
    gpu = compositor->render(plan, {});
    const bool preparing = std::find(gpu.warnings.begin(), gpu.warnings.end(), "graphics-preparing") !=
                           gpu.warnings.end();
    if (!preparing) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  } while (std::chrono::steady_clock::now() < deadline);
  // The overlay band must contain more than two distinct colors (background +
  // band + rastered text pixels), proving the raster composited.
  std::set<std::array<int, 4>> colors;
  for (float fx = 0.08f; fx < 0.55f; fx += 0.02f) {
    for (float fy = 0.79f; fy < 0.93f; fy += 0.02f) {
      colors.insert(previewAt(gpu.preview, fx, fy));
    }
  }
  EXPECT_GT(colors.size(), 2u);
}

TEST(MetalCompositor, ClipRectConfinesLayerPixels) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "plan-clip";
  auto layer = solidLayer("clipped", 0.0f, 0.0f, 1.0f, 1.0f);
  layer.hasClipRect = true;
  layer.clipRect = {0.0f, 0.0f, 0.5f, 1.0f};
  plan.layers.push_back(layer);
  const auto frame = bgraFrame("clipped", 64, 36, 0, 0, 255);

  const auto gpu = compositor->render(plan, {frame});
  const auto inside = previewAt(gpu.preview, 0.25f, 0.5f);
  const auto outside = previewAt(gpu.preview, 0.75f, 0.5f);
  EXPECT_NEAR(inside[2], 255, 2);
  // Outside the clip the background clear color remains (dark, red channel low).
  EXPECT_LT(outside[2], 40);
}

TEST(MetalCompositor, SourceTexCacheUploadsOncePerFrameId) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "plan-cache";
  plan.layers.push_back(solidLayer("cached", 0.0f, 0.0f, 1.0f, 1.0f));
  auto frame = bgraFrame("cached", 64, 36, 1, 2, 3);

  (void)compositor->render(plan, {frame});
  const auto afterFirst = compositor->sourceTexStats();
  EXPECT_EQ(afterFirst.cachedUploads, 1u);

  (void)compositor->render(plan, {frame});
  const auto afterRepeat = compositor->sourceTexStats();
  EXPECT_EQ(afterRepeat.cachedUploads, 1u);
  EXPECT_GE(afterRepeat.cacheHits, 1u);

  frame.frameId = 2;
  (void)compositor->render(plan, {frame});
  const auto afterAdvance = compositor->sourceTexStats();
  EXPECT_EQ(afterAdvance.cachedUploads, 2u);
}

// Reads one pixel {b,g,r,a} out of an IOSurface by its global ID — the same
// resolve path a Phase 4 shell will use (IOSurfaceLookup + lock + read).
std::array<int, 4> iosurfacePixel(uint32_t iosurfaceId, float fx, float fy, bool& ok) {
  ok = false;
  IOSurfaceRef surface = IOSurfaceLookup(iosurfaceId);
  if (!surface) {
    return {0, 0, 0, 0};
  }
  IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
  const int width = static_cast<int>(IOSurfaceGetWidth(surface));
  const int height = static_cast<int>(IOSurfaceGetHeight(surface));
  const size_t stride = IOSurfaceGetBytesPerRow(surface);
  const auto* base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
  std::array<int, 4> pixel{0, 0, 0, 0};
  if (base && width > 0 && height > 0) {
    const int x = std::min(width - 1, static_cast<int>(fx * width));
    const int y = std::min(height - 1, static_cast<int>(fy * height));
    const uint8_t* p = base + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
    pixel = {p[0], p[1], p[2], p[3]};
    ok = true;
  }
  IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
  CFRelease(surface);
  return pixel;
}

TEST(MetalCompositor, ProgramSharedTextureCarriesStableIOSurfaceId) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "plan-iosurface";
  plan.layers.push_back(solidLayer("io", 0.0f, 0.0f, 1.0f, 1.0f));

  const auto first = compositor->render(plan, {});
  const auto second = compositor->render(plan, {});
  ASSERT_NE(first.sharedTexture.iosurfaceId, 0u);
  EXPECT_EQ(first.sharedTexture.iosurfaceId, second.sharedTexture.iosurfaceId);
  EXPECT_EQ(first.sharedTexture.width, plan.width);
  EXPECT_EQ(first.sharedTexture.height, plan.height);
  EXPECT_TRUE(first.sharedTexture.sharedHandleHex.empty());

  // The program pixels must be readable THROUGH the IOSurface by global ID.
  bool ok = false;
  const auto viaSurface = iosurfacePixel(first.sharedTexture.iosurfaceId, 0.5f, 0.5f, ok);
  ASSERT_TRUE(ok);
  const auto viaPreview = previewAt(second.preview, 0.5f, 0.5f);
  for (int channel = 0; channel < 3; ++channel) {
    EXPECT_NEAR(viaSurface[channel], viaPreview[channel], 2);
  }
}

TEST(MetalCompositor, MultiviewPassRendersIntoItsOwnSurface) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  // Program renders one participant; the multiview plan shows two cells with
  // distinct colors. The two passes must not bleed into each other.
  CompositorRenderPlan programPlan;
  programPlan.renderPlanId = "plan-program";
  programPlan.layers.push_back(solidLayer("solo", 0.0f, 0.0f, 1.0f, 1.0f));
  (void)compositor->render(programPlan, {});

  // bus health on air (#535 slice 4a): both cells are frameless, so without
  // a health distinction they would both resolve to the SAME warming slate
  // and EXPECT_NE below would pass whether or not the two cells actually
  // render distinctly. "cell-b" is marked sourceHealth="failed" (->
  // kFailedSlateRgba) so the two placeholders stay visually distinct by
  // health, not by a per-id colour.
  CompositorRenderPlan multiviewPlan;
  multiviewPlan.renderPlanId = "plan-multiview";
  multiviewPlan.layers.push_back(solidLayer("cell-a", 0.0f, 0.0f, 0.5f, 0.5f));
  multiviewPlan.layers.push_back(solidLayer("cell-b", 0.5f, 0.0f, 0.5f, 0.5f, "failed"));

  const auto texture = compositor->renderMultiview(multiviewPlan, {});
  ASSERT_NE(texture.iosurfaceId, 0u);
  EXPECT_EQ(texture.width, multiviewPlan.width);
  EXPECT_EQ(texture.height, multiviewPlan.height);

  bool okA = false;
  bool okB = false;
  const auto cellA = iosurfacePixel(texture.iosurfaceId, 0.25f, 0.25f, okA);
  const auto cellB = iosurfacePixel(texture.iosurfaceId, 0.75f, 0.25f, okB);
  ASSERT_TRUE(okA);
  ASSERT_TRUE(okB);
  EXPECT_NE(cellA, cellB);
  // Name what they now are: the warming slate and the failed slate.
  const uint32_t warming = ::corevideo::compositor::kWarmingSlateRgba;
  const uint32_t failed = ::corevideo::compositor::kFailedSlateRgba;
  EXPECT_NEAR(cellA[2], static_cast<int>((warming >> 16) & 0xff), 2);
  EXPECT_NEAR(cellA[1], static_cast<int>((warming >> 8) & 0xff), 2);
  EXPECT_NEAR(cellA[0], static_cast<int>(warming & 0xff), 2);
  EXPECT_NEAR(cellB[2], static_cast<int>((failed >> 16) & 0xff), 2);
  EXPECT_NEAR(cellB[1], static_cast<int>((failed >> 8) & 0xff), 2);
  EXPECT_NEAR(cellB[0], static_cast<int>(failed & 0xff), 2);

  // The program pass afterwards is unaffected by the secondary pass (dims
  // restored, its own surface untouched).
  const auto after = compositor->render(programPlan, {});
  EXPECT_EQ(after.sharedTexture.width, programPlan.width);
  EXPECT_NE(after.sharedTexture.iosurfaceId, texture.iosurfaceId);
}

TEST(MetalCompositor, MultiviewBordersRenderInMultiviewOnly) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  // The owner rule: borders exist in the MULTIVIEW pass; the plan builder
  // forces them off for program. At compositor level: a multiview layer with
  // an explicit border style renders visible border pixels along its edge.
  CompositorRenderPlan plan;
  plan.renderPlanId = "plan-mv-borders";
  auto cell = solidLayer("speaker", 0.1f, 0.1f, 0.5f, 0.5f);
  cell.borderStyle = "program";
  cell.borderColor = "#f5a623";
  cell.borderThickness = 6.f;
  plan.layers.push_back(cell);

  const auto texture = compositor->renderMultiview(plan, {});
  ASSERT_NE(texture.iosurfaceId, 0u);
  bool ok = false;
  // Sample just inside the cell's top edge, where the border stroke lands.
  const auto edge = iosurfacePixel(texture.iosurfaceId, 0.35f, 0.105f, ok);
  ASSERT_TRUE(ok);
  // #f5a623 in BGRA: strong red+green, low blue.
  EXPECT_GT(edge[2], 180);
  EXPECT_GT(edge[1], 120);
  EXPECT_LT(edge[0], 90);
}

TEST(MetalCompositor, PreviewPassIsIndependentOfProgramAndMultiview) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan previewPlan;
  previewPlan.renderPlanId = "plan-preview-bus";
  previewPlan.layers.push_back(solidLayer("preview-only", 0.0f, 0.0f, 1.0f, 1.0f));

  const auto texture = compositor->renderPreview(previewPlan, {});
  ASSERT_NE(texture.iosurfaceId, 0u);
  bool ok = false;
  const auto pixel = iosurfacePixel(texture.iosurfaceId, 0.5f, 0.5f, ok);
  ASSERT_TRUE(ok);
  // bus health on air (#535 slice 4a): "preview-only" has no frame and no
  // sourceHealth set by this hand-built plan, so it reads "" -> the warming
  // slate, not the clear color and not a per-id colour.
  const uint32_t expected = ::corevideo::compositor::kWarmingSlateRgba;
  EXPECT_NEAR(pixel[2], static_cast<int>((expected >> 16) & 0xff), 2);
  EXPECT_NEAR(pixel[1], static_cast<int>((expected >> 8) & 0xff), 2);
  EXPECT_NEAR(pixel[0], static_cast<int>(expected & 0xff), 2);

  // All three passes own DISTINCT surfaces.
  CompositorRenderPlan programPlan;
  programPlan.renderPlanId = "plan-any";
  const auto program = compositor->render(programPlan, {});
  const auto multiview = compositor->renderMultiview(previewPlan, {});
  EXPECT_NE(texture.iosurfaceId, program.sharedTexture.iosurfaceId);
  EXPECT_NE(texture.iosurfaceId, multiview.iosurfaceId);
  EXPECT_NE(program.sharedTexture.iosurfaceId, multiview.iosurfaceId);
}

TEST(MetalCompositor, SharedTextureJsonCarriesIOSurfaceId) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "plan-json";
  plan.layers.push_back(solidLayer("json", 0.0f, 0.0f, 1.0f, 1.0f));
  const auto frame = compositor->render(plan, {});

  const auto json = programSharedTextureJson(frame);
  ASSERT_FALSE(json.isNull());
  ASSERT_NE(json.get("iosurfaceId"), nullptr);
  EXPECT_EQ(static_cast<uint32_t>(json.get("iosurfaceId")->asNumber()),
            frame.sharedTexture.iosurfaceId);
  EXPECT_EQ(json.getString("format"), "B8G8R8A8_UNORM");
}

TEST(MetalCompositor, LayerOpacityBlendsTowardBackground) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "plan-opacity";
  auto layer = solidLayer("half", 0.0f, 0.0f, 1.0f, 1.0f);
  layer.opacity = 0.5f;
  plan.layers.push_back(layer);
  const auto frame = bgraFrame("half", 64, 36, 0, 0, 255);

  const auto gpu = compositor->render(plan, {frame});
  const auto pixel = previewAt(gpu.preview, 0.5f, 0.5f);
  // 50% red over the dark background: red lands mid-range, neither ~24 nor ~255.
  EXPECT_GT(pixel[2], 80);
  EXPECT_LT(pixel[2], 200);
}

// Port of D3D11Compositor.FadeToBlackProducesBlackPixelsAndRestoresHeldVideo.
// ProgramBlackout appends a sourceless hasFillColor "#000000" layer; before the
// Metal hasFillColor branch it fell through to the default 0xff444444 grey.
TEST(MetalCompositor, FadeToBlackProducesBlackPixelsAndRestoresHeldVideo) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "plan-ftb";
  plan.width = 640;
  plan.height = 360;
  plan.layers.push_back(solidLayer("camera", 0.f, 0.f, 1.f, 1.f));
  const auto source = bgraFrame("camera", 64, 36, 0xee, 0xee, 0xee);
  const auto baseline = compositor->render(plan, {source});
  ASSERT_FALSE(baseline.preview.bgra.empty());
  const auto basePixel = previewAt(baseline.preview, 0.5f, 0.5f);
  EXPECT_GT(basePixel[2], 200);

  corevideo::core::ProgramBlackout fade;
  fade.set(true);
  fade.advance(150);
  auto half = plan;
  fade.append(half);
  const auto middle = previewAt(compositor->render(half, {source}).preview, 0.5f, 0.5f);
  // Opacity 0.5 of black over ~0xee: roughly half brightness.
  for (int c = 0; c < 3; ++c) {
    EXPECT_GT(middle[c], 105) << "channel " << c;
    EXPECT_LT(middle[c], 135) << "channel " << c;
  }

  fade.advance(150);
  auto black = plan;
  fade.append(black);
  const auto full = compositor->render(black, {source});
  ASSERT_FALSE(full.preview.bgra.empty());
  const auto pixel = previewAt(full.preview, 0.5f, 0.5f);
  for (int c = 0; c < 3; ++c) EXPECT_LE(pixel[c], 2) << "channel " << c << " (grey 0x44 = 68)";

  fade.set(false);
  fade.advance(300);
  auto restored = plan;
  fade.append(restored);
  EXPECT_EQ(compositor->render(restored, {source}).preview.bgra, baseline.preview.bgra);
}

TEST(MetalCompositor, SourcelessFillColorLayerPaintsItsParsedColour) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "plan-fill";
  plan.width = 640;
  plan.height = 360;
  CompositorRenderPlanLayer bg;
  bg.layerId = "tiles-bg:test";
  bg.kind = "tiles-background";
  bg.rect = {0.f, 0.f, 1.f, 1.f};
  bg.hasFillColor = true;
  bg.fillColor = "#3366CC";
  plan.layers.push_back(bg);
  const auto pixel = previewAt(compositor->render(plan, {}).preview, 0.5f, 0.5f);
  EXPECT_NEAR(pixel[2], 0x33, 2);  // R
  EXPECT_NEAR(pixel[1], 0x66, 2);  // G
  EXPECT_NEAR(pixel[0], 0xCC, 2);  // B
}

// ── Overlay raster off the render tick (Metal graphics backend G1) ──────────
//
// The overlay raster prepares on a worker; the render thread only adopts a
// published texture. The prepare hook (CompositorFaultInjection.h) counts
// every preparation and can BLOCK the worker (with a hard 1.5 s timeout, so a
// broken test cannot wedge the binary). Canvas 320x180 makes the 320x180
// preview readback pixel-exact, so every pixel assertion below is in canvas
// pixels computed from computeOverlayTileLayout's fractions.

std::atomic<int> gOverlayPrepares{0};
std::atomic<bool> gOverlayPrepareBlocked{false};
std::mutex gOverlayPrepareMutex;
std::condition_variable gOverlayPrepareCv;

void overlayPrepareHook() {
  gOverlayPrepares.fetch_add(1);
  std::unique_lock<std::mutex> lock(gOverlayPrepareMutex);
  gOverlayPrepareCv.wait_for(lock, std::chrono::milliseconds(1500),
                             [] { return !gOverlayPrepareBlocked.load(); });
}

// Declare AFTER the compositor: destroyed first, so a blocked worker is
// released before the compositor joins it.
struct OverlayPrepareHookScope {
  explicit OverlayPrepareHookScope(bool block) {
    gOverlayPrepares = 0;
    gOverlayPrepareBlocked = block;
    corevideo::compositor::setOverlayRasterPrepareHookForTest(&overlayPrepareHook);
  }
  void block() { gOverlayPrepareBlocked = true; }
  void release() {
    {
      std::lock_guard<std::mutex> lock(gOverlayPrepareMutex);
      gOverlayPrepareBlocked = false;
    }
    gOverlayPrepareCv.notify_all();
  }
  ~OverlayPrepareHookScope() {
    release();
    corevideo::compositor::setOverlayRasterPrepareHookForTest(nullptr);
  }
};

bool hasWarning(const ProgramFrame& frame, const std::string& warning) {
  return std::find(frame.warnings.begin(), frame.warnings.end(), warning) != frame.warnings.end();
}

std::array<int, 4> canvasPixel(const ProgramFramePreviewPixels& preview, int x, int y) {
  const size_t offset = (static_cast<size_t>(y) * preview.width + x) * 4u;
  return {preview.bgra[offset + 0], preview.bgra[offset + 1], preview.bgra[offset + 2],
          preview.bgra[offset + 3]};
}

bool bright(const std::array<int, 4>& p) { return p[0] > 200 && p[1] > 200 && p[2] > 200; }

CompositorRenderPlanLayer overlayLayer(const std::string& id, CompositorLayerRect rect,
                                       const std::string& name, const std::string& title) {
  CompositorRenderPlanLayer layer;
  layer.layerId = "overlay:" + id;
  layer.kind = "overlay-lower-third";
  layer.rect = rect;
  layer.order = 10;
  layer.hasOverlayContent = true;
  layer.overlay.text = name;
  layer.overlay.title = title;
  layer.overlay.brandColor = "#ff0000";
  layer.overlay.brandBackgroundColor = "#0c1118";
  layer.overlay.keyPhase = "on-air";
  layer.overlay.keyProgress = 1.f;
  return layer;
}

CompositorRenderPlan overlayPlan(const std::string& id) {
  CompositorRenderPlan plan;
  plan.renderPlanId = id;
  plan.width = 320;
  plan.height = 180;
  auto bg = solidLayer("bg", 0.f, 0.f, 1.f, 1.f);
  bg.order = 0;
  plan.layers.push_back(bg);
  return plan;
}

ProgramFrame renderUntilOverlayReady(ICompositor& compositor, const CompositorRenderPlan& plan,
                                     const std::vector<VideoFrame>& frames) {
  ProgramFrame frame;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  do {
    frame = compositor.render(plan, frames);
    if (!hasWarning(frame, "graphics-preparing")) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  } while (std::chrono::steady_clock::now() < deadline);
  return frame;
}

// Name-line rect in canvas pixels for an overlay at canvas rect (x0, y0, w, h)
// (OverlayTileRaster.cpp lower-third fractions: x 0.06..0.88, y 0.18..0.52).
struct PixelRect { int x0, y0, x1, y1; };
PixelRect nameLine(int x0, int y0, int w, int h) {
  return {x0 + int(w * 0.06f), y0 + int(h * 0.18f), x0 + int(w * 0.88f), y0 + int(h * 0.52f)};
}
int brightIn(const ProgramFramePreviewPixels& preview, PixelRect r) {
  int count = 0;
  for (int y = r.y0; y < r.y1; ++y)
    for (int x = r.x0; x < r.x1; ++x)
      if (bright(canvasPixel(preview, x, y))) ++count;
  return count;
}

TEST(MetalCompositor, ABlockedOverlayPreparationNeverStallsTheRenderTick) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  OverlayPrepareHookScope hook(/*block=*/true);
  auto plan = overlayPlan("plan-overlay-blocked");
  plan.layers.push_back(overlayLayer("lt", {0.f, 0.5f, 1.f, 0.5f}, "ADA OTIENO", "AETHELRED LABS"));
  const std::vector<VideoFrame> frames{bgraFrame("bg", 64, 36, 0, 0, 0)};
  const auto name = nameLine(0, 90, 320, 90);
  double worstMs = 0;
  for (int i = 0; i < 20; ++i) {
    const auto start = std::chrono::steady_clock::now();
    const auto frame = compositor->render(plan, frames);
    worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - start).count());
    ASSERT_TRUE(hasWarning(frame, "graphics-preparing")) << "tick " << i;
    EXPECT_EQ(frame.health, "degraded");
    // Fallback while preparing: the brand accent bar (left 4%), and no text.
    const auto bar = canvasPixel(frame.preview, 3, 135);
    EXPECT_GT(bar[2], 200);
    EXPECT_LT(bar[1], 40);
    EXPECT_EQ(brightIn(frame.preview, name), 0) << "tick " << i << " drew text it cannot have yet";
  }
  std::fprintf(stderr, "[metal-test] blocked overlay preparation: worst tick %.3f ms\n", worstMs);
  EXPECT_LT(worstMs, 250.0) << "a render tick waited for the overlay raster";
  EXPECT_GE(gOverlayPrepares.load(), 1) << "the preparation never started";

  hook.release();
  const auto ready = renderUntilOverlayReady(*compositor, plan, frames);
  EXPECT_FALSE(hasWarning(ready, "graphics-preparing"));
  EXPECT_EQ(ready.health, "live");
  EXPECT_GE(brightIn(ready.preview, name), 30) << "the prepared text never reached Program";
}

TEST(MetalCompositor, TwoOverlaysAndTwoPassesRasterOncePerContent) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  OverlayPrepareHookScope hook(/*block=*/false);
  auto plan = overlayPlan("plan-two-overlays");
  plan.layers.push_back(overlayLayer("a", {0.f, 0.5f, 0.5f, 0.25f}, "FIRST NAME", "FIRST TITLE"));
  plan.layers.push_back(overlayLayer("b", {0.5f, 0.75f, 0.5f, 0.25f}, "SECOND NAME", "SECOND TITLE"));
  const std::vector<VideoFrame> frames{bgraFrame("bg", 64, 36, 0, 0, 0)};
  renderUntilOverlayReady(*compositor, plan, frames);
  for (int tick = 0; tick < 30; ++tick) {
    const auto frame = compositor->render(plan, frames);
    EXPECT_FALSE(hasWarning(frame, "graphics-preparing")) << "tick " << tick;
    compositor->renderPreview(plan, frames);  // same canvas size: same keys
  }
  EXPECT_EQ(gOverlayPrepares.load(), 2) << "one preparation per distinct content, never per pass or frame";
}

TEST(MetalCompositor, LegacyLowerThirdTextLandsInsideItsBand) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  auto plan = overlayPlan("plan-lt-pixels");
  plan.layers.push_back(overlayLayer("lt", {0.f, 0.5f, 1.f, 0.5f}, "ADA OTIENO", "AETHELRED LABS"));
  const std::vector<VideoFrame> frames{bgraFrame("bg", 64, 36, 0, 0, 0)};
  const auto frame = renderUntilOverlayReady(*compositor, plan, frames);
  ASSERT_FALSE(hasWarning(frame, "graphics-preparing"));
  const auto name = nameLine(0, 90, 320, 90);
  EXPECT_GE(brightIn(frame.preview, name), 30);
  // White text exists only on the name line (the title is the red accent
  // colour); one pixel of antialiasing bleed allowed.
  int stray = 0;
  for (int y = 0; y < 180; ++y)
    for (int x = 0; x < 320; ++x) {
      const bool inName = x >= name.x0 - 1 && x <= name.x1 && y >= name.y0 - 1 && y <= name.y1;
      if (!inName && bright(canvasPixel(frame.preview, x, y))) ++stray;
    }
  EXPECT_EQ(stray, 0);
  // The band (brand background #0c1118) fills band pixels away from text.
  const auto band = canvasPixel(frame.preview, 300, 95);
  EXPECT_NEAR(band[0], 0x18, 3);
  EXPECT_NEAR(band[1], 0x11, 3);
  EXPECT_NEAR(band[2], 0x0c, 3);
  // Above the overlay the black source is untouched.
  const auto above = canvasPixel(frame.preview, 160, 40);
  EXPECT_LE(above[0] + above[1] + above[2], 6);
}

// #1073 parity: a Tiles name line (holdLastRaster) keeps drawing its previous
// raster, scaled to the new band, while the resized raster prepares. Never
// the bare accent bar. The un-held overlay beside it is the control: under
// the same resize it shows the fallback and no text.
TEST(MetalCompositor, AHeldNameLineKeepsItsTextWhileTheResizedRasterPrepares) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  OverlayPrepareHookScope hook(/*block=*/false);
  auto plan = overlayPlan("plan-held-name");
  auto held = overlayLayer("held", {0.f, 0.f, 1.f, 0.4f}, "HELD NAME", "");
  held.overlay.holdLastRaster = true;
  plan.layers.push_back(held);
  plan.layers.push_back(overlayLayer("plain", {0.f, 0.5f, 1.f, 0.4f}, "PLAIN NAME", ""));
  const std::vector<VideoFrame> frames{bgraFrame("bg", 64, 36, 0, 0, 0)};
  const auto ready = renderUntilOverlayReady(*compositor, plan, frames);
  ASSERT_FALSE(hasWarning(ready, "graphics-preparing"));
  ASSERT_GE(brightIn(ready.preview, nameLine(0, 0, 320, 72)), 20);
  ASSERT_GE(brightIn(ready.preview, nameLine(0, 90, 320, 72)), 20);

  hook.block();
  plan.layers[1].rect = {0.f, 0.f, 1.f, 0.45f};   // held: 72 -> 81 px tall
  plan.layers[2].rect = {0.f, 0.5f, 1.f, 0.45f};  // plain: same resize
  for (int tick = 0; tick < 5; ++tick) {
    const auto frame = compositor->render(plan, frames);
    ASSERT_TRUE(hasWarning(frame, "graphics-preparing")) << "tick " << tick;
    EXPECT_GE(brightIn(frame.preview, nameLine(0, 0, 320, 81)), 20)
        << "tick " << tick << ": the held name line lost its text while resizing";
    EXPECT_EQ(brightIn(frame.preview, nameLine(0, 90, 320, 81)), 0)
        << "tick " << tick << ": the un-held control cannot have text yet";
    const auto bar = canvasPixel(frame.preview, 3, 120);
    EXPECT_GT(bar[2], 200) << "the un-held control draws the accent-bar fallback";
  }
}

TEST(MetalCompositor, AnAppearanceBandIsDrawnOnceNotTwice) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  auto plan = overlayPlan("plan-appearance-band");
  auto lt = overlayLayer("lt", {0.f, 0.5f, 1.f, 0.5f}, "N", "");
  lt.overlay.appearance.enabled = true;
  lt.overlay.appearance.fontFamily = "Helvetica Neue";
  lt.overlay.appearance.backgroundColor = "#000000";
  lt.overlay.appearance.backgroundOpacity = 0.5;
  lt.overlay.appearance.cornerRadius = 0;
  lt.overlay.appearance.showLogo = false;
  plan.layers.push_back(lt);
  const std::vector<VideoFrame> frames{bgraFrame("bg", 64, 36, 255, 255, 255)};
  const auto frame = renderUntilOverlayReady(*compositor, plan, frames);
  ASSERT_FALSE(hasWarning(frame, "graphics-preparing"));
  // One 50% black band over white: 127.5. A doubled band (quad + raster) is
  // far darker.
  const auto band = canvasPixel(frame.preview, 310, 176);
  for (int c = 0; c < 3; ++c) EXPECT_NEAR(band[c], 128, 3) << "channel " << c;
}

#if COREVIDEO_WITH_MAC_VIRTUALCAM
// ── GPU NV12 tap for the macOS virtual camera (roadmap M9.2, Task 5) ─────────
//
// Every expected value below is a fixture constant hand-computed from the
// integer BT.601 formula (VirtualCameraFrame.h convertBgraToNv12), never from
// the code under test:
//   red  (r=255,g=0,b=0): Y=(66*255+128)/256+16=82  U=(-38*255+128)/256+128=91
//                         V=(112*255+128)/256+128=240
//   blue (r=0,g=0,b=255): Y=(25*255+128)/256+16=41
// The +-1 tolerance is only for the BGRA unorm round trip through the program
// texture; the bit-exact property is pinned by the CPU-oracle test.

constexpr int kVcamW = 1920;
constexpr int kVcamH = 1080;
constexpr size_t kVcamBytes = 3110400;  // 1920*1080*3/2

struct VcamDelivery {
  ICompositor::VcamFrameBuffer nv12;
  int width = 0;
  int height = 0;
  int64_t programSequence = 0;
  int64_t deliveredAt100ns = -1;
};

struct VcamCapture {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<VcamDelivery> deliveries;

  size_t count() {
    std::lock_guard<std::mutex> lock(mutex);
    return deliveries.size();
  }
  VcamDelivery last() {
    std::lock_guard<std::mutex> lock(mutex);
    return deliveries.empty() ? VcamDelivery{} : deliveries.back();
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mutex);
    deliveries.clear();
  }
};

// The capture is shared-owned by the sink so a test that returns early can
// never leave the tap thread calling into a destroyed stack object.
std::shared_ptr<VcamCapture> installVcamCapture(ICompositor& compositor) {
  auto capture = std::make_shared<VcamCapture>();
  compositor.setIdentifiedVcamFrameSink(
      [capture](ICompositor::VcamFrameBuffer nv12, int width, int height, int64_t sequence,
                int64_t deliveredAt100ns) {
        std::lock_guard<std::mutex> lock(capture->mutex);
        capture->deliveries.push_back({std::move(nv12), width, height, sequence, deliveredAt100ns});
        capture->cv.notify_all();
      });
  return capture;
}

// Renders at ~60 Hz until the capture holds at least `wanted` deliveries or
// two seconds pass. Returns the delivery count reached.
size_t renderUntilDelivered(ICompositor& compositor, const CompositorRenderPlan& plan,
                            const std::vector<VideoFrame>& frames, VcamCapture& capture,
                            size_t wanted) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (capture.count() < wanted && std::chrono::steady_clock::now() < deadline) {
    compositor.render(plan, frames);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  return capture.count();
}

int nv12Y(const std::vector<uint8_t>& nv12, int x, int y) {
  return nv12[static_cast<size_t>(y) * kVcamW + x];
}
int nv12U(const std::vector<uint8_t>& nv12, int x, int y) {
  return nv12[static_cast<size_t>(kVcamW) * kVcamH + static_cast<size_t>(y / 2) * kVcamW + (x & ~1)];
}
int nv12V(const std::vector<uint8_t>& nv12, int x, int y) {
  return nv12[static_cast<size_t>(kVcamW) * kVcamH + static_cast<size_t>(y / 2) * kVcamW + (x & ~1) + 1];
}

// b = x % 256, g = y % 256, r = (x + y) % 256: every 2x2 block differs in all
// three channels, so a wrong chroma average, a swapped plane or a float
// division in place of int shows up somewhere in the frame.
VideoFrame gradientFrame(const std::string& participantId, int width, int height) {
  VideoFrame frame = bgraFrame(participantId, width, height, 0, 0, 0);
  auto pixels = std::make_shared<std::vector<uint8_t>>(*frame.pixels);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
      (*pixels)[offset + 0] = static_cast<uint8_t>(x % 256);
      (*pixels)[offset + 1] = static_cast<uint8_t>(y % 256);
      (*pixels)[offset + 2] = static_cast<uint8_t>((x + y) % 256);
      (*pixels)[offset + 3] = 0xff;
    }
  }
  frame.pixels = std::move(pixels);
  return frame;
}

// Left half one colour, right half another (BGR order like bgraFrame).
VideoFrame splitFrame(const std::string& participantId, int width, int height,
                      std::array<uint8_t, 3> leftBgr, std::array<uint8_t, 3> rightBgr) {
  VideoFrame frame = bgraFrame(participantId, width, height, 0, 0, 0);
  auto pixels = std::make_shared<std::vector<uint8_t>>(*frame.pixels);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const auto& c = x < width / 2 ? leftBgr : rightBgr;
      const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
      (*pixels)[offset + 0] = c[0];
      (*pixels)[offset + 1] = c[1];
      (*pixels)[offset + 2] = c[2];
    }
  }
  frame.pixels = std::move(pixels);
  return frame;
}

CompositorRenderPlan vcamPlan(const char* id, int width, int height) {
  CompositorRenderPlan plan;
  plan.renderPlanId = id;
  plan.width = width;
  plan.height = height;
  plan.layers.push_back(solidLayer("cam", 0.f, 0.f, 1.f, 1.f));
  // The SHIPPED Mac plan (ruling C6): the camera arms only the GPU NV12 tap,
  // never the BGRA full-program readback. Only the oracle test turns that on.
  plan.fullProgramReadback = false;
  plan.vcamNv12Tap = true;
  return plan;
}

// A sink the test can hold inside its call: while it is held the tap thread
// cannot copy, so every dispatch made meanwhile keeps its NV12 buffer busy.
struct HoldableVcamCapture : VcamCapture {
  bool holdNext = false;
  bool holding = false;
  bool released = false;
};

std::shared_ptr<HoldableVcamCapture> installHoldableCapture(ICompositor& compositor) {
  auto capture = std::make_shared<HoldableVcamCapture>();
  compositor.setIdentifiedVcamFrameSink(
      [capture](ICompositor::VcamFrameBuffer nv12, int width, int height, int64_t sequence,
                int64_t deliveredAt100ns) {
        std::unique_lock<std::mutex> lock(capture->mutex);
        capture->deliveries.push_back({std::move(nv12), width, height, sequence, deliveredAt100ns});
        if (capture->holdNext) {
          capture->holdNext = false;
          capture->holding = true;
          capture->cv.notify_all();
          capture->cv.wait(lock, [&] { return capture->released; });
          capture->holding = false;
        }
        capture->cv.notify_all();
      });
  return capture;
}

// Holds the tap thread inside the sink, renders `ticksWhileHeld` light ticks
// (each dispatch claims a buffer at encode time, so occupancy is exact, not
// timing-dependent), releases, and drains. Returns the rendered frame numbers
// of the held ticks.
std::vector<int64_t> renderWhileTapHeld(ICompositor& compositor, HoldableVcamCapture& capture,
                                        const CompositorRenderPlan& plan,
                                        const std::vector<VideoFrame>& frames, int ticksWhileHeld) {
  {
    std::lock_guard<std::mutex> lock(capture.mutex);
    capture.holdNext = true;
  }
  {
    std::unique_lock<std::mutex> lock(capture.mutex);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!capture.holding && std::chrono::steady_clock::now() < deadline) {
      lock.unlock();
      compositor.render(plan, frames);
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      lock.lock();
    }
    if (!capture.holding) return {};
  }
  std::vector<int64_t> held;
  for (int i = 0; i < ticksWhileHeld; ++i) {
    held.push_back(compositor.render(plan, frames).frameNumber);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  // Let every held dispatch's GPU work finish before the release, so the
  // drain below sees them all.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  {
    std::lock_guard<std::mutex> lock(capture.mutex);
    capture.released = true;
  }
  capture.cv.notify_all();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(capture.mutex);
      if (!capture.deliveries.empty() && capture.deliveries.back().programSequence >= held.back()) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return held;
}

TEST(MetalCompositor, VcamTapDeliversBt601Nv12OfTheProgram) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  ASSERT_TRUE(compositor->publishesVcamFrames());
  auto capture = installVcamCapture(*compositor);
  const auto plan = vcamPlan("vcam-bt601", kVcamW, kVcamH);
  const std::vector<VideoFrame> frames{bgraFrame("cam", 320, 180, 0, 0, 255)};
  ASSERT_GE(renderUntilDelivered(*compositor, plan, frames, *capture, 2), 1u) << "the vcam sink never fired";
  const auto delivery = capture->last();
  ASSERT_TRUE(delivery.nv12);
  EXPECT_EQ(delivery.width, kVcamW);
  EXPECT_EQ(delivery.height, kVcamH);
  EXPECT_EQ(delivery.deliveredAt100ns, 0);
  EXPECT_GT(delivery.programSequence, 0);
  ASSERT_EQ(delivery.nv12->size(), kVcamBytes);
  EXPECT_NEAR(nv12Y(*delivery.nv12, 960, 540), 82, 1);
  EXPECT_NEAR(nv12U(*delivery.nv12, 960, 540), 91, 1);
  EXPECT_NEAR(nv12V(*delivery.nv12, 960, 540), 240, 1);
  compositor->setIdentifiedVcamFrameSink({});
}

TEST(MetalCompositor, VcamTapMatchesTheCpuOracleBitForBitAt1080p) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  auto capture = installVcamCapture(*compositor);
  auto plan = vcamPlan("vcam-oracle", kVcamW, kVcamH);
  plan.fullProgramReadback = true;  // the test-only oracle input, nothing else
  const std::vector<VideoFrame> frames{gradientFrame("cam", kVcamW, kVcamH)};
  // Read the program back ONCE through the existing full-res BGRA tap (one
  // frame of latency) — the test-only CPU oracle input. The content is static,
  // so every vcam delivery after the first frame is the same picture.
  std::vector<uint8_t> programBgra;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline && (programBgra.empty() || capture->count() < 3)) {
    auto frame = compositor->render(plan, frames);
    if (programBgra.empty() && !frame.programFullBgra.bgra.empty()) {
      ASSERT_EQ(frame.programFullBgra.width, kVcamW);
      ASSERT_EQ(frame.programFullBgra.height, kVcamH);
      programBgra = frame.programFullBgra.bgra;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  ASSERT_EQ(programBgra.size(), static_cast<size_t>(kVcamW) * kVcamH * 4u);
  ASSERT_GE(capture->count(), 3u);
  std::vector<uint8_t> oracle;
  ASSERT_TRUE(convertBgraToNv12(programBgra.data(), kVcamW, kVcamH, oracle));
  const auto gpu = capture->last();
  ASSERT_TRUE(gpu.nv12);
  ASSERT_EQ(gpu.nv12->size(), oracle.size());
  size_t mismatches = 0;
  size_t first = oracle.size();
  for (size_t i = 0; i < oracle.size(); ++i) {
    if ((*gpu.nv12)[i] != oracle[i]) {
      if (first == oracle.size()) first = i;
      ++mismatches;
    }
  }
  EXPECT_EQ(mismatches, 0u) << "first mismatch at byte " << first
                            << (first < oracle.size()
                                    ? " gpu=" + std::to_string((*gpu.nv12)[first]) +
                                          " cpu=" + std::to_string(oracle[first])
                                    : std::string());
  compositor->setIdentifiedVcamFrameSink({});
}

TEST(MetalCompositor, VcamTapMirrorsOnTheGpu) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  auto capture = installVcamCapture(*compositor);
  const auto plan = vcamPlan("vcam-mirror", kVcamW, kVcamH);
  // left red (b=0,g=0,r=255), right blue (b=255,g=0,r=0)
  const std::vector<VideoFrame> frames{splitFrame("cam", kVcamW, kVcamH, {0, 0, 255}, {255, 0, 0})};

  compositor->setVcamMirror(true);
  capture->clear();
  ASSERT_GE(renderUntilDelivered(*compositor, plan, frames, *capture, 4), 4u);
  auto mirrored = capture->last();
  ASSERT_TRUE(mirrored.nv12);
  EXPECT_NEAR(nv12Y(*mirrored.nv12, 100, 540), 41, 1) << "mirror on: the left edge shows the blue right half";
  EXPECT_NEAR(nv12Y(*mirrored.nv12, 1819, 540), 82, 1);

  compositor->setVcamMirror(false);
  capture->clear();
  ASSERT_GE(renderUntilDelivered(*compositor, plan, frames, *capture, 4), 4u);
  auto plain = capture->last();
  ASSERT_TRUE(plain.nv12);
  EXPECT_NEAR(nv12Y(*plain.nv12, 100, 540), 82, 1) << "mirror off: the left edge is red";
  EXPECT_NEAR(nv12Y(*plain.nv12, 1819, 540), 41, 1);
  compositor->setIdentifiedVcamFrameSink({});
}

TEST(MetalCompositor, VcamTapRunsOnLightTicksWithoutCpuReadback) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  auto capture = installVcamCapture(*compositor);
  auto plan = vcamPlan("vcam-light", kVcamW, kVcamH);
  plan.skipCpuReadback = true;
  const std::vector<VideoFrame> frames{bgraFrame("cam", 320, 180, 0, 0, 255)};
  bool previewEverRead = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (capture->count() < 1 && std::chrono::steady_clock::now() < deadline) {
    const auto frame = compositor->render(plan, frames);
    previewEverRead = previewEverRead || !frame.preview.bgra.empty() || !frame.programFullBgra.bgra.empty();
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  EXPECT_GE(capture->count(), 1u) << "the vcam tap must run on the light display tick";
  EXPECT_FALSE(previewEverRead) << "a light tick must not do a CPU readback";
  compositor->setIdentifiedVcamFrameSink({});
}

TEST(MetalCompositor, VcamTapScalesANon1080pCanvasTo1080p) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  auto capture = installVcamCapture(*compositor);
  const auto plan = vcamPlan("vcam-720p", 1280, 720);
  const std::vector<VideoFrame> frames{bgraFrame("cam", 320, 180, 0, 0, 255)};
  ASSERT_GE(renderUntilDelivered(*compositor, plan, frames, *capture, 2), 1u);
  const auto delivery = capture->last();
  ASSERT_TRUE(delivery.nv12);
  EXPECT_EQ(delivery.width, kVcamW);
  EXPECT_EQ(delivery.height, kVcamH);
  ASSERT_EQ(delivery.nv12->size(), kVcamBytes);
  EXPECT_NEAR(nv12Y(*delivery.nv12, 960, 540), 82, 1);
  EXPECT_NEAR(nv12Y(*delivery.nv12, 0, 0), 82, 1);
  EXPECT_NEAR(nv12Y(*delivery.nv12, 1919, 1079), 82, 1);
  compositor->setIdentifiedVcamFrameSink({});
}

TEST(MetalCompositor, ClearingTheVcamSinkStopsDelivery) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  auto capture = installVcamCapture(*compositor);
  const auto plan = vcamPlan("vcam-clear", kVcamW, kVcamH);
  const std::vector<VideoFrame> frames{bgraFrame("cam", 320, 180, 0, 0, 255)};
  ASSERT_GE(renderUntilDelivered(*compositor, plan, frames, *capture, 1), 1u);
  compositor->setIdentifiedVcamFrameSink({});  // waits for any in-flight call
  const size_t settled = capture->count();
  for (int i = 0; i < 10; ++i) {
    compositor->render(plan, frames);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(capture->count(), settled);
}

// Paced at 60 Hz like the product's render thread (controller ruling C7), so
// the two ping-pong buffers are exercised at the real cadence, not flooded.
TEST(MetalCompositor, VcamTapDeliversEveryRenderedFrame) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  auto capture = installVcamCapture(*compositor);
  const auto plan = vcamPlan("vcam-every", kVcamW, kVcamH);
  const std::vector<VideoFrame> frames{gradientFrame("cam", kVcamW, kVcamH)};
  // Warm up: the tap prepares its pipeline off the render thread on the first
  // tap tick, so the stream starts once that is done.
  ASSERT_GE(renderUntilDelivered(*compositor, plan, frames, *capture, 1), 1u);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  capture->clear();
  constexpr int kTicks = 120;
  std::vector<int64_t> rendered;
  const auto period = std::chrono::microseconds(16667);
  auto next = std::chrono::steady_clock::now();
  for (int i = 0; i < kTicks; ++i) {
    rendered.push_back(compositor->render(plan, frames).frameNumber);
    next += period;
    std::this_thread::sleep_until(next);
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (capture->count() < static_cast<size_t>(kTicks) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  compositor->setIdentifiedVcamFrameSink({});
  std::vector<int64_t> delivered;
  {
    std::lock_guard<std::mutex> lock(capture->mutex);
    for (const auto& d : capture->deliveries) delivered.push_back(d.programSequence);
  }
  EXPECT_EQ(delivered.size(), static_cast<size_t>(kTicks));
  EXPECT_EQ(delivered, rendered) << "every rendered frame is delivered once, in order";
  for (size_t i = 1; i < delivered.size(); ++i) {
    EXPECT_EQ(delivered[i], delivered[i - 1] + 1) << "sequence gap at delivery " << i;
  }
  EXPECT_EQ(compositor->vcamDispatchSkipped(), 0u);
}
// Fix round 1 (review Important #1): the program pass keeps up to 3 command
// buffers in flight without blocking, so under GPU backlog 3 tap dispatches can
// be pending while a 4th frame is being copied. The ring must absorb that.
TEST(MetalCompositor, VcamTapAbsorbsTheProgramInFlightDepthWhileTheTapIsBusy) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  auto capture = installHoldableCapture(*compositor);
  auto plan = vcamPlan("vcam-inflight", kVcamW, kVcamH);
  plan.skipCpuReadback = true;
  const std::vector<VideoFrame> frames{bgraFrame("cam", 320, 180, 0, 0, 255)};
  ASSERT_GE(renderUntilDelivered(*compositor, plan, frames, *capture, 1), 1u);
  const auto held = renderWhileTapHeld(*compositor, *capture, plan, frames, 4);
  ASSERT_EQ(held.size(), 4u) << "the tap thread was never held";
  compositor->setIdentifiedVcamFrameSink({});
  EXPECT_EQ(compositor->vcamDispatchSkipped(), 0u)
      << "4 dispatches pending behind a busy tap must not skip (3 in flight + 1 copy)";
  std::set<int64_t> delivered;
  {
    std::lock_guard<std::mutex> lock(capture->mutex);
    for (const auto& d : capture->deliveries) delivered.insert(d.programSequence);
  }
  for (const auto sequence : held) EXPECT_TRUE(delivered.count(sequence)) << "frame " << sequence << " lost";
}

// ...and once every buffer IS busy, the lost frame is counted, not hidden.
TEST(MetalCompositor, VcamTapCountsASkipWhenEveryBufferIsBusy) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  auto capture = installHoldableCapture(*compositor);
  auto plan = vcamPlan("vcam-skip", kVcamW, kVcamH);
  plan.skipCpuReadback = true;
  const std::vector<VideoFrame> frames{bgraFrame("cam", 320, 180, 0, 0, 255)};
  ASSERT_GE(renderUntilDelivered(*compositor, plan, frames, *capture, 1), 1u);
  const auto held = renderWhileTapHeld(*compositor, *capture, plan, frames, 5);
  ASSERT_EQ(held.size(), 5u) << "the tap thread was never held";
  compositor->setIdentifiedVcamFrameSink({});
  EXPECT_EQ(compositor->vcamDispatchSkipped(), 1u);
  std::set<int64_t> delivered;
  {
    std::lock_guard<std::mutex> lock(capture->mutex);
    for (const auto& d : capture->deliveries) delivered.insert(d.programSequence);
  }
  for (size_t i = 0; i < 4; ++i) EXPECT_TRUE(delivered.count(held[i])) << "frame " << held[i] << " lost";
  EXPECT_FALSE(delivered.count(held[4])) << "the skipped frame cannot have been delivered";
}

// Fix round 1 (review Minor, coordinator-mandated): the kernel compile and the
// 4 x 3 MB buffer allocation never run on the render thread. Every tick from
// the camera's enable to its first delivery stays near the ordinary
// light-tick cost.
TEST(MetalCompositor, VcamTapPreparationNeverStallsTheRenderTick) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  auto plan = vcamPlan("vcam-prep", kVcamW, kVcamH);
  plan.skipCpuReadback = true;
  const std::vector<VideoFrame> frames{bgraFrame("cam", 320, 180, 0, 0, 255)};
  auto timedRender = [&](const CompositorRenderPlan& p) {
    const auto start = std::chrono::steady_clock::now();
    compositor->render(p, frames);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  };
  auto idle = plan;
  idle.vcamNv12Tap = false;
  double baselineWorst = 0;
  for (int i = 0; i < 30; ++i) {
    baselineWorst = std::max(baselineWorst, timedRender(idle));
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  auto capture = installVcamCapture(*compositor);
  double enableWorst = 0;
  int ticks = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (capture->count() < 1 && std::chrono::steady_clock::now() < deadline) {
    enableWorst = std::max(enableWorst, timedRender(plan));
    ++ticks;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  compositor->setIdentifiedVcamFrameSink({});
  std::fprintf(stderr, "[metal-test] vcam enable: worst tick %.3f ms over %d ticks (idle worst %.3f ms)\n",
               enableWorst, ticks, baselineWorst);
  ASSERT_GE(capture->count(), 1u);
  EXPECT_LT(enableWorst, baselineWorst + 4.0)
      << "a tick between enable and first delivery stalled the render thread";
}

// Carried fix (Task 7): a failed tap preparation (kernel compile or buffer
// allocation) used to be one log line and nothing else: the camera stayed
// "live" with no frames, forever. It must surface as the tap warning MediaCore
// folds into the virtualCamera snapshot.
TEST(MetalCompositor, AFailedVcamTapPreparationIsReportedAsAWarning) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  EXPECT_EQ(compositor->vcamTapWarning(), "") << "no warning before the tap is ever asked for";
  corevideo::compositor::setVcamTapPrepFailureForTest(true);
  auto capture = installVcamCapture(*compositor);
  auto plan = vcamPlan("vcam-prep-fail", kVcamW, kVcamH);
  plan.skipCpuReadback = true;
  const std::vector<VideoFrame> frames{bgraFrame("cam", 320, 180, 0, 0, 255)};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (compositor->vcamTapWarning().empty() && std::chrono::steady_clock::now() < deadline) {
    compositor->render(plan, frames);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  corevideo::compositor::setVcamTapPrepFailureForTest(false);
  const std::string warning = compositor->vcamTapWarning();
  compositor->setIdentifiedVcamFrameSink({});
  EXPECT_EQ(warning.rfind("CAMERA_TAP_PREP_FAILED:", 0), 0u) << warning;
  EXPECT_EQ(capture->count(), 0u) << "a tap that never prepared delivers nothing";
}

// ...and a healthy preparation never reports one.
TEST(MetalCompositor, AHealthyVcamTapReportsNoWarning) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  auto capture = installVcamCapture(*compositor);
  auto plan = vcamPlan("vcam-prep-ok", kVcamW, kVcamH);
  plan.skipCpuReadback = true;
  const std::vector<VideoFrame> frames{bgraFrame("cam", 320, 180, 0, 0, 255)};
  ASSERT_GE(renderUntilDelivered(*compositor, plan, frames, *capture, 1), 1u);
  compositor->setIdentifiedVcamFrameSink({});
  EXPECT_EQ(compositor->vcamTapWarning(), "");
}
#endif  // COREVIDEO_WITH_MAC_VIRTUALCAM

}  // namespace

#endif  // COREVIDEO_WITH_METAL
