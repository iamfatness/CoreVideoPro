#include "compositor/TilesAnimator.h"
#include "compositor/TilesPlanAnimation.h"
#include <gtest/gtest.h>

using namespace corevideo::compositor;
namespace {
TilesAnimationTarget tile(const char* id, float x = 0, float width = 1) {
  return {id, {x, 0, width, 1}};
}
TEST(TilesAnimator, EmptyAdoptionDoesNotMakeFirstArrivalPopIn) {
  TilesAnimator animator;
  EXPECT_TRUE(animator.sample({}, 0, true, 350).empty());
  auto frame = animator.sample({tile("a")}, 16, true, 350);
  ASSERT_EQ(frame.size(), 1u);
  EXPECT_EQ(frame[0].alpha, 0.f);
  EXPECT_FALSE(frame[0].atRest);
  frame = animator.sample({tile("a")}, 191, true, 350);
  EXPECT_NEAR(frame[0].alpha, 0.5, 1e-6);
  frame = animator.sample({tile("a")}, 366, true, 350);
  EXPECT_EQ(frame[0].alpha, 1.f);
  EXPECT_TRUE(frame[0].atRest);
}
TEST(TilesAnimator, ExistingWallIsAdoptedAndDisabledClearsMotion) {
  TilesAnimator animator;
  EXPECT_EQ(animator.sample({tile("a")}, 0, true, 350)[0].alpha, 1.f);
  auto frame = animator.sample({tile("a", 0, .5f), tile("b", .5f, .5f)}, 16, true, 350);
  EXPECT_EQ(frame[1].alpha, 0.f);
  frame = animator.sample({tile("a", 0, .5f), tile("b", .5f, .5f)}, 17, false, 350);
  EXPECT_EQ(frame[1].alpha, 1.f);
  EXPECT_EQ(frame[0].rect.width, .5f);
  frame = animator.sample({tile("a", 0, .5f), tile("b", .5f, .5f)}, 18, true, 350);
  EXPECT_EQ(frame[1].alpha, 1.f);
  EXPECT_TRUE(frame[0].atRest);
}
TEST(TilesAnimator, DepartureIsImmediateAndReturnFadesAtItsNewSlot) {
  TilesAnimator animator;
  animator.sample({tile("a", 0, .5f), tile("b", .5f, .5f)}, 0, true, 350);
  auto frame = animator.sample({tile("a")}, 16, true, 350);
  ASSERT_EQ(frame.size(), 1u);
  EXPECT_EQ(frame[0].id, "a");
  frame = animator.sample({tile("a", 0, .5f), tile("b", .5f, .5f)}, 32, true, 350);
  EXPECT_EQ(frame[1].alpha, 0.f);
  EXPECT_EQ(frame[1].rect.x, .5f);
}
TEST(TilesAnimator, SameTimeRetargetPreservesCurrentPosition) {
  TilesAnimator animator;
  animator.sample({tile("a", 0, .25f)}, 0, true, 350);
  auto moving = animator.sample({tile("a", .75f, .25f)}, 100, true, 350);
  auto reversed = animator.sample({tile("a", 0, .25f)}, 100, true, 350);
  EXPECT_EQ(reversed[0].rect.x, moving[0].rect.x);
  auto settled = animator.sample({tile("a", 0, .25f)}, 5000, true, 350);
  EXPECT_TRUE(settled[0].atRest);
  EXPECT_EQ(settled[0].rect.x, 0.f);
}
TEST(TilesAnimator, ElapsedTimeNotTickCountControlsMovement) {
  TilesAnimator frequent, sparse;
  frequent.sample({tile("a", 0, .25f)}, 0, true, 350);
  sparse.sample({tile("a", 0, .25f)}, 0, true, 350);
  std::vector<AnimatedTile> frame;
  for (int ms = 10; ms <= 100; ms += 10)
    frame = frequent.sample({tile("a", .75f, .25f)}, ms, true, 350);
  auto other = sparse.sample({tile("a", .75f, .25f)}, 100, true, 350);
  EXPECT_NEAR(frame[0].rect.x, other[0].rect.x, 1e-6);
}
TEST(TilesAnimator, DuplicateIdsAndDeparturesCannotGrowRetainedState) {
  TilesAnimator animator;
  auto frame = animator.sample({tile("a"), tile("a"), tile("b")}, 0, true, 350);
  ASSERT_EQ(frame.size(), 2u);
  EXPECT_TRUE(animator.sample({}, 16, true, 350).empty());
  frame = animator.sample({tile("b")}, 32, true, 350);
  ASSERT_EQ(frame.size(), 1u);
  EXPECT_EQ(frame[0].alpha, 0.f);
}
TEST(TilesAnimator, PlanSamplingKeepsGlowAndVideoTogetherWithoutAdvancingOnRead) {
  TilesPlanAnimation animation;
  corevideo::modules::CompositorRenderPlan empty;
  animation.advance(empty, "scene:wall", true, true, 350, 0);
  corevideo::modules::CompositorRenderPlan plan;
  corevideo::modules::CompositorRenderPlanLayer video;
  video.kind = "participant-video"; video.layerId = "tile:zoom:42";
  video.rect = {.25f, 0, .5f, 1};
  auto glow = video; glow.kind = "tiles-glow"; glow.layerId = "tiles-glow:zoom:42";
  plan.layers = {video, glow};
  animation.advance(plan, "scene:wall", true, true, 350, 16);
  EXPECT_EQ(plan.layers[0].opacity, 0.f);
  EXPECT_EQ(plan.layers[1].opacity, 0.f);
  plan.layers = {video, glow};
  animation.applyLatest(plan, "scene:wall");
  EXPECT_EQ(plan.layers[0].opacity, 0.f);
  EXPECT_EQ(plan.layers[1].opacity, 0.f);
  plan.layers = {video, glow};
  animation.advance(plan, "scene:wall", true, true, 350, 191);
  EXPECT_NEAR(plan.layers[0].opacity, .5, 1e-6);
  EXPECT_EQ(plan.layers[0].opacity, plan.layers[1].opacity);
  EXPECT_EQ(plan.layers[0].rect.x, plan.layers[1].rect.x);
}
TEST(TilesAnimator, DifferentWallCannotReuseAnotherWallsCachedGeometry) {
  TilesPlanAnimation animation;
  corevideo::modules::CompositorRenderPlan plan;
  corevideo::modules::CompositorRenderPlanLayer video;
  video.kind = "participant-video"; video.layerId = "tile:zoom:42";
  video.rect = {0, 0, .5f, 1}; plan.layers = {video};
  animation.advance(plan, "a", true, true, 350, 0);
  video.rect.x = .5f; plan.layers = {video};
  animation.applyLatest(plan, "b");
  EXPECT_EQ(plan.layers[0].rect.x, .5f);
  animation.advance(plan, "b", true, true, 350, 16);
  EXPECT_EQ(plan.layers[0].rect.x, .5f);
  EXPECT_EQ(plan.layers[0].opacity, 1.f);
}
}
