#include "core/MediaCore.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <string>

// T1 (docs/2026-08-15-corevideo-tiles-T1-core-wall): the core parses a
// `tiles` layer off the scene-sync command into state. Nothing renders yet
// -- these tests only pin the parse + the reset-on-omit behavior.
//
// NOTE on the source of these commands: the plan brief for this task wrote
// its example commands as raw JSON text fed through Json::parse() + a
// mutating .set("type", ...) call. Neither exists on rpc::Json (parse()
// returns std::optional<Json> and there is no setter) -- the codebase's
// actual pattern, used throughout MediaCoreCommandTest.cpp, is to build the
// command directly as a Json::Object/Json::Array literal. That is what this
// file does; the field names/values match the brief's JSON exactly.

namespace {

using corevideo::core::MediaCore;
using corevideo::rpc::Json;

// loadSceneGraph is PRIVATE (MediaCore.h) -- drive it the way
// MediaCoreCommandTest already does: applyCommands with a load-scene-graph
// command, which also exercises the real dispatch path rather than a back
// door.
void loadScene(MediaCore& core, Json command) {
  (void)core.applyCommands(Json::Array{std::move(command)});
}

// A minimal load-scene-graph command carrying one tiles layer.
Json sceneWithTilesCommand() {
  return Json::Object{
      {"type", "load-scene-graph"},
      {"sceneId", "scene-1"},
      {"routes", Json::Array{}},
      {"tiles",
       Json::Object{
           {"layerId", "tiles:scene-1"},
           {"order", 0},
           {"rect", Json::Object{{"x", 0.0}, {"y", 0.0}, {"w", 1.0}, {"h", 1.0}}},
           {"members", Json::Array{"zoom:101", "zoom:102", "capture:cam-a"}},
           {"style",
            Json::Object{
                {"tileAspect", "4:3"},
                {"gutterPercent", 1.5},
                {"marginPercent", 2.0},
                {"backgroundColor", "#101418"},
            }},
       }},
  };
}

}  // namespace

TEST(TilesLayer, LoadSceneGraphParsesMembersAndStyleInOrder) {
  MediaCore core;
  loadScene(core, sceneWithTilesCommand());

  const auto& tiles = core.tilesLayerForTest();
  ASSERT_TRUE(tiles.present);
  EXPECT_EQ(tiles.layerId, "tiles:scene-1");
  ASSERT_EQ(tiles.members.size(), 3u);
  EXPECT_EQ(tiles.members[0], "zoom:101");
  EXPECT_EQ(tiles.members[2], "capture:cam-a");
  EXPECT_EQ(tiles.style.tileAspect, "4:3");
  // No EXPECT_DOUBLE_EQ in this repo's vendored gtest (see AudioMasteringTest.cpp).
  EXPECT_NEAR(tiles.style.gutterPercent, 1.5, 1e-9);
  EXPECT_EQ(tiles.style.backgroundColor, "#101418");
}

TEST(TilesLayer, AbsentTilesNodeLeavesTheLayerUnset) {
  MediaCore core;
  loadScene(core, Json::Object{{"type", "load-scene-graph"}, {"sceneId", "s"}, {"routes", Json::Array{}}});
  EXPECT_FALSE(core.tilesLayerForTest().present);
}

// A scene that previously had a wall must not keep it when the next sync
// omits it -- stale walls are the respawn/one-shot failure class in
// miniature.
TEST(TilesLayer, ReloadWithoutTilesClearsThePreviousWall) {
  MediaCore core;
  loadScene(core, sceneWithTilesCommand());
  ASSERT_TRUE(core.tilesLayerForTest().present);
  loadScene(core, Json::Object{{"type", "load-scene-graph"}, {"sceneId", "s2"}, {"routes", Json::Array{}}});
  EXPECT_FALSE(core.tilesLayerForTest().present);
}

// An unknown aspect token is a legal value we did not expect; fall back
// rather than guess, and say so.
TEST(TilesLayer, UnknownAspectFallsBackTo16x9AndWarns) {
  MediaCore core;
  loadScene(core, Json::Object{
                       {"type", "load-scene-graph"},
                       {"sceneId", "s"},
                       {"routes", Json::Array{}},
                       {"tiles", Json::Object{
                                     {"members", Json::Array{"zoom:1"}},
                                     {"style", Json::Object{{"tileAspect", "banana"}}},
                                 }},
                   });
  EXPECT_EQ(core.tilesLayerForTest().style.tileAspect, "16:9");
  EXPECT_FALSE(core.sceneValidationWarningsForTest().empty());
}
