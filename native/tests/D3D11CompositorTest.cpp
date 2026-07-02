#include "compositor/CompositorLayout.h"
#include "modules/Interfaces.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

uint32_t previewPixelRgba(const corevideo::modules::ProgramFramePreviewPixels& preview, int x, int y) {
  if (preview.width <= 0 || preview.height <= 0 || preview.bgra.empty()) {
    return 0;
  }
  const int clampedX = std::max(0, std::min(x, preview.width - 1));
  const int clampedY = std::max(0, std::min(y, preview.height - 1));
  const size_t offset = static_cast<size_t>((clampedY * preview.width + clampedX) * 4);
  return (static_cast<uint32_t>(preview.bgra[offset + 3]) << 24) |
         (static_cast<uint32_t>(preview.bgra[offset + 2]) << 16) |
         (static_cast<uint32_t>(preview.bgra[offset + 1]) << 8) |
         static_cast<uint32_t>(preview.bgra[offset + 0]);
}

uint32_t blendRgbaOver(uint32_t source, uint32_t destination, float opacity) {
  const float alpha = static_cast<float>((source >> 24) & 0xff) / 255.f * opacity;
  const float inverseAlpha = 1.f - alpha;
  const auto mix = [alpha, inverseAlpha](uint32_t sourceChannel, uint32_t destinationChannel) {
    return static_cast<uint32_t>(std::lround(static_cast<float>(sourceChannel) * alpha + static_cast<float>(destinationChannel) * inverseAlpha));
  };
  const uint32_t r = mix((source >> 16) & 0xff, (destination >> 16) & 0xff);
  const uint32_t g = mix((source >> 8) & 0xff, (destination >> 8) & 0xff);
  const uint32_t b = mix(source & 0xff, destination & 0xff);
  return 0xff000000u | (r << 16) | (g << 8) | b;
}

corevideo::modules::VideoFrame makeMetadataFrame(const std::string& participantId, int width = 1280, int height = 720) {
  corevideo::modules::VideoFrame frame;
  frame.participantId = participantId;
  frame.width = width;
  frame.height = height;
  frame.naturalWidth = width;
  frame.naturalHeight = height;
  frame.timestampMs = 16;
  return frame;
}

corevideo::modules::VideoFrame makeLeftRightBandFrame(
    const std::string& participantId,
    int pixelWidth,
    int pixelHeight,
    int naturalWidth,
    int naturalHeight,
    uint32_t leftRgba,
    uint32_t rightRgba) {
  corevideo::modules::VideoFrame frame;
  frame.participantId = participantId;
  frame.width = naturalWidth;
  frame.height = naturalHeight;
  frame.naturalWidth = naturalWidth;
  frame.naturalHeight = naturalHeight;
  frame.pixelWidth = pixelWidth;
  frame.pixelHeight = pixelHeight;
  frame.pixelStride = pixelWidth * 4;
  frame.timestampMs = 16;
  auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(pixelWidth) * pixelHeight * 4);
  for (int y = 0; y < pixelHeight; ++y) {
    for (int x = 0; x < pixelWidth; ++x) {
      const uint32_t rgba = x < pixelWidth / 2 ? leftRgba : rightRgba;
      const size_t offset = static_cast<size_t>((y * pixelWidth + x) * 4);
      (*pixels)[offset + 0] = static_cast<uint8_t>(rgba & 0xff);
      (*pixels)[offset + 1] = static_cast<uint8_t>((rgba >> 8) & 0xff);
      (*pixels)[offset + 2] = static_cast<uint8_t>((rgba >> 16) & 0xff);
      (*pixels)[offset + 3] = static_cast<uint8_t>((rgba >> 24) & 0xff);
    }
  }
  frame.pixels = std::move(pixels);
  return frame;
}

corevideo::modules::CompositorRenderPlan overlappingSceneGraphPlan() {
  corevideo::modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = "b3-stub-scene";
  renderPlan.sceneId = "scene-graph-rendering";
  renderPlan.width = 640;
  renderPlan.height = 360;
  renderPlan.layers.push_back({
      "front",
      "participant-video",
      "zoom:front",
      "front",
      20,
      {0.f, 0.f, 1.f, 1.f},
      1.f,
  });
  renderPlan.layers.push_back({
      "back",
      "participant-video",
      "zoom:back",
      "back",
      10,
      {0.f, 0.f, 1.f, 1.f},
      1.f,
  });
  return renderPlan;
}

}  // namespace

TEST(CompositorLayout, ComputesStableGridCells) {
  const auto topLeft = corevideo::compositor::gridCell(4, 0);
  const auto bottomRight = corevideo::compositor::gridCell(4, 3);
  EXPECT_TRUE(topLeft.x < bottomRight.x);
  EXPECT_TRUE(topLeft.y < bottomRight.y);
  EXPECT_TRUE(topLeft.width > 0.f);
  EXPECT_TRUE(bottomRight.height > 0.f);
}

TEST(CompositorLayout, ProducesDistinctParticipantColors) {
  const auto left = corevideo::compositor::colorFromParticipantId("participant-a");
  const auto right = corevideo::compositor::colorFromParticipantId("participant-b");
  EXPECT_NE(left, right);
}

#if COREVIDEO_STUB
TEST(StubCompositor, SceneGraphRenderingStaysGreenInCorevideoStub) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);
  EXPECT_EQ(modules.compositor->rendererName(), "software");

  const auto renderPlan = overlappingSceneGraphPlan();
  const auto frame = modules.compositor->render(renderPlan, {makeMetadataFrame("front"), makeMetadataFrame("back")});

  EXPECT_EQ(frame.renderer, "software");
  EXPECT_EQ(frame.renderPlanId, "b3-stub-scene");
  EXPECT_EQ(frame.layerCount, 2);
  EXPECT_FALSE(frame.gpuComposed);
  EXPECT_EQ(frame.health, "live");
  EXPECT_NE(frame.programPixelSignature, 0u);
  EXPECT_EQ(frame.preview.width, 320);
  EXPECT_EQ(frame.preview.height, 180);
  EXPECT_EQ(frame.preview.bgra.size(), static_cast<size_t>(frame.preview.width * frame.preview.height * 4));
  EXPECT_EQ(previewPixelRgba(frame.preview, frame.preview.width / 2, frame.preview.height / 2), corevideo::compositor::colorFromParticipantId("front"));
  EXPECT_EQ(frame.sharedTexture.sharedHandleHex, "0xFEEDFACE");
  EXPECT_EQ(corevideo::modules::createD3D11Compositor(), nullptr);
}

TEST(StubCompositor, ProducesDeterministicPreviewAndSignatureForLayerOrder) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);

  auto firstPlan = overlappingSceneGraphPlan();
  auto secondPlan = firstPlan;
  std::reverse(secondPlan.layers.begin(), secondPlan.layers.end());

  const std::vector<corevideo::modules::VideoFrame> frames = {makeMetadataFrame("front"), makeMetadataFrame("back")};
  const auto first = modules.compositor->render(firstPlan, frames);
  const auto second = modules.compositor->render(secondPlan, frames);

  EXPECT_EQ(first.programPixelSignature, second.programPixelSignature);
  EXPECT_EQ(first.preview.width, second.preview.width);
  EXPECT_EQ(first.preview.height, second.preview.height);
  EXPECT_EQ(first.preview.bgra, second.preview.bgra);
}

TEST(StubCompositor, SourcePanUsesNaturalSourceDimensionsBeforePreviewPixels) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);

  corevideo::modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = "natural-framing";
  renderPlan.width = 640;
  renderPlan.height = 640;
  renderPlan.layers.push_back({
      "camera",
      "participant-video",
      "zoom:camera",
      "camera",
      0,
      {0.f, 0.f, 1.f, 1.f},
      1.f,
      "fill",
      "none",
      "#44C1A1",
      0.f,
      1.f,
      1.f,
      0.f,
  });

  const uint32_t leftRed = 0xffff0000u;
  const uint32_t rightBlue = 0xff0000ffu;
  const auto source = makeLeftRightBandFrame("camera", 100, 100, 1600, 900, leftRed, rightBlue);
  const auto frame = modules.compositor->render(renderPlan, {source});

  EXPECT_EQ(previewPixelRgba(frame.preview, frame.preview.width / 4, frame.preview.height / 2), rightBlue);
}

TEST(StubCompositor, MarksWarnedRenderPlansDegraded) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);

  auto renderPlan = overlappingSceneGraphPlan();
  renderPlan.warnings.push_back("missing overlay asset");

  const auto frame = modules.compositor->render(renderPlan, {makeMetadataFrame("front"), makeMetadataFrame("back")});
  EXPECT_EQ(frame.health, "degraded");
  EXPECT_NE(frame.programPixelSignature, 0u);
}

TEST(StubCompositor, ValidatesInvalidRenderPlansButStillProducesDeterministicFrame) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);

  corevideo::modules::CompositorRenderPlan renderPlan;
  renderPlan.width = -1;
  renderPlan.height = 0;
  renderPlan.fps = 0;
  renderPlan.layers.push_back({
      "",
      "unknown-layer-kind",
      "",
      "",
      0,
      {0.f, 0.f, -1.f, 1.f},
      std::numeric_limits<float>::quiet_NaN(),
  });

  const auto first = modules.compositor->render(renderPlan, {});
  const auto second = modules.compositor->render(renderPlan, {});

  EXPECT_EQ(first.renderer, "software");
  EXPECT_EQ(first.health, "degraded");
  EXPECT_EQ(first.width, 1920);
  EXPECT_EQ(first.height, 1080);
  EXPECT_EQ(first.renderPlanId, "invalid-render-plan");
  EXPECT_GE(first.warnings.size(), 5u);
  EXPECT_NE(first.programPixelSignature, 0u);
  EXPECT_NE(first.renderPlanSignature, 0u);
  EXPECT_EQ(first.programPixelSignature, second.programPixelSignature);
  EXPECT_EQ(first.renderPlanSignature, second.renderPlanSignature);
}

TEST(StubCompositor, AppliesSemanticOverlayDepthAndOpacity) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);

  corevideo::modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = "b3-overlay-depth";
  renderPlan.width = 640;
  renderPlan.height = 360;
  renderPlan.layers.push_back({
      "overlay:lower-third",
      "overlay",
      "",
      "",
      0,
      {0.f, 0.f, 0.f, 0.f},
      0.5f,
  });
  renderPlan.layers.push_back({
      "speaker",
      "participant-video",
      "zoom:speaker",
      "speaker",
      100,
      {0.f, 0.f, 1.f, 1.f},
      1.f,
  });

  const auto frame = modules.compositor->render(renderPlan, {makeMetadataFrame("speaker")});
  const int sampleX = frame.preview.width / 2;
  const int sampleY = static_cast<int>(frame.preview.height * 0.84f);
  const uint32_t participantColor = corevideo::compositor::colorFromParticipantId("speaker");
  const uint32_t overlayColor = 0xff2a3548u;

  EXPECT_EQ(previewPixelRgba(frame.preview, sampleX, sampleY), blendRgbaOver(overlayColor, participantColor, 0.5f));
}

namespace {

// Counts the distinct RGBA values in a normalized sub-rectangle of the preview.
// A real text/image raster produces many distinct values; a solid fill yields 1.
size_t distinctColorsInRegion(
    const corevideo::modules::ProgramFramePreviewPixels& preview,
    float x,
    float y,
    float w,
    float h) {
  std::set<uint32_t> colors;
  const int left = std::max(0, static_cast<int>(x * preview.width));
  const int top = std::max(0, static_cast<int>(y * preview.height));
  const int right = std::min(preview.width, static_cast<int>((x + w) * preview.width));
  const int bottom = std::min(preview.height, static_cast<int>((y + h) * preview.height));
  for (int py = top; py < bottom; ++py) {
    for (int px = left; px < right; ++px) {
      colors.insert(previewPixelRgba(preview, px, py));
    }
  }
  return colors.size();
}

// Builds a VideoFrame whose top half is `top` and bottom half is `bottom`
// (packed 0xAARRGGBB), so framing crops can be verified by which band survives.
corevideo::modules::VideoFrame makeHalfBandFrame(
    const std::string& participantId,
    int width,
    int height,
    uint32_t topRgba,
    uint32_t bottomRgba) {
  corevideo::modules::VideoFrame frame;
  frame.participantId = participantId;
  frame.width = width;
  frame.height = height;
  frame.naturalWidth = width;
  frame.naturalHeight = height;
  frame.pixelWidth = width;
  frame.pixelHeight = height;
  frame.pixelStride = width * 4;
  auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    const uint32_t rgba = y < height / 2 ? topRgba : bottomRgba;
    for (int x = 0; x < width; ++x) {
      const size_t offset = static_cast<size_t>((y * width + x) * 4);
      (*pixels)[offset + 0] = static_cast<uint8_t>(rgba & 0xff);
      (*pixels)[offset + 1] = static_cast<uint8_t>((rgba >> 8) & 0xff);
      (*pixels)[offset + 2] = static_cast<uint8_t>((rgba >> 16) & 0xff);
      (*pixels)[offset + 3] = static_cast<uint8_t>((rgba >> 24) & 0xff);
    }
  }
  frame.pixels = pixels;
  return frame;
}

corevideo::modules::VideoFrame makeVerticalBandFrame(
    const std::string& participantId,
    int width,
    int height,
    uint32_t leftRgba,
    uint32_t rightRgba) {
  corevideo::modules::VideoFrame frame;
  frame.participantId = participantId;
  frame.width = width;
  frame.height = height;
  frame.naturalWidth = width;
  frame.naturalHeight = height;
  frame.pixelWidth = width;
  frame.pixelHeight = height;
  frame.pixelStride = width * 4;
  auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const uint32_t rgba = x < width / 2 ? leftRgba : rightRgba;
      const size_t offset = static_cast<size_t>((y * width + x) * 4);
      (*pixels)[offset + 0] = static_cast<uint8_t>(rgba & 0xff);
      (*pixels)[offset + 1] = static_cast<uint8_t>((rgba >> 8) & 0xff);
      (*pixels)[offset + 2] = static_cast<uint8_t>((rgba >> 16) & 0xff);
      (*pixels)[offset + 3] = static_cast<uint8_t>((rgba >> 24) & 0xff);
    }
  }
  frame.pixels = pixels;
  return frame;
}

corevideo::modules::CompositorRenderPlanLayer makeOverlayLayer(
    const std::string& layerId,
    float rx,
    float ry,
    float rw,
    float rh) {
  corevideo::modules::CompositorRenderPlanLayer layer;
  layer.layerId = layerId;
  layer.kind = "overlay";
  layer.order = 10;
  layer.rect = {rx, ry, rw, rh};
  layer.opacity = 0.95f;
  layer.hasOverlayContent = true;
  return layer;
}

}  // namespace

TEST(StubCompositor, FramingStretchSamplesFullSourceTopAndBottom) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);

  corevideo::modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = "f3-stretch";
  renderPlan.width = 1280;
  renderPlan.height = 720;
  corevideo::modules::CompositorRenderPlanLayer layer{
      "src", "participant-video", "zoom:src", "src", 0, {0.f, 0.f, 1.f, 1.f}, 1.f};
  layer.fitMode = "stretch";
  layer.borderStyle = "none";
  renderPlan.layers.push_back(layer);

  const uint32_t topColor = 0xffff2020u;
  const uint32_t bottomColor = 0xff2040ffu;
  const auto frame = modules.compositor->render(
      renderPlan, {makeHalfBandFrame("src", 256, 256, topColor, bottomColor)});

  // Stretch maps the whole source to the whole rect: the upper-quarter samples
  // the top band, the lower-quarter samples the bottom band.
  const uint32_t topSample = previewPixelRgba(frame.preview, frame.preview.width / 2, frame.preview.height / 4);
  const uint32_t bottomSample = previewPixelRgba(frame.preview, frame.preview.width / 2, frame.preview.height * 3 / 4);
  EXPECT_EQ(topSample, topColor);
  EXPECT_EQ(bottomSample, bottomColor);
}

TEST(StubCompositor, FramingVerticalPanSelectsSourceBand) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);

  const uint32_t topColor = 0xffff2020u;
  const uint32_t bottomColor = 0xff2040ffu;

  auto renderWithOffset = [&](float offsetY) {
    corevideo::modules::CompositorRenderPlan renderPlan;
    renderPlan.renderPlanId = "f3-pan";
    renderPlan.width = 1280;
    renderPlan.height = 720;
    corevideo::modules::CompositorRenderPlanLayer layer{
        "src", "participant-video", "zoom:src", "src", 0, {0.f, 0.f, 1.f, 1.f}, 1.f};
    layer.fitMode = "stretch";
    layer.borderStyle = "none";
    layer.sourceScale = 4.f;   // Sample a small window...
    layer.sourceOffsetY = offsetY;  // ...panned to the top (-1) or bottom (+1).
    renderPlan.layers.push_back(layer);
    return modules.compositor->render(
        renderPlan, {makeHalfBandFrame("src", 256, 256, topColor, bottomColor)});
  };

  const auto panUp = renderWithOffset(-1.f);
  const auto panDown = renderWithOffset(1.f);
  const int cx = panUp.preview.width / 2;
  const int cy = panUp.preview.height / 2;
  // Panning fully up samples the top band; fully down samples the bottom band.
  EXPECT_EQ(previewPixelRgba(panUp.preview, cx, cy), topColor);
  EXPECT_EQ(previewPixelRgba(panDown.preview, cx, cy), bottomColor);
}

TEST(StubCompositor, FramingFillHorizontalPanUsesOriginalSourceEdges) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);

  const uint32_t leftColor = 0xff40d080u;
  const uint32_t rightColor = 0xff8040ffu;

  auto renderWithOffset = [&](float offsetX) {
    corevideo::modules::CompositorRenderPlan renderPlan;
    renderPlan.renderPlanId = "f3-fill-horizontal-pan";
    renderPlan.width = 900;
    renderPlan.height = 900;
    corevideo::modules::CompositorRenderPlanLayer layer{
        "src", "participant-video", "zoom:src", "src", 0, {0.f, 0.f, 1.f, 1.f}, 1.f};
    layer.fitMode = "fill";
    layer.borderStyle = "none";
    layer.sourceScale = 1.f;
    layer.sourceOffsetX = offsetX;
    renderPlan.layers.push_back(layer);
    return modules.compositor->render(
        renderPlan, {makeVerticalBandFrame("src", 1600, 900, leftColor, rightColor)});
  };

  const auto panLeft = renderWithOffset(-1.f);
  const auto panRight = renderWithOffset(1.f);
  const int cx = panLeft.preview.width / 2;
  const int cy = panLeft.preview.height / 2;

  EXPECT_EQ(previewPixelRgba(panLeft.preview, cx, cy), leftColor);
  EXPECT_EQ(previewPixelRgba(panRight.preview, cx, cy), rightColor);
}

TEST(StubCompositor, LowerThirdOverlayRastersRealTextPixels) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);

  corevideo::modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = "f3-lower-third";
  renderPlan.width = 1280;
  renderPlan.height = 720;
  renderPlan.layers.push_back({
      "speaker",
      "participant-video",
      "zoom:speaker",
      "speaker",
      0,
      {0.f, 0.f, 1.f, 1.f},
      1.f,
  });
  auto overlay = makeOverlayLayer("overlay:lt:lower-third", 0.05f, 0.78f, 0.9f, 0.16f);
  overlay.overlay.title = "ADA OTIENO";
  overlay.overlay.org = "AETHELRED";
  overlay.overlay.keyPhase = "on-air";
  overlay.overlay.keyProgress = 1.f;
  overlay.overlay.brandColor = "#44c1a1";
  renderPlan.layers.push_back(overlay);

  const auto frame = modules.compositor->render(renderPlan, {makeMetadataFrame("speaker")});
  // The overlay band must contain many distinct colors (text glyphs + brand
  // band + accent), proving real raster rather than a single solid rect.
  // The old solid-fill overlay produced exactly one uniform color in the band;
  // a real raster (brand band + accent bar + rastered title/org text) produces
  // several distinct colors, proving non-uniform text pixels.
  const size_t distinct = distinctColorsInRegion(frame.preview, 0.05f, 0.78f, 0.9f, 0.16f);
  EXPECT_TRUE(distinct > 2u) << "distinct overlay colors = " << distinct;
}

TEST(StubCompositor, OverlayTextContentChangesPixels) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);

  auto buildPlan = [](const std::string& title) {
    corevideo::modules::CompositorRenderPlan plan;
    plan.renderPlanId = "f3-text-vary";
    plan.width = 1280;
    plan.height = 720;
    auto overlay = makeOverlayLayer("overlay:lt:lower-third", 0.05f, 0.78f, 0.9f, 0.16f);
    overlay.overlay.title = title;
    overlay.overlay.keyPhase = "on-air";
    overlay.overlay.keyProgress = 1.f;
    plan.layers.push_back(overlay);
    return plan;
  };

  const auto frameA = modules.compositor->render(buildPlan("AAAA TITLE"), {});
  const auto frameB = modules.compositor->render(buildPlan("OOOO OTHER"), {});
  EXPECT_NE(frameA.preview.bgra, frameB.preview.bgra);
}

TEST(StubCompositor, OverlayKeyPhaseAnimatesOverProgress) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);

  auto buildPlan = [](float progress) {
    corevideo::modules::CompositorRenderPlan plan;
    plan.renderPlanId = "f3-anim";
    plan.width = 1280;
    plan.height = 720;
    auto overlay = makeOverlayLayer("overlay:lt:lower-third", 0.05f, 0.78f, 0.9f, 0.16f);
    overlay.overlay.title = "ANIMATED TITLE";
    overlay.overlay.keyPhase = "building-in";
    overlay.overlay.keyProgress = progress;
    plan.layers.push_back(overlay);
    return plan;
  };

  // Early in the build-in the overlay is nearly transparent (few non-background
  // colors); fully on-air it is opaque with rastered text. The pixel buffers
  // must therefore differ across animation progress.
  const auto early = modules.compositor->render(buildPlan(0.f), {});
  const auto late = modules.compositor->render(buildPlan(1.f), {});
  EXPECT_NE(early.preview.bgra, late.preview.bgra);
  EXPECT_TRUE(
      distinctColorsInRegion(late.preview, 0.05f, 0.78f, 0.9f, 0.16f) >
      distinctColorsInRegion(early.preview, 0.05f, 0.78f, 0.9f, 0.16f));
}

TEST(StubCompositor, OverlayAppliesBrandColor) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);

  auto buildPlan = [](const std::string& brandColor) {
    corevideo::modules::CompositorRenderPlan plan;
    plan.renderPlanId = "f3-brand";
    plan.width = 1280;
    plan.height = 720;
    auto overlay = makeOverlayLayer("overlay:lt:lower-third", 0.05f, 0.78f, 0.9f, 0.16f);
    overlay.overlay.title = "BRANDED";
    overlay.overlay.keyPhase = "on-air";
    overlay.overlay.keyProgress = 1.f;
    overlay.overlay.brandColor = brandColor;
    plan.layers.push_back(overlay);
    return plan;
  };

  // Different brand accent colors must change the rendered overlay pixels.
  const auto green = modules.compositor->render(buildPlan("#44c1a1"), {});
  const auto magenta = modules.compositor->render(buildPlan("#ff20a0"), {});
  EXPECT_NE(green.preview.bgra, magenta.preview.bgra);
}

TEST(StubCompositor, CaptionOverlayRastersSpeakerAndBody) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);

  corevideo::modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = "f3-caption";
  renderPlan.width = 1280;
  renderPlan.height = 720;
  auto caption = makeOverlayLayer("overlay:caption", 0.08f, 0.86f, 0.84f, 0.10f);
  caption.overlay.isCaption = true;
  caption.overlay.speaker = "ADA";
  caption.overlay.text = "TESTING CAPTIONS";
  caption.overlay.keyPhase = "on-air";
  caption.overlay.keyProgress = 1.f;
  renderPlan.layers.push_back(caption);

  const auto frame = modules.compositor->render(renderPlan, {});
  EXPECT_TRUE(distinctColorsInRegion(frame.preview, 0.08f, 0.86f, 0.84f, 0.10f) > 3u);
}

TEST(StubCompositor, TransparentChromaKeyLayerRevealsLowerLayer) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);

  corevideo::modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = "b3-chroma-depth";
  renderPlan.width = 640;
  renderPlan.height = 360;
  renderPlan.layers.push_back({
      "back",
      "participant-video",
      "zoom:back",
      "back",
      0,
      {0.f, 0.f, 1.f, 1.f},
      1.f,
  });
  renderPlan.layers.push_back({
      "front:chroma-key",
      "chroma-key",
      "zoom:front",
      "front",
      0,
      {0.f, 0.f, 1.f, 1.f},
      0.f,
  });

  const auto frame = modules.compositor->render(renderPlan, {makeMetadataFrame("back"), makeMetadataFrame("front")});
  EXPECT_EQ(previewPixelRgba(frame.preview, frame.preview.width / 2, frame.preview.height / 2), corevideo::compositor::colorFromParticipantId("back"));
}
#endif

#if COREVIDEO_WITH_D3D11
TEST(D3D11Compositor, ComposesMultiLayerSceneGraphOnGpu) {
  auto compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_NE(compositor, nullptr);

  corevideo::modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = "b3-interview";
  renderPlan.sceneId = "interview";
  renderPlan.width = 640;
  renderPlan.height = 360;
  renderPlan.colorGrade = {0.1f, 0.05f, 0.1f, 0.f};

  const auto leftLayout = corevideo::compositor::gridCell(2, 0);
  const auto rightLayout = corevideo::compositor::gridCell(2, 1);
  renderPlan.layers.push_back({
      "route:speaker",
      "participant-video",
      "zoom:101",
      "101",
      0,
      {leftLayout.x, leftLayout.y, leftLayout.width, leftLayout.height},
      1.f,
  });
  renderPlan.layers.push_back({
      "route:guest",
      "participant-video",
      "zoom:202",
      "202",
      1,
      {rightLayout.x, rightLayout.y, rightLayout.width, rightLayout.height},
      1.f,
  });

  const auto frame = compositor->render(renderPlan, {{"101", 1280, 720, 16}, {"202", 1280, 720, 16}});
  EXPECT_EQ(frame.renderer, "d3d11");
  EXPECT_EQ(frame.renderPlanId, "b3-interview");
  EXPECT_EQ(frame.layerCount, 2);
  EXPECT_TRUE(frame.gpuComposed);
  EXPECT_NE(frame.programPixelSignature, 0u);
  EXPECT_EQ(frame.health, "live");
}

namespace {

// GPU-gated mirrors of the stub-section helpers (that section is compiled only
// under COREVIDEO_STUB; the dev build compiles this one instead).
size_t gpuDistinctColorsInRegion(
    const corevideo::modules::ProgramFramePreviewPixels& preview,
    float x,
    float y,
    float w,
    float h) {
  std::set<uint32_t> colors;
  const int left = std::max(0, static_cast<int>(x * preview.width));
  const int top = std::max(0, static_cast<int>(y * preview.height));
  const int right = std::min(preview.width, static_cast<int>((x + w) * preview.width));
  const int bottom = std::min(preview.height, static_cast<int>((y + h) * preview.height));
  for (int py = top; py < bottom; ++py) {
    for (int px = left; px < right; ++px) {
      colors.insert(previewPixelRgba(preview, px, py));
    }
  }
  return colors.size();
}

corevideo::modules::CompositorRenderPlanLayer makeGpuOverlayLayer(
    const std::string& layerId,
    float rx,
    float ry,
    float rw,
    float rh) {
  corevideo::modules::CompositorRenderPlanLayer layer;
  layer.layerId = layerId;
  layer.kind = "overlay";
  layer.order = 10;
  layer.rect = {rx, ry, rw, rh};
  layer.opacity = 0.95f;
  layer.hasOverlayContent = true;
  return layer;
}

}  // namespace

// The DirectWrite/D2D overlay raster: a lower-third must produce real glyph
// pixels (many distinct colors), not the solid band + accent fallback.
TEST(D3D11Compositor, LowerThirdOverlayRastersDirectWriteTextOnGpu) {
  auto compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_NE(compositor, nullptr);

  corevideo::modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = "dwrite-lower-third";
  renderPlan.width = 1280;
  renderPlan.height = 720;
  auto overlay = makeGpuOverlayLayer("overlay:lt:lower-third", 0.05f, 0.78f, 0.9f, 0.16f);
  overlay.overlay.title = "ADA OTIENO";
  overlay.overlay.org = "AETHELRED BROADCASTING";
  overlay.overlay.keyPhase = "on-air";
  overlay.overlay.keyProgress = 1.f;
  overlay.overlay.brandColor = "#44c1a1";
  renderPlan.layers.push_back(overlay);

  const auto frame = compositor->render(renderPlan, {});
  EXPECT_TRUE(frame.gpuComposed);
  const size_t distinct = gpuDistinctColorsInRegion(frame.preview, 0.05f, 0.78f, 0.9f, 0.16f);
  EXPECT_TRUE(distinct > 3u) << "distinct overlay colors = " << distinct;
}

// Different overlay text must change the rendered pixels — this fails if the
// raster silently fell back to the (text-blind) band + accent draw, so it is
// the assertion that DirectWrite glyphs actually landed in the frame.
TEST(D3D11Compositor, OverlayTextContentChangesPixelsOnGpu) {
  auto compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_NE(compositor, nullptr);

  auto buildPlan = [](const std::string& title) {
    corevideo::modules::CompositorRenderPlan plan;
    plan.renderPlanId = "dwrite-text-vary";
    plan.width = 1280;
    plan.height = 720;
    auto overlay = makeGpuOverlayLayer("overlay:lt:lower-third", 0.05f, 0.78f, 0.9f, 0.16f);
    overlay.overlay.title = title;
    overlay.overlay.keyPhase = "on-air";
    overlay.overlay.keyProgress = 1.f;
    plan.layers.push_back(overlay);
    return plan;
  };

  const auto frameA = compositor->render(buildPlan("AAAA TITLE"), {});
  const auto frameB = compositor->render(buildPlan("OOOO OTHER"), {});
  ASSERT_FALSE(frameA.preview.bgra.empty());
  EXPECT_NE(frameA.preview.bgra, frameB.preview.bgra);
}

// Captions ride the same raster stage (isCaption selects the speaker/body
// layout and the top accent strip).
TEST(D3D11Compositor, CaptionRastersSpeakerAndBodyOnGpu) {
  auto compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_NE(compositor, nullptr);

  corevideo::modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = "dwrite-caption";
  renderPlan.width = 1280;
  renderPlan.height = 720;
  auto overlay = makeGpuOverlayLayer("overlay:caption", 0.08f, 0.86f, 0.84f, 0.10f);
  overlay.overlay.isCaption = true;
  overlay.overlay.speaker = "ADA";
  overlay.overlay.text = "THE OVERLAY TEXT NOW RENDERS ON THE GPU.";
  overlay.overlay.keyPhase = "on-air";
  overlay.overlay.keyProgress = 1.f;
  renderPlan.layers.push_back(overlay);

  const auto frame = compositor->render(renderPlan, {});
  EXPECT_TRUE(frame.gpuComposed);
  const size_t distinct = gpuDistinctColorsInRegion(frame.preview, 0.08f, 0.86f, 0.84f, 0.10f);
  EXPECT_TRUE(distinct > 3u) << "distinct caption colors = " << distinct;
}

// The raster cache: rendering the same overlay twice must reuse the texture
// (same pixels out), and a brand-color change must invalidate it.
TEST(D3D11Compositor, OverlayRasterCacheReusesAndInvalidates) {
  auto compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_NE(compositor, nullptr);

  auto buildPlan = [](const std::string& brandColor) {
    corevideo::modules::CompositorRenderPlan plan;
    plan.renderPlanId = "dwrite-cache";
    plan.width = 1280;
    plan.height = 720;
    auto overlay = makeGpuOverlayLayer("overlay:lt:lower-third", 0.05f, 0.78f, 0.9f, 0.16f);
    overlay.overlay.title = "CACHE CHECK";
    overlay.overlay.keyPhase = "on-air";
    overlay.overlay.keyProgress = 1.f;
    overlay.overlay.brandColor = brandColor;
    plan.layers.push_back(overlay);
    return plan;
  };

  const auto first = compositor->render(buildPlan("#44c1a1"), {});
  const auto second = compositor->render(buildPlan("#44c1a1"), {});
  const auto recolored = compositor->render(buildPlan("#d04040"), {});
  EXPECT_EQ(first.preview.bgra, second.preview.bgra);
  EXPECT_NE(first.preview.bgra, recolored.preview.bgra);
}
#endif
