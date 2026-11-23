// The macOS engine's comms decisions that need no Zoom SDK
// (zoom-engine/engine/engine-comms-policy.h). The integer literals below are
// FIXTURE constants copied from the SDK headers, never from the
// implementation (plan-authoring rule 2):
//   ZoomSDKMeetingStatus  ZoomSDK.framework/Headers/ZoomSDKErrors.h:454-482
//   ZoomSDKAudioStatus    ZoomSDKErrors.h:524-536
//   UserRole              ZoomSDKErrors.h:750-760
//   AVAuthorizationStatus AVFoundation AVCaptureDevice.h (NotDetermined=0,
//                         Restricted=1, Denied=2, Authorized=3)
#include <gtest/gtest.h>

#include "engine-comms-policy.h"

TEST(EngineCommsPolicy, StatusTableMatchesTheWindowsRuleOnTheMacEnum) {
  using engine_comms::StatusAction;
  using engine_comms::classifyMeetingStatus;
  EXPECT_EQ(classifyMeetingStatus(3), StatusAction::ResetOwnership);  // InMeeting
  EXPECT_EQ(classifyMeetingStatus(8), StatusAction::Ignore);          // AudioReady
  for (int s : {0, 1, 2, 4, 5, 6, 7, 9, 10, 12, 13, 14, 15, 11, 99, -1})
    EXPECT_EQ(classifyMeetingStatus(s), StatusAction::ResetAndEndMeeting) << s;
}

TEST(EngineCommsPolicy, NotJoinedAudioIsMutedAndClosed) {
  EXPECT_TRUE(engine_comms::selfMuted(0));  // None: audio not joined
  EXPECT_TRUE(engine_comms::selfMuted(1));  // Muted
  EXPECT_TRUE(engine_comms::selfMuted(3));  // MutedByHost
  EXPECT_TRUE(engine_comms::selfMuted(5));  // MutedAllByHost
  EXPECT_FALSE(engine_comms::selfMuted(2));  // UnMuted
  EXPECT_FALSE(engine_comms::selfMuted(4));  // UnMutedByHost
  EXPECT_FALSE(engine_comms::selfMuted(6));  // UnMutedAllByHost
  EXPECT_FALSE(engine_comms::meetingMicOpen(/*None*/ 0, /*UnMuted*/ 2));
  EXPECT_TRUE(engine_comms::meetingMicOpen(/*Voip*/ 1, 2));
  EXPECT_FALSE(engine_comms::meetingMicOpen(1, 1));
}

TEST(EngineCommsPolicy, RolesAndLicence) {
  EXPECT_FALSE(engine_comms::hostOrCohost(0));
  EXPECT_TRUE(engine_comms::hostOrCohost(1));
  EXPECT_TRUE(engine_comms::hostOrCohost(2));
  EXPECT_FALSE(engine_comms::hostOrCohost(3));
  EXPECT_TRUE(engine_comms::licensed(0));   // ZoomSDKError_Success
  EXPECT_FALSE(engine_comms::licensed(1));
  EXPECT_FALSE(engine_comms::licensed(12));
}

TEST(EngineCommsPolicy, MicAccessErrorNamesTheSetting) {
  EXPECT_EQ(engine_comms::micAccessError(3), "");  // Authorized
  EXPECT_EQ(engine_comms::micAccessError(2),       // Denied
            "Microphone access is off for CoreVideo Pro. Turn it on in System "
            "Settings > Privacy & Security > Microphone.");
  EXPECT_EQ(engine_comms::micAccessError(1), engine_comms::micAccessError(2));  // Restricted
  EXPECT_EQ(engine_comms::micAccessError(0), "Waiting for microphone permission.");  // NotDetermined
}

// comms_audio_join answers the plan's audio-join unknown (owner decision N3):
// is meeting audio joined at all after a Mac join? Written on every InMeeting
// transition and every change of either value, never per tick.
TEST(EngineCommsPolicy, AudioJoinBreadcrumbOnlyOnChange) {
  EXPECT_EQ(engine_comms::audioJoinBreadcrumb(1, 2, 1, 2, false), std::nullopt);
  EXPECT_EQ(engine_comms::audioJoinBreadcrumb(1, 2, 1, 2, true),
            std::optional<std::string>(
                R"({"cmd":"debug","stage":"comms_audio_join","audioType":1,"audioStatus":2})"));
  EXPECT_EQ(engine_comms::audioJoinBreadcrumb(0, 0, 1, 1, false),
            std::optional<std::string>(
                R"({"cmd":"debug","stage":"comms_audio_join","audioType":1,"audioStatus":1})"));
  EXPECT_NE(engine_comms::audioJoinBreadcrumb(1, 2, 1, 1, false), std::nullopt);  // status alone
}
