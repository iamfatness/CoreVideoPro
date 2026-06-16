#include "compositor/CompositorLayout.h"
#include "modules/Interfaces.h"

#include <gtest/gtest.h>

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