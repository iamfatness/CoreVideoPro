#include <gtest/gtest.h>

#include "modules/ZoomCommsRelay.h"

using namespace corevideo::modules;
using corevideo::rpc::Json;

namespace {

Json parse(const std::string& s) {
  auto j = Json::parse(s);
  return j ? *j : Json();
}

// A node-shape comms object: available, Ana eligible, Wes on the web client.
Json availableComms() {
  return *commsStateForNode(parse(
      R"({"cmd":"comms_state","available":true,"reason":null,)"
      R"("roster":[{"participant_id":201,"name":"Ana","talkback":true,"room":null,"channels":[0]},)"
      R"({"participant_id":203,"name":"Wes","talkback":false,"room":null,"channels":[]}]})"));
}

}  // namespace

TEST(CommsRelay, StateIsRekeyedToCamelCaseAtEveryDepth) {
  const auto node = commsStateForNode(parse(
      R"({"cmd":"comms_state","available":false,"reason":"Not in a meeting.",)"
      R"("lamps":{"link":false,"meeting_mic":false,"channel":false,"tx":false},"room":null,)"
      R"("channels":[{"slot":0,"label":null,"listeners":0,"ready":false,"keyed":false,"latched":false,"room":null,"reach_ok":0,"reach_dark":0}],)"
      R"("roster":[{"participant_id":7,"name":"Ana_B","talkback":true,"room":null,"channels":[0,3]}],)"
      R"("mic":{"device":"Mic","devices":["Mic"],"peak_dbfs":null,"gain_db":0,"sidetone":true,"sidetone_device":"","sidetone_devices":[],"tone":false},)"
      R"("stats":{"frames_sent":1,"channel_sends":2,"underruns":3,"send_errors":4},"log":["x_y"]})"));
  ASSERT_TRUE(node.has_value());
  EXPECT_TRUE(node->get("cmd") == nullptr);
  EXPECT_TRUE(node->get("lamps")->get("meetingMic") != nullptr);
  EXPECT_TRUE(node->get("lamps")->get("meeting_mic") == nullptr);
  const auto& ch = node->get("channels")->asArray()[0];
  EXPECT_TRUE(ch.get("reachOk") != nullptr);
  EXPECT_TRUE(ch.get("reachDark") != nullptr);
  EXPECT_TRUE(ch.get("label")->isNull());
  const auto& p = node->get("roster")->asArray()[0];
  EXPECT_EQ(p.getNumber("participantId"), 7.0);
  EXPECT_EQ(p.getString("name"), std::string("Ana_B"));  // values are never touched
  EXPECT_EQ(p.get("channels")->asArray().size(), size_t{2});
  EXPECT_TRUE(node->get("mic")->get("peakDbfs")->isNull());
  EXPECT_TRUE(node->get("mic")->get("sidetoneDevices") != nullptr);
  EXPECT_EQ(node->get("stats")->getNumber("sendErrors"), 4.0);
  EXPECT_EQ(node->get("stats")->getNumber("framesSent"), 1.0);
  EXPECT_EQ(node->get("log")->asArray()[0].asString(), std::string("x_y"));
  EXPECT_EQ(node->getString("reason"), std::string("Not in a meeting."));
}

TEST(CommsRelay, VerbsBecomeEngineLines) {
  const Json comms = availableComms();
  auto r = buildCommsEngineLine("comms-key", parse(R"({"cmd":"comms-key","slot":3,"on":true})"), true, &comms);
  EXPECT_EQ(r.error, std::string());
  EXPECT_EQ(r.engineLine, std::string(R"({"cmd":"comms_key","on":true,"slot":3})"));
  r = buildCommsEngineLine("comms-latch", parse(R"({"all":true,"on":false})"), true, &comms);
  EXPECT_EQ(r.engineLine, std::string(R"({"cmd":"comms_latch","on":false,"slot":"all"})"));
  r = buildCommsEngineLine("comms-assign", parse(R"({"slot":2,"participantId":201,"on":true})"), true, &comms);
  EXPECT_EQ(r.engineLine, std::string(R"({"cmd":"comms_assign","on":true,"participant_id":201,"slot":2})"));
  r = buildCommsEngineLine("comms-sidetone-out", parse(R"x({"device":"Headphones (2- USB)"})x"), true, &comms);
  EXPECT_EQ(r.engineLine, std::string(R"x({"cmd":"comms_sidetone_out","device":"Headphones (2- USB)"})x"));
  r = buildCommsEngineLine("comms-gain", parse(R"({"db":-6})"), true, &comms);
  EXPECT_EQ(r.engineLine, std::string(R"({"cmd":"comms_gain","db":-6})"));
  r = buildCommsEngineLine("comms-tone", parse(R"({"on":true})"), true, &comms);
  EXPECT_EQ(r.engineLine, std::string(R"({"cmd":"comms_tone","on":true})"));
}

TEST(CommsRelay, BadArgumentsAreRefusedWithText) {
  const Json comms = availableComms();
  EXPECT_FALSE(buildCommsEngineLine("comms-key", parse(R"({"slot":16,"on":true})"), true, &comms).error.empty());
  EXPECT_FALSE(buildCommsEngineLine("comms-key", parse(R"({"slot":1})"), true, &comms).error.empty());
  EXPECT_FALSE(buildCommsEngineLine("comms-key", parse(R"({"slot":1.5,"on":true})"), true, &comms).error.empty());
  EXPECT_FALSE(buildCommsEngineLine("comms-key", parse(R"({"all":true,"slot":1,"on":true})"), true, &comms).error.empty());
  EXPECT_FALSE(buildCommsEngineLine("comms-gain", parse(R"({"db":21})"), true, &comms).error.empty());
  EXPECT_FALSE(buildCommsEngineLine("comms-gain", parse(R"({"db":"3"})"), true, &comms).error.empty());
  EXPECT_FALSE(buildCommsEngineLine("comms-mic", parse(R"({})"), true, &comms).error.empty());
  EXPECT_FALSE(buildCommsEngineLine("comms-assign", parse(R"({"slot":2,"on":true})"), true, &comms).error.empty());
}

TEST(CommsRelay, RefusesOutsideAMeetingAndWhileUnavailableButAlwaysReleases) {
  const Json comms = availableComms();
  EXPECT_EQ(buildCommsEngineLine("comms-key", parse(R"({"slot":0,"on":true})"), false, &comms).error,
            std::string("Join a meeting first."));
  EXPECT_EQ(buildCommsEngineLine("comms-gain", parse(R"({"db":0})"), false, nullptr).error,
            std::string("Join a meeting first."));
  const Json down = *commsStateForNode(
      parse(R"({"cmd":"comms_state","available":false,"reason":"Make this account host or co-host to bring talkback up."})"));
  EXPECT_EQ(buildCommsEngineLine("comms-key", parse(R"({"slot":0,"on":true})"), true, &down).error,
            std::string("Comms is not available: Make this account host or co-host to bring talkback up."));
  // Releasing a key is never refused, whatever state the node is in.
  EXPECT_EQ(buildCommsEngineLine("comms-key", parse(R"({"slot":0,"on":false})"), false, nullptr).error, std::string());
  EXPECT_EQ(buildCommsEngineLine("comms-latch", parse(R"({"all":true,"on":false})"), true, &down).error, std::string());
}

TEST(CommsRelay, AssigningAWebClientParticipantSaysWhy) {
  const Json comms = availableComms();
  const auto r = buildCommsEngineLine("comms-assign", parse(R"({"slot":4,"participantId":203,"on":true})"), true, &comms);
  EXPECT_TRUE(r.error.find("Wes is on the Zoom web client") != std::string::npos);
  EXPECT_TRUE(r.engineLine.empty());
  EXPECT_EQ(buildCommsEngineLine("comms-assign", parse(R"({"slot":4,"participantId":999,"on":true})"), true, &comms).error,
            std::string("That participant is not in the meeting."));
  // Unassigning is not gated on the roster.
  EXPECT_EQ(buildCommsEngineLine("comms-assign", parse(R"({"slot":4,"participantId":999,"on":false})"), true, &comms).error,
            std::string());
}

TEST(CommsRelay, HoldLeaseAndReleaseAll) {
  const Json comms = availableComms();
  auto r = buildCommsEngineLine("comms-key", parse(R"({"slot":1,"on":true,"leaseMs":1500})"), true, &comms);
  EXPECT_EQ(r.engineLine, std::string(R"({"cmd":"comms_key","lease_ms":1500,"on":true,"slot":1})"));
  r = buildCommsEngineLine("comms-key", parse(R"({"slot":1,"on":true,"leaseMs":-5})"), true, &comms);
  EXPECT_FALSE(r.error.empty());
  r = buildCommsEngineLine("comms-latch", parse(R"({"slot":1,"on":true,"leaseMs":1500})"), true, &comms);
  EXPECT_FALSE(r.error.empty());  // a latch is deliberate: no lease
  r = buildCommsEngineLine("comms-release-all", parse(R"({})"), false, nullptr);
  EXPECT_EQ(r.error, std::string());  // never refused
  EXPECT_EQ(r.engineLine, std::string(R"({"cmd":"comms_release_all"})"));
}
