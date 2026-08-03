// Metal compositor adapter tests (M1). Compiled only under
// COREVIDEO_WITH_METAL; on a machine without a usable Metal device every test
// SKIPS instead of failing — a GPU-less CI runner must never brick the suite
// (deliberate divergence from the D3D11 tests' hard ASSERT_NE).

#if COREVIDEO_WITH_METAL

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "compositor/CompositorLayout.h"
#include "compositor/CompositorShaderParams.h"
#include "modules/Interfaces.h"
#include "modules/ProgramFramePreview.h"

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
    std::fprintf(stderr, "[metal-test] skipping: %s\n", skipReason_.c_str()); \
    return;                                                                   \
  }

std::array<int, 4> previewAt(const ProgramFramePreviewPixels& preview, float fx, float fy) {
  const int x = std::min(preview.width - 1, std::max(0, static_cast<int>(fx * preview.width)));
  const int y = std::min(preview.height - 1, std::max(0, static_cast<int>(fy * preview.height)));
  const size_t offset = (static_cast<size_t>(y) * preview.width + x) * 4u;
  return {preview.bgra[offset + 0], preview.bgra[offset + 1], preview.bgra[offset + 2],
          preview.bgra[offset + 3]};
}

CompositorRenderPlanLayer solidLayer(const std::string& id, float x, float y, float w, float h) {
  CompositorRenderPlanLayer layer;
  layer.layerId = "layer:" + id;
  layer.kind = "participant-video";
  layer.participantId = id;
  layer.sourceId = "zoom:" + id;
  layer.rect = {x, y, w, h};
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
  plan.layers.push_back(solidLayer("alpha", 0.0f, 0.0f, 0.5f, 1.0f));
  plan.layers.push_back(solidLayer("beta", 0.5f, 0.0f, 0.5f, 1.0f));

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

  const auto gpu = compositor->render(plan, {});
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

}  // namespace

#endif  // COREVIDEO_WITH_METAL
