// end-other-meeting.h — the SDK-free half of the #475 same-account answer,
// shared so the macOS engine (main-macos.mm) is testable without the Zoom SDK.
//
// Joining as the signed-in operator (ZAK) while that account is already in the
// meeting — their own Zoom app — makes the Meeting SDK ask the APP whether to
// end the other session (Windows: IMeetingConfigurationEvent::
// onEndOtherMeetingToJoinMeetingNotification; macOS: ZoomSDKMeetingServiceDelegate
// -onEndOtherMeetingToJoinMeetingNotification:actionCancel:). Unanswered, the
// join hangs until our own timeout (docs/reference/zoom-join-prompts.md).
//
// The rules, identical to the Windows engine (main.cpp EngineConfigurationEvent):
//  - the takeover is an EXPLICIT per-join choice, read fresh from each join
//    line and never stored — ending the other session evicts the operator's
//    own Zoom client, so it only happens because they just asked;
//  - by default CANCEL and fail the join with reason "account-busy-elsewhere",
//    which the core's one mapping (modules/ZoomJoinFailureMessage.h) turns
//    into the operator sentence that offers "Join and end my other Zoom session".
//
// The IPC lines are byte-identical to main.cpp's so the core and a log reader
// cannot tell the two engines apart.

#pragma once

#include <string>

namespace engine_join {

enum class EndOtherMeetingAnswer { EndOther, Cancel };

/// True only when THIS join command carries the operator's takeover choice.
inline bool joinRequestsEndOtherMeeting(const std::string& joinLine) {
    return joinLine.find("\"end_other_meeting\":true") != std::string::npos;
}

/// The answer to the SDK prompt for a join sent with `operatorChoseEndOther`.
inline EndOtherMeetingAnswer answerEndOtherMeeting(bool operatorChoseEndOther) {
    return operatorChoseEndOther ? EndOtherMeetingAnswer::EndOther
                                 : EndOtherMeetingAnswer::Cancel;
}

/// The event line the engine writes after answering.
inline const char* endOtherMeetingEventLine(EndOtherMeetingAnswer answer) {
    return answer == EndOtherMeetingAnswer::EndOther
        ? R"({"cmd":"debug","stage":"end_other_meeting","action":"end"})"
        : R"({"cmd":"error","stage":"join","msg":"join_failed","reason":"account-busy-elsewhere"})";
}

/// Written when the SDK hands us no way to answer (Windows: null handler).
inline const char* endOtherMeetingUnanswerableLine() {
    return R"({"cmd":"debug","stage":"end_other_meeting","code":-1})";
}

}  // namespace engine_join
