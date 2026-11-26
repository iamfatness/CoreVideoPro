// Metal pixel proofs for the CoreVideo Tiles decoration (mac parity Phase 3,
// slice N1). The D3D twin (TilesDecorationTest.cpp) had its thresholds
// confirmed on Windows CI against real GPU output, so its numbers are the
// Windows-derived fixture: they are copied verbatim and never loosened here.
// A missing Metal device is an explicit skip, like MetalCompositorTest.cpp.

#if COREVIDEO_WITH_METAL

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "compositor/MetalCompositorLibrary.h"
#include "compositor/MetalCompositorShaders.h"
#include "modules/Interfaces.h"

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

// Copied from MetalCompositorTest.cpp:39-71 (the vendored gtest has no GTEST_SKIP).
#define MAKE_COMPOSITOR_OR_SKIP(var)                                          \
  std::string skipReason_;                                                    \
  auto var = makeCompositorOrSkipReason(skipReason_);                         \
  if (!var) {                                                                 \
    std::fprintf(stderr, "[metal-test] unavailable: %s\n", skipReason_.c_str()); \
    ASSERT_TRUE(skipReason_.find("no Metal device") != std::string::npos); \
    return;                                                                   \
  }

CompositorRenderPlan decorationPlan() {
  CompositorRenderPlan plan;
  plan.width = 320; plan.height = 180; plan.fps = 60;
  plan.sceneId = "decoration"; plan.renderPlanId = "decoration";
  return plan;
}

CompositorRenderPlanLayer blackBackground() {
  CompositorRenderPlanLayer background;
  background.layerId = "background"; background.kind = "tiles-background";
  background.rect = {0, 0, 1, 1}; background.hasFillColor = true; background.fillColor = "#000000";
  return background;
}

VideoFrame solidBgra(const std::string& participantId, uint8_t b, uint8_t g, uint8_t r, uint64_t frameId) {
  VideoFrame source;
  source.participantId = participantId; source.width = source.pixelWidth = 64;
  source.height = source.pixelHeight = 64; source.pixelStride = 256; source.frameId = frameId;
  auto pixels = std::make_shared<std::vector<uint8_t>>(64 * 64 * 4, 255);
  for (size_t at = 0; at < pixels->size(); at += 4) {
    (*pixels)[at] = b; (*pixels)[at + 1] = g; (*pixels)[at + 2] = r;
  }
  source.pixels = pixels;
  return source;
}

}  // namespace

// Port of TilesDecoration.D3DPixelsHaveRoundedMaskInsetBorderAndAnalyticGlow
// (TilesDecorationTest.cpp:35-120): same plan, same four renders, every
// assertion copied exactly.
TEST(MetalTilesPixels, RoundedMaskInsetBorderAndAnalyticGlowMatchTheD3DProof) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan = decorationPlan();
  CompositorRenderPlanLayer background = blackBackground();
  CompositorRenderPlanLayer tile;
  tile.layerId = "tile:test"; tile.sourceId = "zoom:test"; tile.participantId = "test";
  tile.kind = "participant-video"; tile.rect = {0.25f, 0.2f, 0.5f, 0.6f}; tile.order = 2;
  auto& style = tile.tilesDecoration;
  style.enabled = true; style.radius = 24; style.borderWidth = 8; style.borderColor = "#FF0000";
  style.glowSize = 16; style.glowIntensity = 1; style.glowColor = "#00FF00";
  auto glow = tile; glow.kind = "tiles-glow"; glow.layerId = "glow"; glow.participantId.clear();
  glow.sourceId.clear(); glow.order = 1; glow.tilesDecoration.glowPass = true;
  VideoFrame source;
  source.participantId = "test"; source.width = source.pixelWidth = 64;
  source.height = source.pixelHeight = 64; source.pixelStride = 256; source.frameId = 1;
  auto pixels = std::make_shared<std::vector<uint8_t>>(64 * 64 * 4, 255);
  source.pixels = pixels;
  plan.layers = {background, glow, tile};
  const auto rendered = compositor->render(plan, {source});
  ASSERT_TRUE(rendered.gpuComposed);
  const auto& image = rendered.preview;
  ASSERT_EQ(image.width, 320); ASSERT_EQ(image.height, 180);
  const auto pixel = [&](int x, int y, int channel) { return image.bgra[(y * 320 + x) * 4 + channel]; };
  // Interior retains white source pixels; border replaces RGB, not geometry.
  EXPECT_TRUE(pixel(160, 90, 0) > 245 && pixel(160, 90, 1) > 245);
  EXPECT_TRUE(pixel(83, 90, 2) > 245 && pixel(83, 90, 1) < 10);
  EXPECT_TRUE(pixel(99, 55, 0) > 245 && pixel(99, 55, 1) > 245);
  // Diagonal corner is cut away; its underlying green halo is not red/white video.
  EXPECT_TRUE(pixel(81, 37, 2) < 10 && pixel(81, 37, 0) < 10);
  // 8 pixels beyond edge: t=7.5/16, alpha=(1-t)^2, approximately 72/255.
  EXPECT_TRUE(pixel(72, 90, 1) >= 65 && pixel(72, 90, 1) <= 80);
  EXPECT_TRUE(pixel(56, 90, 1) < 5);
  // Zoom's limited-range I420 path must receive the identical decoration mask.
  source.pixels.reset(); source.i420Width = 64; source.i420Height = 64;
  source.i420FullRange = false; source.frameId = 2;
  auto i420 = std::make_shared<std::vector<uint8_t>>(64 * 64 * 3 / 2, 128);
  std::fill_n(i420->begin(), 64 * 64, 235);
  source.i420 = i420;
  const auto yuv = compositor->render(plan, {source});
  ASSERT_EQ(yuv.preview.width, 320);
  const auto yp = [&](int x, int y, int channel) { return yuv.preview.bgra[(y * 320 + x) * 4 + channel]; };
  EXPECT_TRUE(yp(160, 90, 0) > 245 && yp(160, 90, 1) > 245);
  EXPECT_TRUE(yp(83, 90, 2) > 245 && yp(83, 90, 1) < 10);
  EXPECT_TRUE(yp(81, 37, 2) < 10);
  EXPECT_TRUE(yp(72, 90, 1) >= 65 && yp(72, 90, 1) <= 80);
  plan.layers = {background, tile};
  plan.layers[1].tilesDecoration = {};
  const auto plain = compositor->render(plan, {source});
  ASSERT_EQ(plain.preview.width, 320);
  EXPECT_TRUE(plain.preview.bgra[(37 * 320 + 81) * 4 + 2] > 245);
  EXPECT_TRUE(plain.preview.bgra[(90 * 320 + 72) * 4 + 1] < 5);
  source.i420.reset(); source.frameId = 3;
  auto bars = std::make_shared<std::vector<uint8_t>>(64 * 64 * 4, 0);
  for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
    const size_t at = (y * 64 + x) * 4;
    (*bars)[at + (x < 16 ? 2 : x >= 48 ? 0 : 1)] = 255;
    (*bars)[at + 3] = 255;
  }
  source.pixels = bars;
  plan.layers[1].sourceCropLeftPercent = 25;
  plan.layers[1].sourceCropRightPercent = 25;
  const auto cropped = compositor->render(plan, {source});
  ASSERT_EQ(cropped.preview.width, 320);
  for (const int x : {84, 160, 235}) {
    const size_t at = (90 * 320 + x) * 4;
    EXPECT_TRUE(cropped.preview.bgra[at + 1] > 245) << "x=" << x;
    EXPECT_TRUE(cropped.preview.bgra[at] < 10 && cropped.preview.bgra[at + 2] < 10) << "x=" << x;
  }
}


// Twin of TilesDecoration.EffectCompilationFailureRetainsCleanShader: only the
// effect code uses fwidth, so poisoning it must still yield a real library
// compiled with COREVIDEO_DISABLE_TILES_EFFECT, and report effects unavailable.
TEST(MetalTilesPixels, ABrokenEffectStillCompilesACleanLibrary) {
  const std::string broken =
      std::string("#define fwidth unavailable_effect_function\n") + kMetalCompositorShaderSource;
  bool effectsAvailable = true;
  const bool compiled = metalCompositorLibraryFallbackForTest(broken.c_str(), effectsAvailable);
  if (!compiled && effectsAvailable) {
    std::fprintf(stderr, "[metal-test] unavailable: no Metal device\n");
    return;
  }
  EXPECT_TRUE(compiled);
  EXPECT_FALSE(effectsAvailable);
  // The healthy source keeps effects on.
  bool healthy = false;
  EXPECT_TRUE(metalCompositorLibraryFallbackForTest(kMetalCompositorShaderSource, healthy));
  EXPECT_TRUE(healthy);
}

// The core sets each tile layer's opacity from the wall's; Metal must blend the
// tile toward the background by exactly that much.
TEST(MetalTilesPixels, WallOpacityBlendsTilesTowardTheBackground) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan = decorationPlan();
  CompositorRenderPlanLayer tile;
  tile.layerId = "tile:test"; tile.sourceId = "zoom:test"; tile.participantId = "test";
  tile.kind = "participant-video"; tile.rect = {0.25f, 0.2f, 0.5f, 0.6f}; tile.order = 2;
  tile.opacity = 0.5f;
  plan.layers = {blackBackground(), tile};
  const auto rendered = compositor->render(plan, {solidBgra("test", 255, 255, 255, 1)});
  ASSERT_EQ(rendered.preview.width, 320);
  const int g = rendered.preview.bgra[(90 * 320 + 160) * 4 + 1];
  EXPECT_TRUE(g >= 120 && g <= 136) << "centre G=" << g;
}

// A tile (and its glow) clipped to the wall rect draws nothing outside it.
TEST(MetalTilesPixels, ATileClippedToTheWallRectDrawsNothingOutsideIt) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan = decorationPlan();
  CompositorRenderPlanLayer tile;
  tile.layerId = "tile:test"; tile.sourceId = "zoom:test"; tile.participantId = "test";
  tile.kind = "participant-video"; tile.rect = {0.1f, 0.1f, 0.8f, 0.8f}; tile.order = 2;
  // Asymmetric clip (N1 review): a symmetric one cannot see a y-flip or an
  // x/y swap of the scissor. Clip spans x 64..224, y 18..90 of 320x180.
  tile.hasClipRect = true; tile.clipRect = {0.2f, 0.1f, 0.5f, 0.4f};
  tile.tilesDecoration.enabled = true; tile.tilesDecoration.glowSize = 16;
  tile.tilesDecoration.glowIntensity = 1; tile.tilesDecoration.glowColor = "#00FF00";
  auto glow = tile; glow.kind = "tiles-glow"; glow.layerId = "glow"; glow.participantId.clear();
  glow.sourceId.clear(); glow.order = 1; glow.tilesDecoration.glowPass = true;
  plan.layers = {blackBackground(), glow, tile};
  const auto rendered = compositor->render(plan, {solidBgra("test", 255, 255, 255, 1)});
  ASSERT_EQ(rendered.preview.width, 320);
  const auto& img = rendered.preview.bgra;
  const auto at = [&](int x, int y, int c) { return img[(static_cast<size_t>(y) * 320 + x) * 4 + c]; };
  // Inside the clip the white tile shows (proves the tile drew at all).
  EXPECT_GT(at(144, 54, 1), 245);
  // Left, right, above (the glow band over the tile's top edge) and below the
  // clip: background only, glow included.
  const int outside[][2] = {{48, 54}, {256, 54}, {144, 9}, {144, 108}};
  for (const auto& p : outside) {
    for (int c = 0; c < 3; ++c)
      EXPECT_LE(at(p[0], p[1], c), 2) << "x=" << p[0] << " y=" << p[1] << " c=" << c;
  }
}

// The wall's live background feed draws full-canvas under the tiles.
TEST(MetalTilesPixels, ALiveBackgroundFrameDrawsUnderTheTiles) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan = decorationPlan();
  CompositorRenderPlanLayer bg;
  bg.layerId = "tiles-source-bg:w"; bg.sourceId = "zoom:bg"; bg.participantId = "bg";
  bg.kind = "participant-video"; bg.rect = {0, 0, 1, 1}; bg.order = 0; bg.fitMode = "fill";
  CompositorRenderPlanLayer tile;
  tile.layerId = "tile:test"; tile.sourceId = "zoom:test"; tile.participantId = "test";
  tile.kind = "participant-video"; tile.rect = {0.25f, 0.25f, 0.5f, 0.5f}; tile.order = 2;
  plan.layers = {bg, tile};
  const auto rendered = compositor->render(
      plan, {solidBgra("bg", 255, 0, 0, 1), solidBgra("test", 255, 255, 255, 1)});
  ASSERT_EQ(rendered.preview.width, 320);
  const auto& img = rendered.preview.bgra;
  const size_t corner = (5 * 320 + 5) * 4;
  EXPECT_GT(img[corner], 240);
  EXPECT_LT(img[corner + 1], 15);
  EXPECT_LT(img[corner + 2], 15);
  const size_t centre = (90 * 320 + 160) * 4;
  EXPECT_GT(img[centre], 245); EXPECT_GT(img[centre + 1], 245); EXPECT_GT(img[centre + 2], 245);
}

#endif  // COREVIDEO_WITH_METAL
