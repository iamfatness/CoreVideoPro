// The talkback station's orchestration (comms_core) over ZComms' recording
// FakeTalkbackSdk: decisions, not schedules. Time is the test's.
#include <gtest/gtest.h>

#include "comms_core.h"
#include "comms/fake_talkback_sdk.h"
#include "rpc/Json.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace edgecomms;
using corevideo::rpc::Json;
using zctest::FakeTalkbackSdk;

namespace {

class StubAudio : public CommsAudioPort {
 public:
  bool running = false, talk = false;
  std::string mic;
  void SetRunning(bool on) override { running = on; }
  void SetTalk(bool on) override { talk = on; }
  void SetMic(const std::string& d) override { mic = d; }
  void SetSidetoneOut(const std::string&) override {}
  void SetSidetone(bool) override {}
  void SetTone(bool) override {}
  void SetGain(int) override {}
  CommsAudioStatus Status() override {
    CommsAudioStatus s;
    s.running = running;
    s.device = "Desk Mic";
    s.devices = {"Desk Mic", "Headset"};
    if (running) s.peak_dbfs = -42.4;
    return s;
  }
};

zc::RosterMember person(unsigned id, const std::string& name, bool talkback = true) {
  zc::RosterMember m;
  m.user_id = id;
  m.name = name;
  m.supports_talkback = talkback;
  return m;
}

struct Rig {
  FakeTalkbackSdk sdk;
  zc::TalkbackChannels bank{&sdk};
  StubAudio audio;
  CommsBankSink sink;
  CommsCore core{&audio, &sink};
  CommsInputs in;
  int64_t now = 1000;

  Rig() {
    core.SetStationName("Edge Station");
    core.AttachBank(&bank);
    in.in_meeting = in.controller = in.supported = in.host_or_cohost = in.mic_open = true;
    in.roster = {person(201, "Ana"), person(202, "Ben"), person(203, "Web Guest", false)};
  }
  void tick(int64_t advance_ms = 100) {
    now += advance_ms;
    in.now_ms = now;
    core.Tick(in);
  }
  void bringUp() {
    tick();
    for (int i = 0; i < 16; ++i) sdk.EmitChannelCreated("guid-" + std::to_string(i));
    tick();
  }
  Json state() const {
    auto j = Json::parse(core.StateJson());
    return j ? *j : Json();
  }
};

const Json& channel(const Json& state, int slot) { return state.get("channels")->asArray()[static_cast<size_t>(slot)]; }

}  // namespace

TEST(CommsCore, ReportsWhyTalkbackIsUnavailable) {
  Rig r;
  r.in.in_meeting = false;
  r.tick();
  EXPECT_FALSE(r.core.available());
  EXPECT_EQ(r.core.reason(), std::string("Not in a meeting."));
  r.in.in_meeting = true;
  r.in.supported = false;
  r.tick();
  EXPECT_EQ(r.core.reason(), std::string("This meeting does not support Zoom talkback."));
  r.in.supported = true;
  r.in.host_or_cohost = false;
  r.tick();
  EXPECT_EQ(r.core.reason(), std::string("Make this account host or co-host to bring talkback up."));
  EXPECT_EQ(r.sdk.CountOp("create"), 0);  // nothing is asked of Zoom without the role
  const Json s = r.state();
  EXPECT_FALSE(s.get("available")->asBool(true));
  EXPECT_EQ(s.getString("reason"), std::string("Make this account host or co-host to bring talkback up."));
  EXPECT_EQ(s.get("channels")->asArray().size(), size_t{16});  // schema never shrinks
}

TEST(CommsCore, RequestsTheWholeBankInOneCallAndRetriesOnlyWhenNothingCame) {
  Rig r;
  r.tick();
  EXPECT_EQ(r.sdk.CountOp("create"), 1);
  EXPECT_EQ(r.sdk.calls[0].user_ids[0], 16u);
  r.tick(5000);
  EXPECT_EQ(r.sdk.CountOp("create"), 1);  // patience: the answer is async
  r.tick(6000);
  EXPECT_EQ(r.sdk.CountOp("create"), 2);  // nothing granted in 10 s: ask again
  r.sdk.EmitChannelCreated("guid-0");     // a partial grant is a working intercom
  r.tick(20000);
  EXPECT_EQ(r.sdk.CountOp("create"), 2);
  EXPECT_TRUE(r.core.available());
  EXPECT_TRUE(r.audio.running);  // the mic chain comes up with the bank
}

TEST(CommsCore, AsyncRefusalBecomesTheVerbatimReason) {
  Rig r;
  r.tick();
  r.bank.OnCreateChannelResponse("", zc::TalkbackEvent::NoPermission);
  r.tick();
  EXPECT_FALSE(r.core.available());
  EXPECT_EQ(r.core.reason(), std::string("Zoom refused talkback: NO_PERMISSION (need host/co-host)"));
}

TEST(CommsCore, EachEligiblePersonGetsTheirOwnChannelAndTheWebClientNone) {
  Rig r;
  r.bringUp();
  // Healer: ONE call per 600 ms pass.
  for (int i = 0; i < 4; ++i) r.tick(700);
  EXPECT_EQ(r.sdk.CountOp("invite"), 2);
  std::vector<unsigned> invited;
  for (const auto& c : r.sdk.calls)
    if (c.op == "invite") invited.insert(invited.end(), c.user_ids.begin(), c.user_ids.end());
  EXPECT_TRUE(std::find(invited.begin(), invited.end(), 203u) == invited.end());  // never the web client
  r.sdk.EmitUserJoined("guid-0", 201);
  r.sdk.EmitUserJoined("guid-1", 202, zc::TalkbackEvent::AlreadyExists);  // presence, not an error
  r.tick();
  const Json s = r.state();
  EXPECT_EQ(channel(s, 0).getString("label"), std::string("Ana"));
  EXPECT_EQ(channel(s, 1).getString("label"), std::string("Ben"));
  EXPECT_EQ(channel(s, 1).getNumber("listeners"), 1.0);
  EXPECT_TRUE(channel(s, 2).get("label")->isNull());
  bool sawWeb = false;
  for (const auto& p : s.get("roster")->asArray()) {
    if (p.getNumber("participant_id") == 203) {
      sawWeb = true;
      EXPECT_FALSE(p.get("talkback")->asBool(true));
      EXPECT_TRUE(p.get("channels")->asArray().empty());
    }
  }
  EXPECT_TRUE(sawWeb);  // reported, with talkback:false, never hidden
  // Settled: no more invites once everyone is confirmed.
  const int invites = r.sdk.CountOp("invite");
  for (int i = 0; i < 20; ++i) r.tick(700);
  EXPECT_EQ(r.sdk.CountOp("invite"), invites);
}

TEST(CommsCore, AutoAssignWaitsForTheBankToFinishForming) {
  // Assigning while channels are still arriving piled every guest onto the
  // first ready channel (found by the fake probe). All 16, or 5 s of no
  // growth, then each person gets their own.
  Rig r;
  r.tick();
  r.sdk.EmitChannelCreated("guid-0");
  r.tick();
  r.tick(700);
  EXPECT_EQ(r.sdk.CountOp("invite"), 0);
  r.sdk.EmitChannelCreated("guid-1");  // a partial grant: two channels
  r.tick();                            // growth observed here
  r.tick(4000);
  EXPECT_EQ(r.sdk.CountOp("invite"), 0);
  r.tick(1000);  // 5.1 s without growth: settled, first heal call
  r.tick(700);   // second heal call
  EXPECT_EQ(r.sdk.CountOp("invite"), 2);
  std::vector<std::string> channels;
  for (const auto& c : r.sdk.calls)
    if (c.op == "invite") channels.push_back(c.channel_id);
  EXPECT_TRUE(channels.size() == 2 && channels[0] != channels[1]);
}

TEST(CommsCore, KeysRouteOnlyWhileAvailableAndReleaseAlways) {
  Rig r;
  r.in.host_or_cohost = false;
  r.tick();
  EXPECT_FALSE(r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_key","slot":0,"on":true})"), r.now).empty());
  EXPECT_EQ(r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_key","slot":0,"on":false})"), r.now), std::string());
  r.in.host_or_cohost = true;
  r.bringUp();
  EXPECT_EQ(r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_key","slot":2,"on":true})"), r.now), std::string());
  EXPECT_TRUE(r.bank.key(2));  // applied at once: PTT does not wait for a tick
  EXPECT_TRUE(r.audio.talk);
  EXPECT_TRUE(channel(r.state(), 2).get("keyed")->asBool(false));
  r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_key","slot":2,"on":false})"), r.now);
  EXPECT_FALSE(r.bank.key(2));
  EXPECT_FALSE(r.audio.talk);
  // ALL CALL and LATCH ALL span the bank.
  r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_key","slot":"all","on":true})"), r.now);
  EXPECT_EQ(r.bank.key_mask(), 0xFFFFu);
  r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_key","slot":"all","on":false})"), r.now);
  r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_latch","slot":5,"on":true})"), r.now);
  r.tick();
  EXPECT_EQ(r.bank.key_mask(), 1u << 5);
  EXPECT_TRUE(channel(r.state(), 5).get("latched")->asBool(false));
  // Losing co-host drops every key, latched or held.
  r.in.host_or_cohost = false;
  r.tick();
  EXPECT_EQ(r.bank.key_mask(), 0u);
}

TEST(CommsCore, AssignRefusesTheWebClientAndMovesPeople) {
  Rig r;
  r.bringUp();
  const auto refusal = r.core.Apply(
      ParseCommsCommand(R"({"cmd":"comms_assign","slot":4,"participant_id":203,"on":true})"), r.now);
  EXPECT_TRUE(refusal.find("web client") != std::string::npos);
  EXPECT_FALSE(r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_assign","slot":4,"participant_id":999,"on":true})"),
                            r.now)
                   .empty());
  EXPECT_EQ(r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_assign","slot":7,"participant_id":201,"on":true})"),
                         r.now),
            std::string());
  // Pending shows at once (intent), ahead of Zoom's confirmation.
  bool onSeven = false;
  const Json s = r.state();  // keep the tree alive across the loop
  for (const auto& p : s.get("roster")->asArray())
    if (p.getNumber("participant_id") == 201)
      for (const auto& ch : p.get("channels")->asArray()) onSeven = onSeven || ch.asNumber() == 7;
  EXPECT_TRUE(onSeven);
  for (int i = 0; i < 4; ++i) r.tick(700);  // one call per pass, rotated
  bool invitedSeven = false;
  for (const auto& c : r.sdk.calls)
    if (c.op == "invite" && c.channel_id == "guid-7") invitedSeven = true;
  EXPECT_TRUE(invitedSeven);
}

TEST(CommsCore, UnityAtCreationThenDuckOnlyWhileVoiceFlows) {
  Rig r;
  r.bringUp();
  r.tick(400);
  bool unity = false;
  for (const auto& c : r.sdk.calls)
    if (c.op == "volume" && c.volume == zc::DuckPlanner::kUnity) unity = true;
  EXPECT_TRUE(unity);  // Zoom ducks members by default; unity cancels it
  // Keyed but silent: no duck (activity, not key state, drives it).
  r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_key","slot":0,"on":true})"), r.now);
  for (int i = 0; i < 40; ++i) r.tick(400);
  for (const auto& c : r.sdk.calls) EXPECT_FALSE(c.op == "volume" && c.volume == zc::DuckPlanner::kDuck);
  // Voice flows through the sink: slot 0 ducks.
  std::vector<int16_t> voice(960, 12000);
  r.sink.Send(voice.data(), 960);
  r.tick(400);
  bool ducked = false;
  for (const auto& c : r.sdk.calls)
    if (c.op == "volume" && c.volume == zc::DuckPlanner::kDuck && c.channel_id == "guid-0") ducked = true;
  EXPECT_TRUE(ducked);
  EXPECT_TRUE(r.sdk.CountOp("send") >= 1);  // the frame went to the keyed channel
}

TEST(CommsCore, LeavingForgetsTheMeeting) {
  Rig r;
  r.bringUp();
  r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_latch","slot":"all","on":true})"), r.now);
  r.core.DetachBank();
  EXPECT_FALSE(r.audio.talk);
  EXPECT_FALSE(r.audio.running);
  r.in.in_meeting = false;
  r.tick();
  const Json s = r.state();
  EXPECT_FALSE(s.get("available")->asBool(true));
  for (int slot = 0; slot < 16; ++slot) {
    EXPECT_FALSE(channel(s, slot).get("keyed")->asBool(true));
    EXPECT_FALSE(channel(s, slot).get("latched")->asBool(true));
  }
  EXPECT_TRUE(s.get("roster")->asArray().empty());
}

TEST(CommsCore, StateCarriesMeasuredMicAndStats) {
  Rig r;
  r.bringUp();
  const Json s = r.state();
  const Json* mic = s.get("mic");
  ASSERT_TRUE(mic != nullptr);
  EXPECT_EQ(mic->getString("device"), std::string("Desk Mic"));
  EXPECT_EQ(mic->get("devices")->asArray().size(), size_t{2});
  EXPECT_EQ(mic->getNumber("peak_dbfs"), -42.0);  // whole dB
  EXPECT_TRUE(s.get("stats")->get("channel_sends") != nullptr);
  EXPECT_TRUE(s.get("lamps")->get("channel")->asBool(false));
  EXPECT_TRUE(s.get("lamps")->get("meeting_mic")->asBool(false));
  EXPECT_FALSE(s.get("lamps")->get("tx")->asBool(true));
  EXPECT_FALSE(s.get("log")->asArray().empty());
}

TEST(CommsCore, AHoldLeaseExpiresUnlessRefreshed) {
  // A control client that stops re-sending (hung) must not leave a live mic
  // on talent: a leased hold drops by itself; an unleased one does not.
  Rig r;
  r.bringUp();
  r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_key","slot":1,"on":true,"lease_ms":1000})"), r.now);
  r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_key","slot":2,"on":true})"), r.now);
  r.tick(600);
  EXPECT_TRUE(r.bank.key(1));
  r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_key","slot":1,"on":true,"lease_ms":1000})"), r.now);  // refresh
  r.tick(600);
  EXPECT_TRUE(r.bank.key(1));
  r.tick(600);  // 1.2 s since the refresh
  EXPECT_FALSE(r.bank.key(1));
  EXPECT_TRUE(r.bank.key(2));  // unleased holds stay until released
}

TEST(CommsCore, ReleaseAllDropsHoldsButKeepsLatches) {
  Rig r;
  r.bringUp();
  r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_key","slot":"all","on":true})"), r.now);
  r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_key","slot":3,"on":true})"), r.now);
  r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_latch","slot":9,"on":true})"), r.now);
  EXPECT_EQ(r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_release_all"})"), r.now), std::string());
  EXPECT_EQ(r.bank.key_mask(), 1u << 9);
  EXPECT_TRUE(r.audio.talk);  // the latch still talks
}

TEST(CommsCore, TheMeterAloneDoesNotDriveTheLineAtTenHertz) {
  struct MovingMeter : StubAudio {
    double level = -40.0;
    CommsAudioStatus Status() override {
      auto s = StubAudio::Status();
      if (running) s.peak_dbfs = level;
      return s;
    }
  };
  FakeTalkbackSdk sdk;
  zc::TalkbackChannels bank{&sdk};
  MovingMeter audio;
  CommsBankSink sink;
  CommsCore core{&audio, &sink};
  CommsStateEmitter emitter;
  CommsInputs in;
  in.in_meeting = in.controller = in.supported = in.host_or_cohost = in.mic_open = true;
  core.AttachBank(&bank);
  int64_t now = 1000;
  in.now_ms = now;
  core.Tick(in);
  for (int i = 0; i < 16; ++i) sdk.EmitChannelCreated("guid-" + std::to_string(i));
  std::string out;
  int lines = 0;
  // 2 s of a moving meter and nothing else: at most one line per 250 ms.
  for (int i = 0; i < 20; ++i) {
    now += 100;
    in.now_ms = now;
    audio.level = -40.0 + (i % 2 ? 3.0 : -3.0);
    core.Tick(in);
    if (emitter.Next(core, now, &out)) ++lines;
  }
  EXPECT_TRUE(lines <= 9);
  EXPECT_TRUE(lines >= 4);  // but the meter still moves
  // A real change goes out at once, whatever the meter did.
  core.Apply(ParseCommsCommand(R"({"cmd":"comms_key","slot":0,"on":true})"), now);
  EXPECT_TRUE(emitter.Next(core, now, &out));
  EXPECT_TRUE(out.find("\"keyed\":true") != std::string::npos);
}

TEST(CommsCore, AClosedMeetingMicIsTheReasonAndKeysAreRefused) {
  // The mic rule kept the meeting mic closed (no licence, or the virtual mic
  // never initialised): nothing keyed would be heard, so say so plainly.
  Rig r;
  r.bringUp();
  r.in.mic_blocked = "Talkback is silent: the silent virtual mic never initialised.";
  r.tick();
  EXPECT_FALSE(r.core.available());
  EXPECT_EQ(r.core.reason(), r.in.mic_blocked);
  EXPECT_FALSE(r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_key","slot":0,"on":true})"), r.now).empty());
  r.in.mic_blocked.clear();
  r.tick();
  EXPECT_TRUE(r.core.available());
}

TEST(CommsCore, PrivateIsTheRequestedMicModeAndTheStateReportsTheAppliedOne) {
  Rig r;
  r.bringUp();
  EXPECT_FALSE(r.core.private_requested());
  EXPECT_TRUE(r.state().get("private") != nullptr);
  EXPECT_FALSE(r.state().get("private")->asBool(true));
  // Not gated on availability: the mode must always be switchable.
  r.in.host_or_cohost = false;
  r.tick();
  EXPECT_EQ(r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_private","on":true})"), r.now), std::string());
  EXPECT_TRUE(r.core.private_requested());
  r.in.private_mode = true;  // what the engine applies (capture_only || requested)
  r.tick();
  EXPECT_TRUE(r.state().get("private")->asBool(false));
  EXPECT_EQ(r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_private","on":false})"), r.now), std::string());
  EXPECT_FALSE(r.core.private_requested());
  r.core.DetachBank();  // the request survives meetings
  EXPECT_EQ(r.core.Apply(ParseCommsCommand(R"({"cmd":"comms_private","on":true})"), r.now), std::string());
  r.core.DetachBank();
  EXPECT_TRUE(r.core.private_requested());
}
