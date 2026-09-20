// Task 2 (#535 slice 4a, "bus health on air"): the compositors stop painting
// the per-id "pink tile" placeholder and resolve one rule from the layer's
// sourceHealth / dropoutPolicy / sourceDisplayName instead. These tests pin
// that rule on the CPU preview path (fillSyntheticProgramFramePreview, the
// same function the stub compositor's render() calls) so it runs on every
// platform, including the CI stub build. See
// docs/superpowers/plans/2026-09-19-source-bus-slice4a-health-on-air.md
// Global Constraints "One resolution rule".

#include "compositor/CompositorLayout.h"
#include "modules/Interfaces.h"
#include "modules/ProgramFramePreview.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
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

// A full-canvas solid frame, so the centre pixel is unambiguous.
corevideo::modules::VideoFrame makeSolidFrame(const std::string& participantId, uint32_t rgba, int width = 640, int height = 360) {
  corevideo::modules::VideoFrame frame;
  frame.participantId = participantId;
  frame.width = width;
  frame.height = height;
  frame.naturalWidth = width;
  frame.naturalHeight = height;
  frame.pixelWidth = width;
  frame.pixelHeight = height;
  frame.pixelStride = width * 4;
  frame.timestampMs = 16;
  auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width) * height * 4);
  for (size_t i = 0; i < pixels->size(); i += 4) {
    (*pixels)[i + 0] = static_cast<uint8_t>(rgba & 0xff);
    (*pixels)[i + 1] = static_cast<uint8_t>((rgba >> 8) & 0xff);
    (*pixels)[i + 2] = static_cast<uint8_t>((rgba >> 16) & 0xff);
    (*pixels)[i + 3] = static_cast<uint8_t>((rgba >> 24) & 0xff);
  }
  frame.pixels = std::move(pixels);
  return frame;
}

// One full-canvas layer at rect (0,0,1,1) referencing "capture:a".
corevideo::modules::CompositorRenderPlanLayer makeSourceLayer(const std::string& sourceHealth, const std::string& dropoutPolicy = "hold") {
  corevideo::modules::CompositorRenderPlanLayer layer;
  layer.layerId = "layer-a";
  layer.kind = "participant-video";
  layer.sourceId = "capture:a";
  layer.participantId = "capture:a";
  layer.order = 0;
  layer.rect = {0.f, 0.f, 1.f, 1.f};
  layer.opacity = 1.f;
  layer.sourceHealth = sourceHealth;
  layer.dropoutPolicy = dropoutPolicy;
  return layer;
}

corevideo::modules::CompositorRenderPlan makePlan(corevideo::modules::CompositorRenderPlanLayer layer) {
  corevideo::modules::CompositorRenderPlan plan;
  plan.renderPlanId = "health-test";
  plan.sceneId = "health-test-scene";
  plan.width = 640;
  plan.height = 360;
  plan.layers.push_back(std::move(layer));
  return plan;
}

uint32_t renderCentrePixel(const corevideo::modules::CompositorRenderPlan& plan, const std::vector<corevideo::modules::VideoFrame>& frames) {
  corevideo::modules::ProgramFramePreviewPixels preview;
  corevideo::modules::ProgramFrame programFrame;
  corevideo::modules::fillSyntheticProgramFramePreview(preview, plan, frames, programFrame);
  return previewPixelRgba(preview, preview.width / 2, preview.height / 2);
}

}  // namespace

TEST(ProgramFramePreviewHealth, NoFrameWarmingDrawsTheWarmingSlate) {
  const auto plan = makePlan(makeSourceLayer("warming"));
  EXPECT_EQ(renderCentrePixel(plan, {}), corevideo::compositor::kWarmingSlateRgba);
}

TEST(ProgramFramePreviewHealth, NoFrameFailedDrawsTheFailedSlate) {
  const auto plan = makePlan(makeSourceLayer("failed"));
  EXPECT_EQ(renderCentrePixel(plan, {}), corevideo::compositor::kFailedSlateRgba);
}

TEST(ProgramFramePreviewHealth, NoFrameUnknownHealthDrawsTheWarmingSlateNotAnIdColour) {
  const auto plan = makePlan(makeSourceLayer(""));
  const uint32_t pixel = renderCentrePixel(plan, {});
  EXPECT_EQ(pixel, corevideo::compositor::kWarmingSlateRgba);
  EXPECT_NE(pixel, corevideo::compositor::colorFromParticipantId("capture:a"));
}

TEST(ProgramFramePreviewHealth, StalledWithBlackPolicyDrawsBlackEvenWithAHeldFrame) {
  constexpr uint32_t kGreen = 0xff00ff00u;
  const auto plan = makePlan(makeSourceLayer("stalled", "black"));
  const auto frames = std::vector<corevideo::modules::VideoFrame>{makeSolidFrame("capture:a", kGreen)};
  EXPECT_EQ(renderCentrePixel(plan, frames), corevideo::compositor::kDropoutBlackRgba);
}

TEST(ProgramFramePreviewHealth, StalledWithHoldPolicyDrawsTheHeldFrame) {
  constexpr uint32_t kGreen = 0xff00ff00u;
  const auto plan = makePlan(makeSourceLayer("stalled", "hold"));
  const auto frames = std::vector<corevideo::modules::VideoFrame>{makeSolidFrame("capture:a", kGreen)};
  EXPECT_EQ(renderCentrePixel(plan, frames), kGreen);
}

TEST(ProgramFramePreviewHealth, MediaLayerUsesTheSameRule) {
  corevideo::modules::CompositorRenderPlanLayer layer;
  layer.layerId = "layer-media";
  layer.kind = "media-video";
  layer.mediaAssetId = "clip-1";
  layer.order = 0;
  layer.rect = {0.f, 0.f, 1.f, 1.f};
  layer.opacity = 1.f;
  layer.sourceHealth = "failed";
  const auto plan = makePlan(std::move(layer));
  EXPECT_EQ(renderCentrePixel(plan, {}), corevideo::compositor::kFailedSlateRgba);
}
