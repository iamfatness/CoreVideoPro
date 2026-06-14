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
  ASSERT_NE(response.get("state"), nullptr);
  const auto* health = response.get("state")->get("health");
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
  const auto* zoom = response.get("zoom");
  ASSERT_NE(zoom, nullptr);
  EXPECT_EQ(zoom->getString("meetingState"), "in-meeting");
  EXPECT_EQ(zoom->getString("activeSpeakerId"), "sdk-presenter");
  EXPECT_EQ(zoom->get("recording")->get("evidence")->get("subscribedVideoFeeds")->asNumber(), 1);
  EXPECT_GE(zoom->get("recording")->get("evidence")->get("programFramesWritten")->asNumber(), 1);
}
