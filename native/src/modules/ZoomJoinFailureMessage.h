#pragma once

// #475. The engine answers the Zoom SDK's join-time prompts now (it used to
// answer none, so a prompted join hung until the shell's 52 s timeout and then
// said only "Timed out waiting for Zoom meeting join result."). Each answer
// carries a machine reason on the wire. This turns that reason into something
// an operator can act on, and is deliberately the ONLY place that mapping
// lives, so the wire vocabulary and the operator vocabulary cannot drift.
//
// Pure, so every phrase is a unit test rather than a live meeting.

#include <string>

namespace corevideo::modules {

// The SDK's own join verdict: the engine reports MeetingFailCode as
// "meeting_failed: MEETING_FAIL_<NAME>" (both engines spell the reason from the
// SDK enum — main.cpp meeting_fail_name / main-macos.mm meeting_fail_name). Only
// codes those tables actually emit are mapped here; an unmapped one is returned
// verbatim. Mac 1.0.0, 2026-11-15: a guest (not signed in) join of an
// authenticated-only meeting failed with
// {"msg":"meeting_failed","code":504,"reason":"MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING"}
// and the operator read "Timed out waiting for Zoom meeting join result.".
inline std::string zoomMeetingFailCodeMessage(const std::string& reason) {
  if (reason == "MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING" ||
      reason == "MEETING_FAIL_NEED_SIGN_IN_FOR_PRIVATE_MEETING" ||
      reason == "MEETING_FAIL_ENFORCE_LOGIN") {
    return "Zoom requires you to sign in to join this meeting. "
           "Use Sign in with Zoom, then join again.";
  }
  if (reason == "MEETING_FAIL_PASSWORD_ERR") {
    return "Zoom rejected the meeting passcode. Check the passcode and join again.";
  }
  if (reason == "MEETING_FAIL_MEETING_NOT_START") {
    return "This meeting has not started yet. Join again once the host has started it.";
  }
  if (reason == "MEETING_FAIL_CONFLOCKED") {
    return "The host has locked this meeting, so no one else can join.";
  }
  if (reason == "MEETING_FAIL_MEETING_OVER") {
    return "This meeting has already ended.";
  }
  if (reason == "MEETING_FAIL_MEETING_NOT_EXIST") {
    return "Zoom could not find this meeting. Check the meeting ID or link.";
  }
  if (reason == "MEETING_FAIL_MEETING_USER_FULL") {
    return "This meeting is full.";
  }
  if (reason == "MEETING_FAIL_HOST_DISALLOW_OUTSIDE_USER_JOIN") {
    return "The host only allows people from their own Zoom account to join this meeting.";
  }
  return {};
}

// `raw` is the engine event message the core already assembles:
// "join_failed: <reason>", "meeting_failed: <MEETING_FAIL_*>", or any other engine error text.
//
// An UNKNOWN reason is returned verbatim. Mapping only what we recognise and
// passing the rest through is what keeps a new SDK failure loud instead of
// silently becoming a generic sentence that says nothing.
inline std::string zoomJoinFailureMessage(const std::string& raw) {
  constexpr const char* kMeetingFailedPrefix = "meeting_failed: ";
  const auto meetingFailedLength = std::string(kMeetingFailedPrefix).size();
  if (raw.compare(0, meetingFailedLength, kMeetingFailedPrefix) == 0) {
    auto mapped = zoomMeetingFailCodeMessage(raw.substr(meetingFailedLength));
    return mapped.empty() ? raw : mapped;
  }
  constexpr const char* kPrefix = "join_failed: ";
  const auto prefixLength = std::string(kPrefix).size();
  if (raw.compare(0, prefixLength, kPrefix) != 0) {
    return raw;
  }
  const auto reason = raw.substr(prefixLength);
  if (reason == "account-busy-elsewhere") {
    // Never says "we ended it" — CoreVideo cancelled and changed nothing. The
    // second sentence is the action the operator has, and it matches the
    // wording of the retry the shell offers.
    return "Your Zoom account is already in this meeting, for example in the Zoom app. "
           "Leave it there and join again, or choose \"Join and end my other Zoom session\".";
  }
  if (reason == "passcode-or-name-required") {
    return "Zoom asked for a passcode or a display name for this meeting. "
           "Check the meeting link includes its passcode.";
  }
  if (reason == "webinar-registration-required") {
    return "This webinar requires registration, so CoreVideo cannot join it.";
  }
  if (reason == "webinar-screen-name-required") {
    return "This webinar asked for a screen name before joining.";
  }
  if (reason == "user-info-required") {
    return "Zoom asked for user information before joining this meeting.";
  }
  if (reason == "meeting-expired-or-deleted") {
    return "This meeting has expired or was deleted, so CoreVideo did not recover it.";
  }
  return raw;
}

}  // namespace corevideo::modules
