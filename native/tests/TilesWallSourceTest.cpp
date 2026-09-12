#include "core/TilesWallSource.h"

#include <gtest/gtest.h>

namespace {
using corevideo::core::TilesWallSource;
using corevideo::core::TilesWallSources;

// One wall id yields ONE source however many buses ask for it. This is the
// whole point of the slice: the animation belongs to the wall, not the bus.
TEST(TilesWallSources, BothBusesAskingForOneWallGetTheSameSource) {
  TilesWallSources sources;
  auto& fromProgram = sources.forWall("tiles:scene-a");
  auto& fromPreview = sources.forWall("tiles:scene-a");
  EXPECT_EQ(&fromProgram, &fromPreview);
  EXPECT_EQ(sources.size(), 1U);
}

TEST(TilesWallSources, DifferentWallsAreDifferentSources) {
  TilesWallSources sources;
  auto& a = sources.forWall("tiles:scene-a");
  auto& b = sources.forWall("tiles:scene-b");
  EXPECT_NE(&a, &b);
  EXPECT_EQ(sources.size(), 2U);
}

// Lifetime is "referenced by a scene", per the parent spec. A wall no scene
// references is released; a wall still referenced survives the sweep.
TEST(TilesWallSources, AWallNoSceneReferencesIsReleased) {
  TilesWallSources sources;
  sources.forWall("tiles:scene-a");
  sources.forWall("tiles:scene-b");

  sources.releaseAllExcept({"tiles:scene-b"});

  EXPECT_EQ(sources.size(), 1U);
  EXPECT_EQ(&sources.forWall("tiles:scene-b"), &sources.forWall("tiles:scene-b"));
}

// A generation that never moves proves nothing. It must move on a reset...
TEST(TilesWallSources, AResetBumpsTheGeneration) {
  TilesWallSources sources;
  auto& wall = sources.forWall("tiles:scene-a");
  const auto before = wall.generation();
  wall.noteReset();
  EXPECT_GT(wall.generation(), before);
}

// ...and a released-then-recreated wall is a NEW wall, so its generation
// must not silently continue the old one's.
TEST(TilesWallSources, ARecreatedWallDoesNotInheritTheOldGeneration) {
  TilesWallSources sources;
  sources.forWall("tiles:scene-a").noteReset();
  const auto retired = sources.forWall("tiles:scene-a").generation();
  sources.releaseAllExcept({});
  EXPECT_EQ(sources.forWall("tiles:scene-a").generation(), 0U);
  EXPECT_NE(sources.forWall("tiles:scene-a").generation(), retired);
}

// Review round 1, finding 1: the five tests above never call
// TilesWallSource::advance(...) - they only prove ++generation_ works, not the
// glue that decides WHEN to call it:
//   if (animation_.advance(...)) { noteReset(); }
// This pins that glue in BOTH directions, driving the real advance() the way
// MediaCore will (one animation object per wall, sampled across two "bus"
// calls with different wall keys - the whole point of the slice).
//
// A different wall key arriving is TilesPlanAnimation::advance's reset path
// (`if (key_ != wallKey) { animator_.reset(); ... }`), and the FIRST call on a
// fresh source is also a "key changed" transition (from the empty initial
// key), so it too counts as a reset - a cold start is not continuity.
TEST(TilesWallSource, AdvanceWithADifferentWallKeyMovesTheGenerationByExactlyOne) {
  TilesWallSource wall;
  corevideo::modules::CompositorRenderPlan plan;
  wall.advance(plan, "tiles:scene-a", /*present=*/true, /*enabled=*/true, 350, 0);
  const auto before = wall.generation();

  corevideo::modules::CompositorRenderPlan otherPlan;
  wall.advance(otherPlan, "tiles:scene-b", /*present=*/true, /*enabled=*/true, 350, 16);

  EXPECT_EQ(wall.generation(), before + 1);
}

TEST(TilesWallSource, AdvanceWithTheSameWallKeyDoesNotMoveTheGeneration) {
  TilesWallSource wall;
  corevideo::modules::CompositorRenderPlan plan;
  wall.advance(plan, "tiles:scene-a", /*present=*/true, /*enabled=*/true, 350, 0);
  const auto before = wall.generation();

  corevideo::modules::CompositorRenderPlan samePlan;
  wall.advance(samePlan, "tiles:scene-a", /*present=*/true, /*enabled=*/true, 350, 16);

  EXPECT_EQ(wall.generation(), before);
}
}  // namespace
