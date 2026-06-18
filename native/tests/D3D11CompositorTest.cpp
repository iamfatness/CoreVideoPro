#include "compositor/CompositorLayout.h"
#include "modules/Interfaces.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
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
  const auto frame = modules.compositor->render(renderPlan, {{"front", 1280, 720, 16}, {"back", 1280, 720, 16}});

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

  const std::vector<corevideo::modules::VideoFrame> frames = {{"front", 1280, 720, 16}, {"back", 1280, 720, 16}};
  const auto first = modules.compositor->render(firstPlan, frames);
  const auto second = modules.compositor->render(secondPlan, frames);

  EXPECT_EQ(first.programPixelSignature, second.programPixelSignature);
  EXPECT_EQ(first.preview.width, second.preview.width);
  EXPECT_EQ(first.preview.height, second.preview.height);
  EXPECT_EQ(first.preview.bgra, second.preview.bgra);
}

TEST(StubCompositor, MarksWarnedRenderPlansDegraded) {
  auto modules = corevideo::modules::createStubModules();
  ASSERT_NE(modules.compositor, nullptr);

  auto renderPlan = overlappingSceneGraphPlan();
  renderPlan.warnings.push_back("missing overlay asset");

  const auto frame = modules.compositor->render(renderPlan, {{"front", 1280, 720, 16}, {"back", 1280, 720, 16}});
  EXPECT_EQ(frame.health, "degraded");
  EXPECT_NE(frame.programPixelSignature, 0u);
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
#endif
