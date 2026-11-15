#include "engine-command.h"

#include <gtest/gtest.h>

TEST(EngineCommandParse, EachRealCommandMapsToItself) {
  EXPECT_EQ(engine_parse_command(R"({"cmd":"init","sdk_key":"k"})"), EngineCommand::Init);
  EXPECT_EQ(engine_parse_command(R"({"cmd":"join","meeting":"1"})"), EngineCommand::Join);
  EXPECT_EQ(engine_parse_command(R"({"cmd":"leave"})"), EngineCommand::Leave);
  EXPECT_EQ(engine_parse_command(R"({"cmd":"start_media"})"), EngineCommand::StartMedia);
  EXPECT_EQ(engine_parse_command(R"({"cmd":"stop_media"})"), EngineCommand::StopMedia);
  EXPECT_EQ(engine_parse_command(R"({"cmd":"subscribe_audio","source_uuid":"u"})"), EngineCommand::SubscribeAudio);
  EXPECT_EQ(engine_parse_command(R"({"cmd":"unsubscribe","source_uuid":"u"})"), EngineCommand::Unsubscribe);
  EXPECT_EQ(engine_parse_command(R"({"cmd":"subscribe","source_uuid":"u"})"), EngineCommand::Subscribe);
  EXPECT_EQ(engine_parse_command(R"({"cmd":"quit"})"), EngineCommand::Quit);
  EXPECT_EQ(engine_parse_command(R"({ "cmd" : "join" })"), EngineCommand::Join);
}

TEST(EngineCommandParse, PayloadWordsNeverChangeTheCommand) {
  EXPECT_EQ(engine_parse_command(R"({"cmd":"subscribe","source_uuid":"join"})"), EngineCommand::Subscribe);
  EXPECT_EQ(engine_parse_command(R"({"cmd":"subscribe","source_uuid":"init"})"), EngineCommand::Subscribe);
  EXPECT_EQ(engine_parse_command(R"({"cmd":"subscribe","source_uuid":"leave"})"), EngineCommand::Subscribe);
  EXPECT_EQ(engine_parse_command(R"({"cmd":"subscribe","source_uuid":"quit"})"), EngineCommand::Subscribe);
  EXPECT_EQ(engine_parse_command(R"({"cmd":"subscribe","source_uuid":"start_media stop_media"})"), EngineCommand::Subscribe);
  EXPECT_EQ(engine_parse_command(R"({"source_uuid":"join","cmd":"subscribe"})"), EngineCommand::Subscribe);
}

TEST(EngineCommandParse, UnknownOrMissingCmdIsUnknown) {
  EXPECT_EQ(engine_parse_command(R"({"cmd":"frobnicate"})"), EngineCommand::Unknown);
  EXPECT_EQ(engine_parse_command(R"({"cmd":"joined"})"), EngineCommand::Unknown);
  EXPECT_EQ(engine_parse_command(R"({"detail":"join","note":"quit"})"), EngineCommand::Unknown);
  EXPECT_EQ(engine_parse_command("not json at all init join"), EngineCommand::Unknown);
  EXPECT_EQ(engine_parse_command(""), EngineCommand::Unknown);
}

TEST(EngineCommandName, ExtractsCmdValueForLogging) {
  EXPECT_EQ(engine_command_name(R"({"cmd":"frobnicate","x":1})"), "frobnicate");
  EXPECT_EQ(engine_command_name(R"({ "cmd" : "a_b" })"), "a_b");
  EXPECT_EQ(engine_command_name(R"({"x":1})"), "");
  EXPECT_EQ(engine_command_name(R"({"cmd":"unterminated)"), "");
}
