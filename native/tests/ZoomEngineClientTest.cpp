#include "modules/ZoomEngineClient.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

namespace {

corevideo::rpc::Json parseCommand(const std::string& line) {
  auto parsed = corevideo::rpc::Json::parse(line);
  EXPECT_TRUE(parsed.has_value());
  return parsed.value_or(nullptr);
}

}  // namespace

TEST(ZoomEngineClient, BuildsInitJoinAndLifecycleCommands) {
  const auto init = parseCommand(corevideo::modules::buildZoomEngineInitCommand({
      "sdk-jwt",
      "public-key",
  }));
  EXPECT_EQ(init.getString("cmd"), "init");
  EXPECT_EQ(init.getString("jwt"), "sdk-jwt");
  EXPECT_EQ(init.getString("public_app_key"), "public-key");

  const auto join = parseCommand(corevideo::modules::buildZoomEngineJoinCommand({
      "123456789",
      "passcode",
      "CoreVideo Producer",
      "on-behalf",
      "zak",
      "app-privilege",
  }));
  EXPECT_EQ(join.getString("cmd"), "join");
  EXPECT_EQ(join.getString("meeting_id"), "123456789");
  EXPECT_EQ(join.getString("display_name"), "CoreVideo Producer");
  EXPECT_EQ(join.getString("passcode"), "passcode");
  EXPECT_EQ(join.getString("on_behalf_token"), "on-behalf");
  EXPECT_EQ(join.getString("user_zak"), "zak");
  EXPECT_EQ(join.getString("app_privilege_token"), "app-privilege");

  EXPECT_EQ(parseCommand(corevideo::modules::buildZoomEngineLeaveCommand()).getString("cmd"), "leave");
  EXPECT_EQ(parseCommand(corevideo::modules::buildZoomEngineStartMediaCommand()).getString("cmd"), "start_media");
  EXPECT_EQ(parseCommand(corevideo::modules::buildZoomEngineStopMediaCommand()).getString("cmd"), "stop_media");
  EXPECT_EQ(parseCommand(corevideo::modules::buildZoomEngineQuitCommand()).getString("cmd"), "quit");
}

TEST(ZoomEngineClient, BuildsVideoAudioAndUnsubscribeCommands) {
  const corevideo::modules::ZoomEngineSubscribeCommand request{
      "source-123",
      42,
      2,
      "screenshare",
      true,
      false,
  };

  const auto subscribe = parseCommand(corevideo::modules::buildZoomEngineSubscribeCommand(request));
  EXPECT_EQ(subscribe.getString("cmd"), "subscribe");
  EXPECT_EQ(subscribe.getString("source_uuid"), "source-123");
  EXPECT_EQ(subscribe.get("participant_id")->asNumber(), 42);
  EXPECT_EQ(subscribe.get("resolution")->asNumber(), 2);
  EXPECT_EQ(subscribe.getString("mode"), "screenshare");
  EXPECT_TRUE(subscribe.get("isolate_audio")->asBool());

  const auto audio = parseCommand(corevideo::modules::buildZoomEngineSubscribeAudioCommand(request));
  EXPECT_EQ(audio.getString("cmd"), "subscribe_audio");
  EXPECT_EQ(audio.getString("source_uuid"), "source-123");
  EXPECT_EQ(audio.get("participant_id")->asNumber(), 42);
  EXPECT_TRUE(audio.get("isolate_audio")->asBool());

  const auto unsubscribe = parseCommand(corevideo::modules::buildZoomEngineUnsubscribeCommand("source-123"));
  EXPECT_EQ(unsubscribe.getString("cmd"), "unsubscribe");
  EXPECT_EQ(unsubscribe.getString("source_uuid"), "source-123");
}

TEST(ZoomEngineClient, ParsesFrameAudioParticipantAndSpeakerEvents) {
  const auto frame = corevideo::modules::parseZoomEngineEvent(
      R"({"cmd":"frame","source_uuid":"source-123","participant_id":42,"w":1280,"h":720})");
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->kind, corevideo::modules::ZoomEngineEventKind::Frame);
  EXPECT_TRUE(frame->ok);
  EXPECT_EQ(frame->sourceUuid, "source-123");
  EXPECT_EQ(frame->participantId, 42u);
  EXPECT_EQ(frame->width, 1280u);
  EXPECT_EQ(frame->height, 720u);

  const auto audio = corevideo::modules::parseZoomEngineEvent(
      R"({"cmd":"audio","source_uuid":"source-123","participant_id":42,"byte_len":960})");
  ASSERT_TRUE(audio.has_value());
  EXPECT_EQ(audio->kind, corevideo::modules::ZoomEngineEventKind::Audio);
  EXPECT_EQ(audio->byteLength, 960u);

  const auto participants = corevideo::modules::parseZoomEngineEvent(
      R"({"cmd":"participants","active_speaker_id":42,"participants":[{"id":42},{"id":77}]})");
  ASSERT_TRUE(participants.has_value());
  EXPECT_EQ(participants->kind, corevideo::modules::ZoomEngineEventKind::Participants);
  EXPECT_EQ(participants->activeSpeakerId, 42u);
  EXPECT_EQ(participants->participantCount, 2u);

  const auto speaker = corevideo::modules::parseZoomEngineEvent(R"({"cmd":"active_speaker","participant_id":77})");
  ASSERT_TRUE(speaker.has_value());
  EXPECT_EQ(speaker->kind, corevideo::modules::ZoomEngineEventKind::ActiveSpeaker);
  EXPECT_EQ(speaker->participantId, 77u);
}

TEST(ZoomEngineClient, ParsesFailureAndDebugEvents) {
  const auto authFail = corevideo::modules::parseZoomEngineEvent(R"({"cmd":"auth_fail","stage":"sdk_auth","code":7})");
  ASSERT_TRUE(authFail.has_value());
  EXPECT_EQ(authFail->kind, corevideo::modules::ZoomEngineEventKind::AuthFail);
  EXPECT_FALSE(authFail->ok);
  EXPECT_EQ(authFail->stage, "sdk_auth");

  const auto debug = corevideo::modules::parseZoomEngineEvent(R"({"cmd":"debug","stage":"video_subscribe","source_uuid":"a"})");
  ASSERT_TRUE(debug.has_value());
  EXPECT_EQ(debug->kind, corevideo::modules::ZoomEngineEventKind::Debug);
  EXPECT_TRUE(debug->ok);
  EXPECT_EQ(debug->stage, "video_subscribe");

  EXPECT_FALSE(corevideo::modules::parseZoomEngineEvent("not-json").has_value());
}

TEST(ZoomEngineClient, MirrorsSharedMemoryNamesAndSizes) {
  EXPECT_EQ(corevideo::modules::zoomEngineVideoSharedMemoryName("source-123"), "ZoomObsPlugin_source-123");
  EXPECT_EQ(corevideo::modules::zoomEngineAudioSharedMemoryName("source-123"), "ZoomObsPlugin_source-123_audio");
  EXPECT_EQ(corevideo::modules::zoomEngineI420FrameByteSize(1280, 720), static_cast<std::size_t>(16 + 1280 * 720 + 1280 * 720 / 2));
  EXPECT_EQ(corevideo::modules::zoomEnginePcmAudioByteSize(960), static_cast<std::size_t>(16 + 960));
}
