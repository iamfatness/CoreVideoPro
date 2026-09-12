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

// `raw` is the engine event message the core already assembles:
// "join_failed: <reason>", or any other engine error text.
//
// An UNKNOWN reason is returned verbatim. Mapping only what we recognise and
// passing the rest through is what keeps a new SDK failure loud instead of
// silently becoming a generic sentence that says nothing.
inline std::string zoomJoinFailureMessage(const std::string& raw) {
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
