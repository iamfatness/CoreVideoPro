#include "core/TilesWallSource.h"

#include <gtest/gtest.h>

namespace {
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
}  // namespace
