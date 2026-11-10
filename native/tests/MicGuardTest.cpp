// The rule that the station's REAL mic can never be open to the room
// (mic_guard.h). The engine performs what the guard decides; these pin the
// decisions.
#include <gtest/gtest.h>

#include "mic_guard.h"

using namespace edgecomms;
using Act = MicGuard::Act;

namespace {

MicGuard::In ready(int64_t now) {
  MicGuard::In in;
  in.in_meeting = true;
  in.can_talkback = true;
  in.capture_only = true;
  in.licence = true;
  in.initialised = false;
  in.self_muted = true;
  in.now_ms = now;
  return in;
}

// Install and confirm the virtual mic; returns with the guard owning.
void installAndInit(MicGuard& g, MicGuard::In& in) {
  g.Next(in);
  g.InstallResult(true, "ok", in.now_ms);
  in.now_ms += 100;
  in.initialised = true;
}

}  // namespace

TEST(MicGuard, NoRawDataLicenceMeansNoInstallAndTheMicStaysClosed) {
  // ZComms: setExternalAudioSource can SUCCEED and never take effect without
  // the raw-data entitlement -- then an unmute would broadcast this PC.
  MicGuard g;
  auto in = ready(1000);
  in.licence = false;
  for (int i = 0; i < 100; ++i, in.now_ms += 100) EXPECT_TRUE(g.Next(in) == Act::None);
  EXPECT_FALSE(g.owns());
  EXPECT_FALSE(g.blocked_reason().empty());  // says why talkback is silent
  EXPECT_EQ(g.TakeLog().size(), size_t{1});  // said once, not every tick
}

TEST(MicGuard, NothingOpensUntilTheVirtualMicIsInitialised) {
  // Owner ruling: no window, however short, where the real mic could reach
  // the room. Installed is not enough; onMicInitialize is the proof.
  MicGuard g;
  auto in = ready(1000);
  EXPECT_TRUE(g.Next(in) == Act::Install);
  g.InstallResult(true, "ok", in.now_ms);
  EXPECT_FALSE(g.owns());  // capture-only keeps muting...
  for (int i = 0; i < 40; ++i) {  // 4 s of waiting: never an unmute
    in.now_ms += 100;
    EXPECT_TRUE(g.Next(in) == Act::None);
    EXPECT_FALSE(g.owns());
  }
  in.now_ms += 100;
  in.initialised = true;  // ...until Zoom confirms the virtual mic
  EXPECT_TRUE(g.Next(in) == Act::Unmute);
  EXPECT_TRUE(g.owns());
  in.now_ms += 100;
  EXPECT_TRUE(g.Next(in) == Act::None);  // re-asserted only every 2 s
  in.now_ms += 2100;
  EXPECT_TRUE(g.Next(in) == Act::Unmute);
  in.self_muted = false;
  in.now_ms += 2100;
  EXPECT_TRUE(g.Next(in) == Act::None);  // open: nothing to do
  EXPECT_TRUE(g.blocked_reason().empty());
}

TEST(MicGuard, NoInitialiseWithinFiveSecondsStaysMutedAndSaysWhy) {
  MicGuard g;
  auto in = ready(1000);
  g.Next(in);
  g.InstallResult(true, "ok", in.now_ms);
  in.now_ms += 5100;
  EXPECT_TRUE(g.Next(in) == Act::None);  // nothing to revoke: it never opened
  EXPECT_FALSE(g.owns());
  EXPECT_TRUE(g.gave_up());
  EXPECT_TRUE(g.blocked_reason().find("never initialised") != std::string::npos);
  // A late initialise does NOT open it this meeting.
  in.initialised = true;
  for (int i = 0; i < 50; ++i) {
    in.now_ms += 100;
    const Act a = g.Next(in);
    EXPECT_TRUE(a != Act::Install && a != Act::Unmute);
  }
  EXPECT_FALSE(g.owns());
  // A new meeting starts clean.
  g.Reset();
  in.now_ms += 100;
  in.initialised = false;
  EXPECT_TRUE(g.Next(in) == Act::Install);
  EXPECT_TRUE(g.blocked_reason().empty());
}

TEST(MicGuard, ADroppedVirtualMicRevokesImmediately) {
  MicGuard g;
  auto in = ready(1000);
  installAndInit(g, in);
  EXPECT_TRUE(g.Next(in) == Act::Unmute);
  EXPECT_TRUE(g.owns());
  // The flag going false (polled) ...
  in.initialised = false;
  in.now_ms += 100;
  EXPECT_TRUE(g.Next(in) == Act::Revoke);  // the caller clears ownership, then mutes
  EXPECT_FALSE(g.owns());
  EXPECT_FALSE(g.blocked_reason().empty());
  in.initialised = true;
  in.now_ms += 100;
  EXPECT_TRUE(g.Next(in) == Act::None);  // gave up for the meeting
  // ... or onMicUninitialized (pushed).
  MicGuard h;
  auto hi = ready(1000);
  installAndInit(h, hi);
  h.Next(hi);
  EXPECT_TRUE(h.Dropped());  // the caller mutes now
  EXPECT_FALSE(h.owns());
  EXPECT_FALSE(h.Dropped());  // nothing owned any more: nothing to mute
  // A drop while still waiting owns nothing: nothing to mute.
  MicGuard w;
  auto wi = ready(1000);
  w.Next(wi);
  w.InstallResult(true, "ok", wi.now_ms);
  EXPECT_FALSE(w.Dropped());
}

TEST(MicGuard, ASynchronousInstallRefusalIsRetriedEveryFiveSecondsAndLoggedOnce) {
  MicGuard g;
  auto in = ready(1000);
  EXPECT_TRUE(g.Next(in) == Act::Install);
  g.InstallResult(false, "setExternalAudioSource 3", in.now_ms);
  EXPECT_FALSE(g.owns());
  in.now_ms += 4900;
  EXPECT_TRUE(g.Next(in) == Act::None);
  in.now_ms += 200;
  EXPECT_TRUE(g.Next(in) == Act::Install);
  g.InstallResult(false, "setExternalAudioSource 3", in.now_ms);
  EXPECT_EQ(g.TakeLog().size(), size_t{1});
  in.now_ms += 5100;
  EXPECT_TRUE(g.Next(in) == Act::Install);
  g.InstallResult(true, "ok", in.now_ms);
  EXPECT_FALSE(g.owns());  // still needs onMicInitialize
}

TEST(MicGuard, AnOperatorWhoSpeaksInTheMeetingIsNeverTouched) {
  // A join that is not capture-only (Pro's operator): no virtual mic, no
  // unmute, ever; only a report when their own mute silences talkback.
  MicGuard g;
  auto in = ready(1000);
  in.capture_only = false;
  in.initialised = true;
  for (int i = 0; i < 100; ++i, in.now_ms += 100) EXPECT_TRUE(g.Next(in) == Act::None);
  EXPECT_FALSE(g.owns());
  EXPECT_EQ(g.TakeLog().size(), size_t{1});
}

TEST(MicGuard, NothingWithoutTalkbackAndLeavingDropsOwnership) {
  MicGuard g;
  auto in = ready(1000);
  in.can_talkback = false;  // not host/co-host, or unsupported
  EXPECT_TRUE(g.Next(in) == Act::None);
  in.can_talkback = true;
  installAndInit(g, in);
  g.Next(in);
  EXPECT_TRUE(g.owns());
  in.in_meeting = false;
  in.now_ms += 100;
  EXPECT_TRUE(g.Next(in) == Act::None);
  EXPECT_FALSE(g.owns());
  EXPECT_FALSE(g.installed());
}
