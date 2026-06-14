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
  EXPECT_EQ(line->get("profile")->getString("name"), "CoreVideo Pro Native Media Core Stub");
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
