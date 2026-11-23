// The macOS engine's comms decisions that do not need the Zoom SDK.
//
// engine-comms-macos.mm is the twin of engine-comms.cpp (Windows). Everything
// it decides from an SDK value is decided HERE, from plain ints, so the
// decisions are unit-tested on every platform's CI (EngineCommsPolicyTest)
// even though the glue itself only builds against ZoomSDK.framework.
//
// The enum values are copied as ints from the macOS SDK headers so this file
// includes no SDK and no Objective-C. They are NOT the Windows values: the
// Mac meeting-status enum has no LOCKED/UNLOCKED, adds AudioReady = 8, and
// every number differs (Mac 9 is OtherMeetingInProgress). Passing a raw Mac
// status to a Windows-shaped function would be a bug; classifyMeetingStatus
// is the one mapping.
#pragma once

#include <optional>
#include <string>

namespace engine_comms {

// ZoomSDKMeetingStatus, ZoomSDK.framework/Headers/ZoomSDKErrors.h:454-482.
enum class MacMeetingStatus : int {
  Idle = 0, Connecting = 1, WaitingForHost = 2, InMeeting = 3, Disconnecting = 4,
  Reconnecting = 5, Failed = 6, Ended = 7, AudioReady = 8, OtherMeetingInProgress = 9,
  InWaitingRoom = 10, WebinarPromote = 12, WebinarDepromote = 13,
  JoinBreakoutRoom = 14, LeaveBreakoutRoom = 15,
};

enum class StatusAction { Ignore, ResetOwnership, ResetAndEndMeeting };

// Windows on_meeting_status (engine-comms.cpp:563-575): LOCKED/UNLOCKED return
// early, INMEETING resets mic ownership, every other status also ends the
// bank. On Mac, AudioReady is the in-meeting notification that plays the
// LOCKED/UNLOCKED role: tearing the bank down on it would re-request all 16
// channels mid-meeting. Unknown values end the meeting (fail closed).
inline StatusAction classifyMeetingStatus(int macStatus) {
  switch (macStatus) {
    case static_cast<int>(MacMeetingStatus::InMeeting): return StatusAction::ResetOwnership;
    case static_cast<int>(MacMeetingStatus::AudioReady): return StatusAction::Ignore;
    default: return StatusAction::ResetAndEndMeeting;
  }
}

// ZoomSDKAudioStatus (ZoomSDKErrors.h:524-536). None (0) means meeting audio
// is not joined at all, and for MicGuard that is muted: the room cannot hear
// us. (The roster fold in main-macos.mm leaves None unmuted for display; the
// comms fold must not.)
inline bool selfMuted(int macAudioStatus) {
  switch (macAudioStatus) {
    case 2:  // UnMuted
    case 4:  // UnMutedByHost
    case 6:  // UnMutedAllByHost
      return false;
    default:
      return true;
  }
}

// ZoomSDKAudioType: None = 0. Talkback delivers only while this client's
// meeting audio is OPEN (ZComms law 1), which is the MTG MIC lamp.
inline bool meetingMicOpen(int macAudioType, int macAudioStatus) {
  return macAudioType != 0 && !selfMuted(macAudioStatus);
}

// UserRole (ZoomSDKErrors.h:750-760): Host = 1, CoHost = 2.
inline bool hostOrCohost(int macUserRole) { return macUserRole == 1 || macUserRole == 2; }

// -[ZoomSDKRawDataController hasRawDataLicense] returns a ZoomSDKError, not a
// bool (Windows HasRawdataLicense is a bool). Only ZoomSDKError_Success (0)
// means licensed.
inline bool licensed(int zoomSdkError) { return zoomSdkError == 0; }

// AVAuthorizationStatus: NotDetermined 0, Restricted 1, Denied 2, Authorized 3.
// A denied microphone on macOS reads as digital silence, not an error, so the
// glue asks before opening the device and reports this verbatim in the mic
// block. Copy: owner decision N1 (Mac-only; Windows has no equivalent).
inline std::string micAccessError(int avAuthorizationStatus) {
  switch (avAuthorizationStatus) {
    case 3: return "";
    case 0: return "Waiting for microphone permission.";
    default:
      return "Microphone access is off for CoreVideo Pro. Turn it on in System "
             "Settings > Privacy & Security > Microphone.";
  }
}

}  // namespace engine_comms
