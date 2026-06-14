#include "core/MediaCore.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

TEST(MediaCoreCommand, AppliesSceneGraphTransformsOverlaysAndOutput) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "interview"},
          {"routes", corevideo::rpc::Json::Array{
                         corevideo::rpc::Json::Object{{"routeId", "a"}, {"mode", "fixed"}, {"audioRole", "mix"}},
                         corevideo::rpc::Json::Object{{"routeId", "b"}, {"mode", "active-speaker"}, {"audioRole", "mix"}},
                     }},
      },
      corevideo::rpc::Json::Object{
          {"type", "set-participant-transform"},
          {"participantId", "participant-1"},
          {"crop", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}},
          {"scale", 1},
      },
      corevideo::rpc::Json::Object{
          {"type", "set-overlay-asset"},
          {"overlayId", "lower-third"},
          {"text", "Speaker"},
          {"position", "lower-third"},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-program-output"},
          {"destinations", corevideo::rpc::Json::Array{"recording"}},
          {"isoParticipantIds", corevideo::rpc::Json::Array{"participant-1"}},
      },
  });

  EXPECT_EQ(state.getString("sceneId"), "interview");
  EXPECT_EQ(state.get("routeCount")->asNumber(), 2);
  EXPECT_EQ(state.get("transformCount")->asNumber(), 1);
  EXPECT_EQ(state.get("overlayCount")->asNumber(), 1);
  EXPECT_TRUE(state.get("active")->asBool());
  EXPECT_EQ(state.get("health")->getString("status"), "live");
}

TEST(MediaCoreCommand, ProfileMirrorsNativeMediaCoreShape) {
  corevideo::core::MediaCore mediaCore;
  const auto profile = mediaCore.profile();

  EXPECT_EQ(profile.getString("name"), "CoreVideo Pro Native Media Core Stub");
  EXPECT_EQ(profile.getString("renderer"), "software");
  EXPECT_EQ(profile.getString("maxProgramResolution"), "1920x1080");
  EXPECT_EQ(profile.get("maxProgramFps")->asNumber(), 30);
  EXPECT_GE(profile.get("maxParticipantFeeds")->asNumber(), 6);
  EXPECT_GE(profile.get("maxIsoRecordings")->asNumber(), 2);
  ASSERT_NE(profile.get("capabilities"), nullptr);
  EXPECT_GE(profile.get("capabilities")->asArray().size(), 11);
}
