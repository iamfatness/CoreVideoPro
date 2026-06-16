#include "core/MediaCore.h"
#include "rpc/Json.h"
#include "rpc/JsonRpcServer.h"

#include <gtest/gtest.h>

#include <sstream>

TEST(JsonRpcServer, EmitsHandshakeBeforeReadingInput) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);
  std::istringstream input;
  std::ostringstream output;

  server.run(input, output);

  std::string error;
  auto line = corevideo::rpc::Json::parse(output.str(), &error);
  ASSERT_TRUE(line.has_value()) << error;
  EXPECT_EQ(line->getString("id"), "handshake");
  EXPECT_TRUE(line->get("ok")->asBool());
  ASSERT_NE(line->get("profile"), nullptr);
#if COREVIDEO_WITH_D3D11
  EXPECT_EQ(line->get("profile")->getString("name"), "CoreVideo Pro Native Media Core");
  EXPECT_EQ(line->get("profile")->getString("renderer"), "d3d11");
#else
  EXPECT_EQ(line->get("profile")->getString("name"), "CoreVideo Pro Native Media Core Stub");
#endif
}

TEST(JsonRpcServer, HandlesMediaCoreSyncRequestEnvelope) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);
  const auto response = server.handle(corevideo::rpc::Json::Object{
      {"id", "core-sync-1"},
      {"type", "media-core-sync"},
      {"elapsedMs", 66},
      {"commands",
       corevideo::rpc::Json::Array{
           corevideo::rpc::Json::Object{
               {"type", "start-program-output"},
               {"destinations", corevideo::rpc::Json::Array{"recording"}},
               {"isoParticipantIds", corevideo::rpc::Json::Array{}},
           },
       }},
  });

  EXPECT_EQ(response.getString("id"), "core-sync-1");
  EXPECT_TRUE(response.get("ok")->asBool());
  EXPECT_EQ(response.getString("type"), "media-core-sync");
  ASSERT_NE(response.get("snapshot"), nullptr);
  EXPECT_TRUE(response.get("snapshot")->get("active")->asBool());
  EXPECT_EQ(response.get("snapshot")->getString("meetingState"), "idle");
  EXPECT_EQ(response.get("snapshot")->getString("breakoutRoomId"), "main");
  EXPECT_EQ(response.get("snapshot")->getString("breakoutRoomName"), "Main room");
}

TEST(JsonRpcServer, SimulatesBreakoutRoomChangeOnSyncCommand) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);
  const auto response = server.handle(corevideo::rpc::Json::Object{
      {"id", "core-sync-room"},
      {"type", "media-core-sync"},
      {"elapsedMs", 120},
      {"commands",
       corevideo::rpc::Json::Array{
           corevideo::rpc::Json::Object{
               {"type", "simulate-breakout-room-change"},
               {"breakoutRoomId", "customer-panel"},
               {"breakoutRoomName", "Customer panel"},
           },
           corevideo::rpc::Json::Object{
               {"type", "start-program-output"},
               {"destinations", corevideo::rpc::Json::Array{"recording"}},
               {"isoParticipantIds", corevideo::rpc::Json::Array{}},
           },
       }},
  });

  ASSERT_NE(response.get("snapshot"), nullptr);
  EXPECT_EQ(response.get("snapshot")->getString("breakoutRoomId"), "customer-panel");
  EXPECT_EQ(response.get("snapshot")->getString("breakoutRoomName"), "Customer panel");
}

TEST(JsonRpcServer, PreservesIdAndAcksStartProgramOutput) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);
  const auto response = server.handle(corevideo::rpc::Json::Object{
      {"id", "agent-a-1"},
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"recording", "rtmp"}},
      {"isoParticipantIds", corevideo::rpc::Json::Array{"participant-1"}},
  });

  EXPECT_EQ(response.getString("id"), "agent-a-1");
  EXPECT_TRUE(response.get("ok")->asBool());
  ASSERT_NE(response.get("snapshot"), nullptr);
  const auto* health = response.get("snapshot")->get("health");
  ASSERT_NE(health, nullptr);
  EXPECT_EQ(health->getString("status"), "live");
  EXPECT_GE(health->get("encodedFrameCount")->asNumber(), 1);
}

TEST(JsonRpcServer, ParsesLineDelimitedJsonRequests) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);
  std::istringstream input("{\"id\":\"one\",\"type\":\"get-output-health\"}\n");
  std::ostringstream output;

  server.run(input, output);

  std::istringstream lines(output.str());
  std::string handshakeLine;
  std::string responseLine;
  ASSERT_TRUE(std::getline(lines, handshakeLine));
  ASSERT_TRUE(std::getline(lines, responseLine));
  auto response = corevideo::rpc::Json::parse(responseLine);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->getString("id"), "one");
  EXPECT_TRUE(response->get("ok")->asBool());
  EXPECT_NE(response->get("health"), nullptr);
}

TEST(JsonRpcServer, HandlesZoomMediaSpineSyncRequest) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);
  const auto response = server.handle(corevideo::rpc::Json::Object{
      {"id", "zoom-spine-1"},
      {"type", "zoom-media-spine-sync"},
      {"elapsedMs", 66},
      {"payload",
       corevideo::rpc::Json::Object{
           {"readiness",
            corevideo::rpc::Json::Object{
                {"status", "ready"},
                {"platform", "windows"},
                {"sdkVersion", "7.0.5"},
                {"checks", corevideo::rpc::Json::Array{}},
            }},
           {"participants",
            corevideo::rpc::Json::Array{
                corevideo::rpc::Json::Object{
                    {"sdkUserId", "sdk-presenter"},
                    {"displayName", "Andre Wallace"},
                    {"role", "panelist"},
                    {"videoOn", true},
                    {"muted", false},
                    {"talking", true},
                    {"sharingScreen", false},
                    {"audioLevel", 80},
                    {"networkQuality", "good"},
                },
            }},
           {"subscriptions",
            corevideo::rpc::Json::Array{
                corevideo::rpc::Json::Object{{"participantId", "sdk-presenter"}, {"kind", "participant-video"}, {"purpose", "program"}, {"priority", 1}},
                corevideo::rpc::Json::Object{{"participantId", "sdk-presenter"}, {"kind", "participant-audio"}, {"purpose", "mix"}, {"priority", 2}},
            }},
           {"recording",
            corevideo::rpc::Json::Object{
                {"targetFolder", "Recordings"},
                {"filenamePrefix", "zoom-spine"},
                {"format", "mp4"},
                {"quality", "high"},
                {"isoParticipantIds", corevideo::rpc::Json::Array{"sdk-presenter"}},
            }},
           {"blocked", false},
           {"warnings", corevideo::rpc::Json::Array{}},
           {"summary", "1 Zoom participant, 1 video and 1 audio raw Zoom subscriptions planned."},
       }},
  });

  EXPECT_EQ(response.getString("id"), "zoom-spine-1");
  EXPECT_TRUE(response.get("ok")->asBool());
  EXPECT_EQ(response.getString("type"), "zoom-media-spine-sync");
  const auto* spineSnapshot = response.get("spineSnapshot");
  ASSERT_NE(spineSnapshot, nullptr);
  EXPECT_EQ(spineSnapshot->getString("meetingState"), "in-meeting");
  EXPECT_EQ(spineSnapshot->getString("activeSpeakerId"), "sdk-presenter");
  EXPECT_EQ(spineSnapshot->get("recording")->get("evidence")->get("subscribedVideoFeeds")->asNumber(), 1);
  EXPECT_GE(spineSnapshot->get("recording")->get("evidence")->get("programFramesWritten")->asNumber(), 1);
}

TEST(JsonRpcServer, HandlesZoomLifecycleRequests) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);

  const auto joined = server.handle(corevideo::rpc::Json::Object{
      {"id", "zoom-join-1"},
      {"type", "zoom-join"},
      {"payload",
       corevideo::rpc::Json::Object{
           {"meetingUrl", "https://zoom.us/j/123456789"},
           {"displayName", "Operator"},
           {"webinar", false},
       }},
  });
  EXPECT_EQ(joined.getString("id"), "zoom-join-1");
  EXPECT_TRUE(joined.get("ok")->asBool());
  EXPECT_EQ(joined.getString("type"), "zoom-join");
  ASSERT_NE(joined.get("snapshot"), nullptr);
  EXPECT_EQ(joined.get("snapshot")->getString("meetingState"), "in_meeting");
  EXPECT_EQ(joined.get("snapshot")->getString("activeSpeakerId"), "operator-1");
  ASSERT_TRUE(joined.get("snapshot")->get("participants")->asArray().size() >= 2);
  EXPECT_EQ(joined.get("snapshot")->get("participants")->asArray()[0].getString("displayName"), "Operator");

  const auto snapshot = server.handle(corevideo::rpc::Json::Object{
      {"id", "zoom-snapshot-1"},
      {"type", "zoom-snapshot"},
  });
  EXPECT_EQ(snapshot.getString("id"), "zoom-snapshot-1");
  EXPECT_TRUE(snapshot.get("ok")->asBool());
  EXPECT_EQ(snapshot.getString("type"), "zoom-snapshot");
  EXPECT_EQ(snapshot.get("snapshot")->getString("meetingState"), "in_meeting");

  const auto left = server.handle(corevideo::rpc::Json::Object{
      {"id", "zoom-leave-1"},
      {"type", "zoom-leave"},
  });
  EXPECT_EQ(left.getString("id"), "zoom-leave-1");
  EXPECT_TRUE(left.get("ok")->asBool());
  EXPECT_EQ(left.getString("type"), "zoom-leave");
  EXPECT_EQ(left.get("snapshot")->getString("meetingState"), "idle");
  EXPECT_TRUE(left.get("snapshot")->get("participants")->asArray().empty());
}

TEST(JsonRpcServer, HandlesCaptureDeviceBridgeRequests) {
  corevideo::core::MediaCore mediaCore;
  corevideo::rpc::JsonRpcServer server(mediaCore);

  const auto listed = server.handle(corevideo::rpc::Json::Object{
      {"id", "capture-list-1"},
      {"type", "list-capture-devices"},
  });
  EXPECT_EQ(listed.getString("id"), "capture-list-1");
  EXPECT_TRUE(listed.get("ok")->asBool());
  EXPECT_EQ(listed.getString("type"), "capture-devices");
  ASSERT_NE(listed.get("devices"), nullptr);
  EXPECT_GE(listed.get("devices")->asArray().size(), 2);
  const auto deckLinkId = listed.get("devices")->asArray()[0].getString("id");
  const auto ajaId = listed.get("devices")->asArray()[1].getString("id");

  const auto selected = server.handle(corevideo::rpc::Json::Object{
      {"id", "capture-select-1"},
      {"type", "select-capture-input"},
      {"payload", corevideo::rpc::Json::Object{{"deviceId", deckLinkId}, {"inputId", "hdmi-1"}}},
  });
  EXPECT_TRUE(selected.get("ok")->asBool());
  EXPECT_EQ(selected.get("devices")->asArray()[0].getString("selectedInputId"), "hdmi-1");

  const auto offset = server.handle(corevideo::rpc::Json::Object{
      {"id", "capture-offset-1"},
      {"type", "set-capture-audio-sync-offset"},
      {"payload", corevideo::rpc::Json::Object{{"deviceId", deckLinkId}, {"offsetMs", 1200}}},
  });
  EXPECT_TRUE(offset.get("ok")->asBool());
  EXPECT_EQ(offset.get("devices")->asArray()[0].get("audioSyncOffsetMs")->asNumber(), 500);

  const auto connected = server.handle(corevideo::rpc::Json::Object{
      {"id", "capture-connect-1"},
      {"type", "connect-capture-device"},
      {"payload", corevideo::rpc::Json::Object{{"deviceId", ajaId}}},
  });
  EXPECT_TRUE(connected.get("ok")->asBool());
  EXPECT_EQ(connected.get("devices")->asArray()[1].getString("connectionState"), "connected");
}
