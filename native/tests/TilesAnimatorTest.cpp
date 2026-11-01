#include "compositor/TilesAnimator.h"
#include "compositor/TilesPlanAnimation.h"
#include "core/TilesWallSource.h"
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
// The take hand-off (owner report 2026-09-09, #448). A wall that changes BUS
// is the SAME wall — and since #448 it is, literally: `core::TilesWallSource`
// gives a wall ONE animator shared by both buses, so a Take need not move or
// adopt anything. There is no per-bus pair left to hand off between.
corevideo::modules::CompositorRenderPlan wallPlan(std::initializer_list<TilesAnimationTarget> tiles) {
  corevideo::modules::CompositorRenderPlan plan;
  for (const auto& tile : tiles) {
    corevideo::modules::CompositorRenderPlanLayer layer;
    layer.kind = "participant-video";
    layer.layerId = "tile:" + tile.id;
    layer.rect = {tile.rect.x, tile.rect.y, tile.rect.width, tile.rect.height};
    plan.layers.push_back(layer);
  }
  return plan;
}
// Was: "a settled wall moves bus without replaying its entrance" via
// adoptSettledFrom(). Now: a wall settled on one bus is ALREADY settled on the
// other, because there is only one animator object and both buses read it.
TEST(TilesAnimator, AWallSettledOnOneBusIsAlreadySettledOnTheOtherBecauseItIsTheSameObject) {
  corevideo::core::TilesWallSources walls;
  auto& wall = walls.forWall("tiles:gallery");
  auto plan = wallPlan({tile("a", 0, .5f), tile("b", .5f, .5f)});
  wall.advance(plan, "tiles:gallery", true, true, 350, 0);
  plan = wallPlan({tile("a", 0, .5f), tile("b", .5f, .5f)});
  wall.advance(plan, "tiles:gallery", true, true, 350, 5000);
  ASSERT_EQ(plan.layers[0].opacity, 1.f);
  const auto generationBefore = wall.generation();

  // "Taking" it is just another applyLatest/advance call against the SAME
  // wallId — no hand-off call, no second object, nothing to adopt.
  auto onProgram = wallPlan({tile("a", 0, .5f), tile("b", .5f, .5f)});
  wall.applyLatest(onProgram, "tiles:gallery");
  EXPECT_EQ(onProgram.layers[0].opacity, 1.f);
  EXPECT_EQ(onProgram.layers[1].opacity, 1.f);

  plan = wallPlan({tile("a", 0, .5f), tile("b", .5f, .5f)});
  wall.advance(plan, "tiles:gallery", true, true, 350, 5016);
  EXPECT_EQ(plan.layers[0].opacity, 1.f);
  EXPECT_EQ(plan.layers[1].opacity, 1.f);
  EXPECT_EQ(wall.generation(), generationBefore)
      << "continuing on the other bus must not bump the wall's generation";
}
// Was: "hand-off is refused for a different or unsettled wall" via
// adoptSettledFrom() returning false. There is no hand-off left to refuse —
// each wall id gets its OWN TilesWallSource, so a wall that was never cued on
// either bus simply starts cold: generation 0, full entry animation.
// NOTE on this rewrite: the underlying TilesAnimator treats an animator's
// truly FIRST-ever sample() call as an "adoption" (see
// TilesAnimator::sample's `adopted_`/`adoption` — proven by the pre-existing
// DifferentWallCannotReuseAnotherWallsCachedGeometry test above, which was
// NOT touched by #448 and still asserts alpha==1 on a fresh animator's first
// tick). So a wall cued cold with content on its very first tick renders
// SETTLED, not fading in from alpha 0 — there is no earlier state for it to
// contradict. What #448 needed pinned is narrower and still true: the wall
// starts at generation 0 before anything has cued it, and a genuinely
// different wall id is a SEPARATE object that can never inherit this one's
// geometry or generation — there is no hand-off left to (correctly) refuse.
TEST(TilesAnimator, AWallThatWasNeverCuedStartsCold) {
  corevideo::core::TilesWallSources walls;
  ASSERT_EQ(walls.find("tiles:gallery"), nullptr)
      << "an uncued wall must not exist yet — find() must never insert";
  auto& wall = walls.forWall("tiles:gallery");
  EXPECT_EQ(wall.generation(), 0u);

  auto arriving = wallPlan({tile("a", .25f, .3f)});
  wall.advance(arriving, "tiles:gallery", true, true, 350, 0);
  EXPECT_EQ(arriving.layers[0].rect.x, .25f);
  EXPECT_EQ(arriving.layers[0].rect.width, .3f);

  // A DIFFERENT wall id is a genuinely different object and also starts cold
  // at generation 0 — it cannot inherit this one's geometry or generation.
  auto& other = walls.forWall("tiles:other");
  EXPECT_EQ(other.generation(), 0u);
  auto otherArriving = wallPlan({tile("a")});
  other.advance(otherArriving, "tiles:other", true, true, 350, 16);
  EXPECT_NE(otherArriving.layers[0].rect.x, arriving.layers[0].rect.x)
      << "the second wall inherited the first wall's geometry";
}
// This guard matters MORE with one shared animator per wall (#448): a wipe
// here now hits whichever bus is looking at the wall, not just one of a pair.
TEST(TilesAnimator, AnAllStaleBeatDoesNotWipeAWallThatIsAlreadyDrawn) {
  TilesPlanAnimation animation;
  auto plan = wallPlan({tile("a")});
  animation.advance(plan, "gallery:tiles:gallery", true, true, 350, 0);
  auto lapsed = wallPlan({});
  animation.advance(lapsed, "gallery:tiles:gallery", true, true, 350, 16);
  auto returned = wallPlan({tile("a")});
  animation.advance(returned, "gallery:tiles:gallery", true, true, 350, 32);
  EXPECT_EQ(returned.layers[0].opacity, 1.f)
      << "a momentary all-stale beat replayed the whole wall's entrance";

  // A DIFFERENT wall still starts cold — retained geometry is never reused.
  auto other = wallPlan({});
  animation.advance(other, "other:tiles:other", true, true, 350, 48);
  auto otherArriving = wallPlan({tile("a")});
  animation.advance(otherArriving, "other:tiles:other", true, true, 350, 64);
  EXPECT_EQ(otherArriving.layers[0].opacity, 0.f);
}
}

// #1073: a tile's name line (compositor/TilesNameLine.h) rides its tile.
TEST(TilesAnimator, ANameLineMovesAndFadesWithItsTile) {
  TilesPlanAnimation animation;
  corevideo::modules::CompositorRenderPlan plan;
  corevideo::modules::CompositorRenderPlanLayer video;
  video.kind = "participant-video"; video.layerId = "tile:zoom:42";
  video.rect = {.5f, .5f, .5f, .5f};
  LowerThirdAppearance style; style.enabled = true;
  const auto placed = placeTilesNameLine({.5f, .5f, .5f, .5f}, .004f, .007f, style);
  ASSERT_TRUE(placed.shown);
  corevideo::modules::CompositorRenderPlanLayer name;
  name.kind = "overlay"; name.layerId = "tile-name:zoom:42"; name.hasOverlayContent = true;
  name.overlay.appearance = placed.appearance;
  name.rect = {placed.rect.x, placed.rect.y, placed.rect.width, placed.rect.height};
  // Settle the tile at a full-canvas rect first, then move it to the 2x2 cell.
  auto full = video; full.rect = {0, 0, 1, 1};
  corevideo::modules::CompositorRenderPlan first; first.layers = {full};
  animation.advance(first, "scene:wall", true, true, 350, 0);
  plan.layers = {video, name};
  animation.advance(plan, "scene:wall", true, true, 350, 16);
  const auto& tile = plan.layers[0];
  const auto& line = plan.layers[1];
  // Mid-flight: the tile has not reached its target yet...
  EXPECT_GT(tile.rect.width, .5f);
  // ...and the name keeps the same offset from the tile's bottom-left corner
  // it will have at rest, with the same size and the same alpha.
  EXPECT_NEAR(line.rect.x - tile.rect.x, placed.rect.x - .5f, 1e-6f);
  EXPECT_NEAR((tile.rect.y + tile.rect.height) - (line.rect.y + line.rect.height),
              1.f - (placed.rect.y + placed.rect.height), 1e-6f);
  EXPECT_EQ(line.rect.width, placed.rect.width);
  EXPECT_EQ(line.rect.height, placed.rect.height);
  EXPECT_EQ(line.opacity, tile.opacity);
  // Once settled, the name is exactly where the placement put it.
  plan.layers = {video, name};
  animation.advance(plan, "scene:wall", true, true, 350, 5000);
  EXPECT_NEAR(plan.layers[1].rect.x, placed.rect.x, 1e-6f);
  EXPECT_NEAR(plan.layers[1].rect.y, placed.rect.y, 1e-6f);
  EXPECT_EQ(plan.layers[1].opacity, 1.f);
}

TEST(TilesAnimator, AnEnteringTilesNameLineFadesInWithIt) {
  TilesPlanAnimation animation;
  corevideo::modules::CompositorRenderPlan empty;
  animation.advance(empty, "scene:wall", true, true, 350, 0);
  corevideo::modules::CompositorRenderPlanLayer video;
  video.kind = "participant-video"; video.layerId = "tile:zoom:7"; video.rect = {0, 0, 1, 1};
  corevideo::modules::CompositorRenderPlanLayer name;
  name.kind = "overlay"; name.layerId = "tile-name:zoom:7"; name.hasOverlayContent = true;
  name.rect = {.05f, .9f, .6f, .09f};
  corevideo::modules::CompositorRenderPlan plan; plan.layers = {video, name};
  animation.advance(plan, "scene:wall", true, true, 350, 16);
  EXPECT_EQ(plan.layers[0].opacity, 0.f);
  EXPECT_EQ(plan.layers[1].opacity, 0.f);
  plan.layers = {video, name};
  animation.advance(plan, "scene:wall", true, true, 350, 191);
  EXPECT_NEAR(plan.layers[0].opacity, .5, 1e-6);
  EXPECT_EQ(plan.layers[0].opacity, plan.layers[1].opacity);
  // A name whose tile is not on the wall is left exactly as built.
  corevideo::modules::CompositorRenderPlanLayer stray = name; stray.layerId = "tile-name:zoom:8";
  plan.layers = {video, stray};
  animation.applyLatest(plan, "scene:wall");
  EXPECT_EQ(plan.layers[1].opacity, 1.f);
  EXPECT_EQ(plan.layers[1].rect.x, .05f);
}

// #1073 review: a tile GROWING into place (4-up -> 1-up) must not carry a
// full-size band past its own edges mid-flight; the band scales with the
// tile about its anchored corner and fades with the tile's alpha.
TEST(TilesAnimator, AGrowingTilesNameLineScalesWithItAndStaysInside) {
  TilesPlanAnimation animation;
  corevideo::modules::CompositorRenderPlanLayer video;
  video.kind = "participant-video"; video.layerId = "tile:zoom:9";
  // Settled in a 2x2 cell first.
  video.rect = {.5f, .5f, .5f, .5f};
  corevideo::modules::CompositorRenderPlan small; small.layers = {video};
  animation.advance(small, "scene:wall", true, true, 350, 0);
  // Now the only member: target is the full canvas.
  video.rect = {0, 0, 1, 1};
  LowerThirdAppearance style; style.enabled = true;
  const auto placed = placeTilesNameLine({0, 0, 1, 1}, .004f, .007f, style);
  ASSERT_TRUE(placed.shown);
  corevideo::modules::CompositorRenderPlanLayer name;
  name.kind = "overlay"; name.layerId = "tile-name:zoom:9"; name.hasOverlayContent = true;
  name.overlay.appearance = placed.appearance;
  name.rect = {placed.rect.x, placed.rect.y, placed.rect.width, placed.rect.height};
  corevideo::modules::CompositorRenderPlan plan; plan.layers = {video, name};
  animation.advance(plan, "scene:wall", true, true, 350, 16);
  const auto& tile = plan.layers[0];
  const auto& line = plan.layers[1];
  ASSERT_LT(tile.rect.width, 1.f);  // mid-flight, still growing
  const float scale = std::min(tile.rect.width, tile.rect.height);
  EXPECT_NEAR(line.rect.width, placed.rect.width * scale, 1e-5f);
  EXPECT_NEAR(line.rect.height, placed.rect.height * scale, 1e-5f);
  EXPECT_GE(line.rect.x, tile.rect.x - 1e-6f);
  EXPECT_LE(line.rect.x + line.rect.width, tile.rect.x + tile.rect.width + 1e-6f);
  EXPECT_GE(line.rect.y, tile.rect.y - 1e-6f);
  EXPECT_LE(line.rect.y + line.rect.height, tile.rect.y + tile.rect.height + 1e-6f);
  EXPECT_EQ(line.opacity, tile.opacity);
  // Settled: full size again.
  plan.layers = {video, name};
  animation.advance(plan, "scene:wall", true, true, 350, 5000);
  EXPECT_NEAR(plan.layers[1].rect.width, placed.rect.width, 1e-6f);
  EXPECT_NEAR(plan.layers[1].rect.x, placed.rect.x, 1e-6f);
}
