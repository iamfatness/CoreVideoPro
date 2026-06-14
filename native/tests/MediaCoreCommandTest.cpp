#include "core/MediaCore.h"
#include "modules/ZoomMeetingSdkAdapter.h"
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

TEST(ZoomMeetingSdkAdapter, FactoryIsDisabledInPortableStubBuild) {
#if COREVIDEO_WITH_ZOOM
  EXPECT_TRUE(true);
#else
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({});
  EXPECT_EQ(source, nullptr);
#endif
}

TEST(ZoomMeetingSdkAdapter, DevGateRejectsMissingJoinCredentials) {
#if COREVIDEO_WITH_ZOOM
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({
      "sdk-root",
      "7.0.5",
      "https://zoom.us",
      true,
      true,
      true,
      true,
      true,
      true,
  });
  ASSERT_NE(source, nullptr);

  const bool joined = source->join({
      "123456789",
      "CoreVideo Producer",
  });

  EXPECT_FALSE(joined);
  EXPECT_EQ(source->meetingState(), "join-ready");
  EXPECT_FALSE(source->warnings().empty());
  source->leave();
  EXPECT_EQ(source->meetingState(), "idle");
#else
  EXPECT_TRUE(true);
#endif
}

TEST(ZoomMeetingSdkAdapter, DevGateTracksDeferredRawSubscriptions) {
#if COREVIDEO_WITH_ZOOM
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({
      "sdk-root",
      "7.0.5",
      "https://zoom.us",
      true,
      true,
      true,
      true,
      true,
      true,
  });
  ASSERT_NE(source, nullptr);

  source->syncSubscriptions({
      {"12345", "participant-video", "program", 1},
      {"12345", "participant-audio", "mix", 2},
  });

  const auto states = source->subscriptionStates();
  ASSERT_TRUE(states.size() == 2);
  EXPECT_EQ(states[0].status, "failed");
  EXPECT_EQ(states[0].lastResultCode, "not-in-meeting");
  EXPECT_EQ(states[1].status, "failed");
  EXPECT_EQ(states[1].lastResultCode, "not-in-meeting");
#else
  EXPECT_TRUE(true);
#endif
}

TEST(ZoomMeetingSdkAdapter, DevGateDoesNotEmitFramesForDeferredRawSubscriptions) {
#if COREVIDEO_WITH_ZOOM
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({
      "sdk-root",
      "7.0.5",
      "https://zoom.us",
      true,
      true,
      true,
      true,
      true,
      true,
  });
  ASSERT_NE(source, nullptr);

  source->syncSubscriptions({
      {"12345", "participant-video", "program", 1},
      {"12345", "participant-audio", "mix", 2},
      {"12345", "screen-share", "program", 3},
  });

  EXPECT_TRUE(source->pollVideoFrames().empty());
  EXPECT_TRUE(source->pollAudioFrames().empty());
#else
  EXPECT_TRUE(true);
#endif
}
