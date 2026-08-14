#include "modules/ZoomEngineClient.h"
#include "modules/ZoomEngineState.h"

#include <gtest/gtest.h>

namespace {

corevideo::modules::ZoomEngineEvent eventFrom(const std::string& line) {
  auto event = corevideo::modules::parseZoomEngineEvent(line);
  EXPECT_TRUE(event.has_value());
  return event.value_or(corevideo::modules::ZoomEngineEvent{});
}

}  // namespace

TEST(ZoomEngineRuntimeState, MapsEngineEventsToSpineRosterState) {
  corevideo::modules::ZoomEngineRuntimeState state;

  state.apply(eventFrom(R"({"cmd":"ready"})"), 0);
  state.apply(eventFrom(R"({"cmd":"auth_ok"})"), 0);
  state.apply(eventFrom(R"({"cmd":"joined"})"), 0);
  state.apply(eventFrom(
      R"({"cmd":"participants","active_speaker_id":42,"participants":[{"id":42,"name":"Sophia Martinez","has_video":true,"is_talking":false,"is_muted":false,"is_sharing_screen":false},{"id":77,"name":"David Chen","has_video":true,"is_talking":false,"is_muted":true,"is_sharing_screen":true}]})"), 0);
  state.apply(eventFrom(R"({"cmd":"active_speaker","participant_id":77})"), 100);

  const auto snapshot = state.snapshot();
  EXPECT_EQ(snapshot.meetingState, "in-meeting");
  // Muted challengers cannot displace the stable incumbent.
  EXPECT_EQ(snapshot.activeSpeakerId, "42");
  EXPECT_EQ(snapshot.screenShareParticipantId, "77");
  ASSERT_TRUE(snapshot.participants.size() == 2u);
  EXPECT_EQ(snapshot.participants[0].id, 42u);
  EXPECT_EQ(snapshot.participants[1].displayName, "David Chen");
  EXPECT_TRUE(snapshot.events.size() >= 4u);

  const auto participants = state.participantsJson();
  ASSERT_TRUE(participants.size() == 2u);
  EXPECT_EQ(participants[0].getString("sdkUserId"), "42");
  EXPECT_EQ(participants[0].getString("displayName"), "Sophia Martinez");
  EXPECT_TRUE(participants[0].get("videoOn")->asBool());
  EXPECT_FALSE(participants[0].get("muted")->asBool());
  EXPECT_TRUE(participants[0].get("talking")->asBool());
  EXPECT_EQ(participants[1].getString("sdkUserId"), "77");
  EXPECT_TRUE(participants[1].get("muted")->asBool());
  EXPECT_FALSE(participants[1].get("talking")->asBool());
  EXPECT_TRUE(participants[1].get("sharingScreen")->asBool());
}

TEST(ZoomEngineRuntimeState, PreservesUtf8ParticipantIdentityAcrossIpcJson) {
  corevideo::modules::ZoomEngineRuntimeState state;
  state.apply(eventFrom(
      R"({"cmd":"participants","active_speaker_id":42,"participants":[{"id":42,"name":"Elena Kovač — 東京","has_video":true,"is_talking":true,"is_muted":false}]})"),
      0);

  const auto snapshot = state.snapshot();
  ASSERT_EQ(snapshot.participants.size(), 1u);
  EXPECT_EQ(snapshot.participants[0].displayName, "Elena Kovač — 東京");
  const auto participants = state.participantsJson();
  ASSERT_EQ(participants.size(), 1u);
  EXPECT_EQ(participants[0].getString("displayName"), "Elena Kovač — 東京");
}

TEST(ZoomEngineRuntimeState, DebouncesActiveSpeakerAndHonorsIncumbentHold) {
  corevideo::modules::ZoomEngineRuntimeState state;
  const auto roster = eventFrom(
      R"({"cmd":"participants","active_speaker_id":42,"participants":[{"id":42,"name":"Host","has_video":true,"is_talking":true,"is_muted":false},{"id":77,"name":"Guest","has_video":true,"is_talking":false,"is_muted":false}]})");
  state.apply(roster, 1'000);
  state.apply(eventFrom(R"({"cmd":"active_speaker","participant_id":77})"), 1'100);

  state.advanceActiveSpeaker(1'700);  // sensitivity met, hold not met
  EXPECT_EQ(state.snapshot().activeSpeakerId, "42");

  state.recordFrameIngestSuccess("participant-video-77-camera", 77, 1280, 720, 1, 1'800.0);
  state.advanceActiveSpeaker(3'000);  // both windows met
  EXPECT_EQ(state.snapshot().activeSpeakerId, "77");
}

TEST(ZoomEngineRuntimeState, RetainsIncumbentUntilChallengerHasAFreshFrame) {
  corevideo::modules::ZoomEngineRuntimeState state;
  const auto roster = eventFrom(
      R"({"cmd":"participants","active_speaker_id":42,"participants":[{"id":42,"name":"Host","has_video":true,"is_talking":true,"is_muted":false},{"id":77,"name":"Guest","has_video":true,"is_talking":false,"is_muted":false}]})");
  state.apply(roster, 1'000);
  state.apply(eventFrom(R"({"cmd":"active_speaker","participant_id":77})"), 1'100);

  state.advanceActiveSpeaker(4'000);
  EXPECT_EQ(state.snapshot().activeSpeakerId, "42");

  state.recordFrameIngestSuccess("participant-video-77-camera", 77, 1280, 720, 1, 4'001.0);
  state.advanceActiveSpeaker(4'002);
  EXPECT_EQ(state.snapshot().activeSpeakerId, "77");
}

TEST(ZoomEngineRuntimeState, KeepsIncumbentThroughMuteVideoOffAndBriefRosterLoss) {
  corevideo::modules::ZoomEngineRuntimeState state;
  state.apply(eventFrom(
      R"({"cmd":"participants","active_speaker_id":42,"participants":[{"id":42,"name":"Host","has_video":true,"is_talking":true,"is_muted":false}]})"), 1'000);
  state.apply(eventFrom(
      R"({"cmd":"participants","active_speaker_id":0,"participants":[{"id":42,"name":"Host","has_video":false,"is_talking":false,"is_muted":true}]})"), 2'000);
  EXPECT_EQ(state.snapshot().activeSpeakerId, "42");

  state.apply(eventFrom(R"({"cmd":"participants","active_speaker_id":0,"participants":[]})"), 3'000);
  state.advanceActiveSpeaker(62'999);
  EXPECT_EQ(state.snapshot().activeSpeakerId, "42");
}

TEST(ZoomEngineRuntimeState, TracksFrameAudioAndErrorEvidence) {
  corevideo::modules::ZoomEngineRuntimeState state;

  state.apply(eventFrom(R"({"cmd":"joined"})"));
  state.apply(eventFrom(R"({"cmd":"frame","source_uuid":"source-1","participant_id":42,"w":1280,"h":720})"));
  state.apply(eventFrom(R"({"cmd":"frame","source_uuid":"source-1","participant_id":42,"w":1280,"h":720})"));
  state.apply(eventFrom(R"({"cmd":"audio","source_uuid":"source-1","participant_id":42,"byte_len":960})"));
  state.apply(eventFrom(R"({"cmd":"error","msg":"raw_data_unavailable"})"));

  const auto snapshot = state.snapshot();
  // A mid-meeting engine error is EVIDENCE (a warning), not a state change:
  // demoting to "error" made the shell read "not in a meeting" while live
  // (owner-hit: raw_media_start_failed while the record privilege was
  // pending). Pre-join errors still move the state to "error".
  EXPECT_EQ(snapshot.meetingState, "in-meeting");
  ASSERT_TRUE(snapshot.subscriptions.size() == 2u);
  EXPECT_EQ(snapshot.subscriptions[0].participantId, "42");
  EXPECT_EQ(snapshot.subscriptions[0].kind, "participant-video");
  EXPECT_EQ(snapshot.subscriptions[0].framesReceived, 2u);
  EXPECT_EQ(snapshot.subscriptions[0].width, 1280u);
  EXPECT_EQ(snapshot.subscriptions[1].kind, "participant-audio");
  EXPECT_EQ(snapshot.subscriptions[1].audioPacketsReceived, 1u);
  ASSERT_TRUE(snapshot.warnings.size() == 1u);
  EXPECT_EQ(snapshot.warnings[0], "raw_data_unavailable");
}

TEST(ZoomEngineRuntimeState, ImmediateJoinFailureMovesSnapshotToError) {
  corevideo::modules::ZoomEngineRuntimeState state;

  state.apply(eventFrom(R"({"cmd":"auth_ok"})"));
  state.apply(eventFrom(R"({"cmd":"error","stage":"join","msg":"join_failed","code":3,"reason":"SDKERR_UNAUTHENTICATION"})"));

  const auto snapshot = state.snapshot();
  EXPECT_EQ(snapshot.meetingState, "error");
  ASSERT_TRUE(snapshot.warnings.size() == 1u);
  EXPECT_EQ(snapshot.warnings[0], "join_failed: SDKERR_UNAUTHENTICATION");
}

TEST(ZoomEngineRuntimeState, TracksFrameFreshnessAndFirstFrameTimingPerParticipant) {
  corevideo::modules::ZoomEngineRuntimeState state;

  state.apply(eventFrom(R"({"cmd":"frame","source_uuid":"source-1","participant_id":42,"w":1280,"h":720})"));
  state.recordFrameIngestSuccess("source-1", 42, 320, 180, 10, 125.0);
  state.refreshFrameFreshness(900.0, 1000.0);

  auto snapshot = state.snapshot();
  ASSERT_TRUE(snapshot.subscriptions.size() == 1u);
  EXPECT_EQ(snapshot.subscriptions[0].participantId, "42");
  EXPECT_EQ(snapshot.subscriptions[0].firstFrameAtMs, 125.0);
  EXPECT_EQ(snapshot.subscriptions[0].firstFrameDelayMs, 125.0);
  EXPECT_EQ(snapshot.subscriptions[0].lastFrameAtMs, 125.0);
  EXPECT_EQ(snapshot.subscriptions[0].lastFrameAgeMs, 775.0);
  EXPECT_EQ(snapshot.subscriptions[0].lastFrameId, 10u);
  EXPECT_TRUE(snapshot.subscriptions[0].frameFresh);
  EXPECT_TRUE(snapshot.warnings.empty());

  state.refreshFrameFreshness(1200.0, 1000.0);
  snapshot = state.snapshot();
  ASSERT_TRUE(snapshot.subscriptions.size() == 1u);
  EXPECT_FALSE(snapshot.subscriptions[0].frameFresh);
  EXPECT_EQ(snapshot.subscriptions[0].lastFrameAgeMs, 1075.0);
  ASSERT_TRUE(snapshot.warnings.size() == 1u);
  EXPECT_NE(snapshot.warnings[0].find("stale"), std::string::npos);
}

TEST(ZoomEngineRuntimeState, CountsRepeatedAndMalformedSharedMemoryFrames) {
  corevideo::modules::ZoomEngineRuntimeState state;

  state.recordFrameIngestSuccess("source-1", 42, 320, 180, 22, 100.0);
  state.recordFrameIngestSuccess("source-1", 42, 320, 180, 22, 133.0);
  state.recordFrameIngestFailure("source-1", 42, "shared memory snapshot was incomplete");

  const auto snapshot = state.snapshot();
  ASSERT_TRUE(snapshot.subscriptions.size() == 1u);
  EXPECT_EQ(snapshot.subscriptions[0].staleFrameCount, 1u);
  EXPECT_EQ(snapshot.subscriptions[0].malformedFrameCount, 1u);
  EXPECT_FALSE(snapshot.subscriptions[0].frameFresh);
  ASSERT_TRUE(snapshot.warnings.size() == 2u);
  EXPECT_NE(snapshot.warnings[0].find("repeated stale frame 22"), std::string::npos);
  EXPECT_NE(snapshot.warnings[1].find("malformed or unavailable"), std::string::npos);
}

TEST(ZoomEngineRuntimeState, ExposesCompositorVideoFramesFromSubscriptionStats) {
  corevideo::modules::ZoomEngineRuntimeState state;
  state.apply(eventFrom(R"({"cmd":"frame","source_uuid":"src-1","participant_id":42,"width":1280,"height":720})"));
  const auto frames = state.pollCompositorVideoFrames(99);
  ASSERT_TRUE(frames.size() == 1u);
  EXPECT_EQ(frames[0].participantId, "42");
  EXPECT_EQ(frames[0].width, 1280);
  EXPECT_EQ(frames[0].height, 720);
  EXPECT_EQ(frames[0].timestampMs, 99);
}

TEST(ZoomEngineRuntimeState, ClearsRosterAndSubscriptionsOnLeave) {
  corevideo::modules::ZoomEngineRuntimeState state;

  state.apply(eventFrom(
      R"({"cmd":"participants","active_speaker_id":42,"participants":[{"id":42,"name":"Sophia","has_video":true,"is_talking":true,"is_muted":false,"is_sharing_screen":false}]})"));
  state.apply(eventFrom(R"({"cmd":"frame","source_uuid":"source-1","participant_id":42,"w":1280,"h":720})"));
  state.apply(eventFrom(R"({"cmd":"left"})"));

  const auto snapshot = state.snapshot();
  EXPECT_EQ(snapshot.meetingState, "idle");
  EXPECT_TRUE(snapshot.participants.empty());
  EXPECT_TRUE(snapshot.subscriptions.empty());
  EXPECT_EQ(snapshot.events.size(), 1u);
  EXPECT_EQ(snapshot.events[0], "Zoom meeting left.");
}
