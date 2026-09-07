#include "compositor/TilesDecorationParams.h"
#include "core/MediaCore.h"
#include "compositor/CompositorShaders.h"
#include <gtest/gtest.h>
using namespace corevideo::modules;

TEST(TilesDecoration, ExtremeSettingsClampToTheTileInterior) {
  CompositorRenderPlanLayer layer;
  layer.rect = {0.25f, 0.25f, 0.5f, 0.5f};
  layer.tilesDecoration.enabled = true;
  layer.tilesDecoration.radius = 1000.f;
  layer.tilesDecoration.borderWidth = 1000.f;
  LayerShaderConstants c{};
  applyTilesDecoration(c, layer, 200, 100);
  EXPECT_EQ(c.tileShape[1], 25.f);
  EXPECT_EQ(c.tileShape[2], 25.f);
  layer.tilesDecoration.enabled = false;
  applyTilesDecoration(c, layer, 200, 100);
  EXPECT_EQ(c.tileShape[0], 0.f);
}

TEST(TilesDecoration, ExplicitSideCropPreservesAspectAndMapsBackToOriginalPixels) {
  const auto framing = corevideo::compositor::computeSourceFraming(
      1920, 1080, {0, 0, 100, 100}, "fill", 1, 0, 0, 25, 25);
  EXPECT_NEAR(framing.u0, 0.25, 1e-6);
  EXPECT_NEAR(framing.u1, 0.75, 1e-6);
  EXPECT_TRUE(framing.v0 > 0.f && framing.v1 < 1.f);
  EXPECT_FALSE(framing.hasLetterbox);
  const auto defensive = corevideo::compositor::sourceCropInterval(80, 40);
  EXPECT_NEAR(defensive.left, 0.6, 1e-6);
  EXPECT_NEAR(defensive.width, 0.1, 1e-6);
}

#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
TEST(TilesDecoration, D3DPixelsHaveRoundedMaskInsetBorderAndAnalyticGlow) {
  auto compositor = createD3D11Compositor();
  ASSERT_TRUE(compositor != nullptr);
  CompositorRenderPlan plan;
  plan.width = 320; plan.height = 180; plan.fps = 60;
  plan.sceneId = "decoration"; plan.renderPlanId = "decoration";

  CompositorRenderPlanLayer background;
  background.layerId = "background"; background.kind = "tiles-background";
  background.rect = {0, 0, 1, 1}; background.hasFillColor = true; background.fillColor = "#000000";
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
  // 8 pixels beyond edge: t=7.5/16, alpha=(1-t)^2, approximately72/255.
  EXPECT_TRUE(pixel(72, 90, 1) >= 65 && pixel(72, 90, 1) <= 80);
  EXPECT_TRUE(pixel(56, 90, 1) < 5);
  // Zoom's limited-range I420 path must receive the identical decoration mask,
  // while preserving the existing source-specific YUV range conversion.
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
    EXPECT_TRUE(cropped.preview.bgra[at + 1] > 245);
    EXPECT_TRUE(cropped.preview.bgra[at] < 10 && cropped.preview.bgra[at + 2] < 10);
  }
}
TEST(TilesDecoration, EffectCompilationFailureRetainsCleanShader) {
  bool available = true;
  std::string error;
  // Only effect code uses fwidth; disabling that code must recover a real,
  // compilable ordinary source shader without dropping the source itself.
  const auto brokenEffect = std::string("#define fwidth unavailable_effect_function\n") + kCompositorTexturedPixelShader;
  auto shader = compileTilesShader(brokenEffect.c_str(), available, error);
  ASSERT_TRUE(shader.get() != nullptr);
  EXPECT_FALSE(available);
}
#endif
