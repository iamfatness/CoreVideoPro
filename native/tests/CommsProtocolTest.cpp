#include <gtest/gtest.h>

#include "comms_protocol.h"

using namespace edgecomms;

TEST(CommsProtocol, KeyTakesASlotOrAll) {
  auto c = ParseCommsCommand(R"({"cmd":"comms_key","slot":3,"on":true})");
  EXPECT_EQ(c.error, std::string());
  EXPECT_TRUE(c.verb == CommsVerb::Key);
  EXPECT_EQ(c.slot, 3);
  EXPECT_TRUE(c.on);
  c = ParseCommsCommand(R"({"cmd":"comms_latch","slot":"all","on":false})");
  EXPECT_EQ(c.error, std::string());
  EXPECT_TRUE(c.verb == CommsVerb::Latch);
  EXPECT_EQ(c.slot, -1);
  EXPECT_FALSE(c.on);
}

TEST(CommsProtocol, WrongTypesAreRefusedNeverDefaulted) {
  // An "on" that is not a bool must not key anything.
  EXPECT_FALSE(ParseCommsCommand(R"({"cmd":"comms_key","slot":3,"on":1})").error.empty());
  EXPECT_FALSE(ParseCommsCommand(R"({"cmd":"comms_key","slot":3})").error.empty());
  EXPECT_FALSE(ParseCommsCommand(R"({"cmd":"comms_key","slot":16,"on":true})").error.empty());
  EXPECT_FALSE(ParseCommsCommand(R"({"cmd":"comms_key","slot":-1,"on":true})").error.empty());
  EXPECT_FALSE(ParseCommsCommand(R"({"cmd":"comms_key","slot":2.5,"on":true})").error.empty());
  EXPECT_FALSE(ParseCommsCommand(R"({"cmd":"comms_assign","slot":"all","participant_id":5,"on":true})").error.empty());
  EXPECT_FALSE(ParseCommsCommand(R"({"cmd":"comms_assign","slot":1,"participant_id":0,"on":true})").error.empty());
  EXPECT_FALSE(ParseCommsCommand(R"({"cmd":"comms_gain","db":21})").error.empty());
  EXPECT_FALSE(ParseCommsCommand(R"({"cmd":"comms_gain","db":"3"})").error.empty());
  EXPECT_FALSE(ParseCommsCommand(R"({"cmd":"comms_mic","device":3})").error.empty());
  EXPECT_FALSE(ParseCommsCommand(R"({"cmd":"comms_bogus"})").error.empty());
}

TEST(CommsProtocol, AssignGainAndDevicesParse) {
  auto c = ParseCommsCommand(R"({"cmd":"comms_assign","slot":15,"participant_id":4294967295,"on":true})");
  EXPECT_EQ(c.error, std::string());
  EXPECT_EQ(c.slot, 15);
  EXPECT_EQ(c.participant_id, 4294967295u);
  c = ParseCommsCommand(R"({"cmd":"comms_gain","db":-20})");
  EXPECT_EQ(c.error, std::string());
  EXPECT_EQ(c.db, -20);
  // Escapes and non-ASCII device names (miniaudio reports UTF-8).
  c = ParseCommsCommand(R"({"cmd":"comms_mic","device":"Mic \"A\" é\\B"})");
  EXPECT_EQ(c.error, std::string());
  EXPECT_EQ(c.device, std::string("Mic \"A\" \xC3\xA9\\B"));
  c = ParseCommsCommand(R"({"cmd":"comms_sidetone_out","device":""})");
  EXPECT_EQ(c.error, std::string());
  EXPECT_TRUE(c.verb == CommsVerb::SidetoneOut);
}

TEST(CommsProtocol, CommsLinesAreRecognisedByTheirCommandNotASubstring) {
  // The engine's own dispatch matches "quit"/"join" as substrings, so comms
  // lines are routed first by exact command name -- a device called
  // "Quit Joiner" must stay a comms_mic line.
  EXPECT_TRUE(IsCommsCommand(R"({"cmd":"comms_mic","device":"Quit Joiner"})"));
  EXPECT_FALSE(IsCommsCommand(R"({"cmd":"join","meeting_id":"comms_key"})"));
  EXPECT_FALSE(IsCommsCommand(R"({"cmd":"quit"})"));
  EXPECT_EQ(ParseCommsCommand(R"({"cmd":"comms_mic","device":"Quit Joiner"})").device, std::string("Quit Joiner"));
}

TEST(CommsProtocol, ReleaseAllAndHoldLeases) {
  EXPECT_TRUE(ParseCommsCommand(R"({"cmd":"comms_release_all"})").verb == CommsVerb::ReleaseAll);
  EXPECT_EQ(ParseCommsCommand(R"({"cmd":"comms_release_all"})").error, std::string());
  auto c = ParseCommsCommand(R"({"cmd":"comms_key","slot":2,"on":true,"lease_ms":1500})");
  EXPECT_EQ(c.error, std::string());
  EXPECT_EQ(c.lease_ms, 1500);
  EXPECT_EQ(ParseCommsCommand(R"({"cmd":"comms_key","slot":2,"on":true})").lease_ms, 0);
  EXPECT_FALSE(ParseCommsCommand(R"({"cmd":"comms_key","slot":2,"on":true,"lease_ms":-1})").error.empty());
  EXPECT_FALSE(ParseCommsCommand(R"({"cmd":"comms_key","slot":2,"on":true,"lease_ms":60001})").error.empty());
}

TEST(CommsProtocol, PrivateTakesABool) {
  auto c = ParseCommsCommand(R"({"cmd":"comms_private","on":true})");
  EXPECT_TRUE(c.verb == CommsVerb::Private);
  EXPECT_TRUE(c.on);
  EXPECT_TRUE(c.error.empty());
  EXPECT_FALSE(ParseCommsCommand(R"({"cmd":"comms_private","on":"yes"})").error.empty());
  EXPECT_FALSE(ParseCommsCommand(R"({"cmd":"comms_private"})").error.empty());
}
