#include "core/ProgramBlackout.h"
#include "core/MediaCore.h"
#include <gtest/gtest.h>

using namespace corevideo;
TEST(ProgramBlackout, ReversesFromCurrentOpacityAndRepeatedSetDoesNotRestart) {
  core::ProgramBlackout fade;
  fade.set(true); fade.advance(150);
  EXPECT_NEAR(fade.opacity(), .5f, .001f);
  fade.set(true); fade.advance(150);
  EXPECT_EQ(fade.opacity(), 1.f);
  fade.set(false); fade.advance(150);
  EXPECT_NEAR(fade.opacity(), .5f, .001f);
  fade.set(true); fade.advance(150);
  EXPECT_NEAR(fade.opacity(), .75f, .001f);
  fade.advance(150); EXPECT_EQ(fade.opacity(), 1.f);
  fade.set(false); fade.advance(300); EXPECT_EQ(fade.opacity(), 0.f);
}
TEST(ProgramBlackout, CoversAllGraphicsAndIgnoresSceneGrade) {
  core::ProgramBlackout fade; fade.set(true, 0);
  modules::CompositorRenderPlan plan;
  modules::CompositorRenderPlanLayer graphic;
  graphic.layerId = "global-graphic"; graphic.order = 200000;
  plan.layers.push_back(graphic); fade.append(plan);
  ASSERT_EQ(plan.layers.size(), 2u);
  const auto& black = plan.layers.back();
  EXPECT_GT(black.order, graphic.order); EXPECT_EQ(black.opacity, 1.f);
  EXPECT_TRUE(black.hasColorGrade); EXPECT_EQ(black.fillColor, "#000000");
  EXPECT_EQ(black.rect.width, 1.f); EXPECT_EQ(black.rect.height, 1.f);
}
TEST(ProgramBlackout, CorePersistsBlackAcrossSceneTakeAndReportsState) {
  core::MediaCore core;
  auto command = [](const char* text) { return *rpc::Json::parse(text); };
  core.applyCommands({command(R"({"type":"program-blackout","enabled":true,"durationMs":0})")});
  core.applyCommands({command(R"({"type":"begin-take-transition","mode":"cut"})"),
                     command(R"({"type":"load-scene-graph","sceneId":"new-scene","routes":[],"tiles":{"layerId":"test-wall","members":[]}})")});
  auto state = core.sessionState();
  ASSERT_TRUE(state.get("programBlackout"));
  EXPECT_TRUE(state.get("programBlackout")->asBool());
  EXPECT_EQ(state.getNumber("programBlackoutOpacity"), 1);
  ASSERT_FALSE(core.lastRenderPlanForTest().layers.empty());
  EXPECT_EQ(core.lastRenderPlanForTest().layers.back().layerId, "program-fade-to-black");
  core.applyCommands({command(R"({"type":"program-blackout","enabled":false,"durationMs":0})")});
  state = core.sessionState();
  EXPECT_FALSE(state.get("programBlackout")->asBool());
  EXPECT_EQ(state.getNumber("programBlackoutOpacity"), 0);
}
