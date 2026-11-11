// CoreVideo Edge delta 8 (zoom-engine README): talkback in the FAKE engine.
//
// The fake runs the SAME orchestration as the real engine (comms/comms_core,
// the ported ZComms laws over the ported TalkbackChannels) on top of a
// scripted TalkbackSdk instead of Zoom, so tests, the validator and
// screenshots see real ladder behaviour without a meeting:
//
//   - CreateChannel(16) is answered channel by channel ~1.2 s later;
//   - every fake guest supports talkback except ONE "web client" guest
//     (IsSupportTalkback() false, so it is reported and never invited);
//     default: the last baseline guest; COREVIDEO_FAKE_WEB_CLIENT_ID=<id>
//     picks another, 0 = none;
//   - invites confirm ~0.4 s later, removals likewise;
//   - a 50 Hz synthetic mic hands frames to the same bank sink the real
//     pacer uses: keyed channels count channel_sends, and the peak moves
//     while keyed or while TONE is on (-60 dBFS "quiet room" otherwise);
//   - COREVIDEO_FAKE_COMMS_ROLE=attendee: the station is not host/co-host;
//     COREVIDEO_FAKE_COMMS_UNSUPPORTED=1: the meeting does not support it;
//   - the meeting mic runs the REAL MicGuard over a scripted SDK: install
//     succeeds, the silent mic initialises ~0.3 s later
//     (COREVIDEO_FAKE_SILENT_MIC=never: it never does, so the 5 s timeout
//     shows), mute/unmute/uninstall act on a simulated self-mute. A
//     capture-only join (the default, Edge) starts muted; otherwise the
//     operator starts unmuted (COREVIDEO_FAKE_OPERATOR_MUTED=1: muted), and
//     comms_private switches the station to the silent mic and back.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "talkback_eligibility.h"

namespace fakecomms {

struct FakeRosterSource {
  // Called on the comms thread; must return everyone except the station.
  std::function<std::vector<zc::RosterMember>()> roster;
  std::function<bool()> in_meeting;
};

void Start(FakeRosterSource source, uint32_t web_client_id);
// True when `line` is a comms_* command (handled, whatever its validity).
bool HandleLine(const std::string& line);
void SetStationName(const std::string& name);
// Per join (before the meeting starts): Edge joins capture-only; Pro does not.
void SetCaptureOnly(bool capture_only);
void Stop();

}  // namespace fakecomms
