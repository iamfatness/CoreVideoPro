// CoreVideo Edge (not ported): the talkback station's orchestration, with no
// Zoom SDK in sight.
//
// This is ZComms src/app/main.cpp's main loop (@ 168a572), lifted out of the
// app so the REAL engine (engine-comms.cpp, over TalkbackSdkWin) and the FAKE
// engine (over a scripted TalkbackSdk) run the same laws:
//
//   - the whole 16-channel bank is requested in ONE CreateChannel(16) call
//     (CreateChannel is rate limited); a partial grant is a working intercom;
//   - every eligible participant auto-lands on their OWN channel (spillover
//     to the least-loaded one past 16), so a channel's key wears a name;
//   - membership heals ONE SDK call per 600 ms pass, rotated across channels
//     (Zoom's limiter is per CALL), with per-(channel,person) backoff
//     5 s doubling to 60 s and 10 s of patience after a successful Execute;
//   - ALREADY_EXIST is confirmed presence (TalkbackChannels records it);
//   - intent for people who left, or who are known-ineligible (the web
//     client: IsSupportTalkback() false), is pruned -- ids are recycled;
//   - meeting audio: unity the moment a channel exists, duck only while that
//     channel is keyed AND the voice actually carries audio (DuckPlanner, one
//     paced SDK call per pass);
//   - a keyed channel nobody is in yet says so once per key-down.
//
// Threading: everything here runs on ONE thread (the engine's SDK thread, or
// the fake's comms thread under its lock). The paced TX thread touches only
// CommsBankSink, which guards its bank pointer itself.
//
// Not in this slice (the ZComms screen leaves room for them): extern feeds,
// echo cancel, breakout-room awareness (room is always null, reach counts
// everyone as reachable), chat cues.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "comms_protocol.h"
#include "duck_plan.h"
#include "signal_gate.h"
#include "talkback_channels.h"
#include "talkback_eligibility.h"
#include "tx_pacer.h"

namespace edgecomms {

// What the audio side measured. Every number here is measured or absent.
struct CommsAudioStatus {
  bool running = false;
  std::string device;                // capture device in use, or the one asked for
  std::vector<std::string> devices;  // capture devices, as enumerated
  std::optional<double> peak_dbfs;   // absent unless capture is running
  int gain_db = 0;
  bool sidetone = true;
  std::string sidetone_device;
  std::vector<std::string> sidetone_devices;
  bool tone = false;
  uint64_t frames_sent = 0;          // frames the 20 ms pacer handed to the bank sink
  uint64_t underruns = 0;
  std::string error;                 // last device-open failure, verbatim ("" = none)
};

// The mic chain behind the station. Real: ZComms' AudioEngine on a worker
// thread (engine-comms.cpp). Fake: a synthetic 50 Hz producer.
class CommsAudioPort {
 public:
  virtual ~CommsAudioPort() = default;
  virtual void SetRunning(bool on) = 0;  // capture + pacer up while the bank is
  virtual void SetTalk(bool on) = 0;     // PTT envelope; must take effect at once
  virtual void SetMic(const std::string& device) = 0;
  virtual void SetSidetoneOut(const std::string& device) = 0;
  virtual void SetSidetone(bool on) = 0;
  virtual void SetTone(bool on) = 0;
  virtual void SetGain(int db) = 0;
  virtual CommsAudioStatus Status() = 0;
};

// The seam between the paced TX thread and the channel bank: ZComms
// main.cpp's ChannelBankSink with the extern-feed mix removed (this slice has
// no feeds, so the voice frame fans out to every keyed, ready channel through
// TalkbackChannels::SendToKeyed -- mono, law 5). Attach(nullptr) returns only
// after any Send in flight has finished, so the bank can then be destroyed.
class CommsBankSink : public zc::FrameSink {
 public:
  CommsBankSink();
  void Attach(zc::TalkbackChannels* bank);
  bool CanSend() override;
  bool Send(const int16_t* pcm, int samples) override;
  int tx_peak() const { return tx_peak_.load(); }  // 0..32767, post-envelope
  bool voice_active() const { return voice_active_.load(); }

 private:
  std::mutex m_;
  zc::TalkbackChannels* bank_ = nullptr;
  zc::SignalGate voice_gate_;
  std::atomic<int> tx_peak_{0};
  std::atomic<bool> voice_active_{false};
};

struct CommsInputs {
  bool in_meeting = false;
  bool controller = false;      // the SDK handed out a talkback controller
  bool supported = false;       // IsMeetingSupportTalkBack()
  bool host_or_cohost = false;  // this station's role
  bool mic_open = false;        // our meeting audio is joined AND unmuted (law 1)
  // Non-empty: talkback would be silent because the mic rule keeps our
  // meeting mic closed (MicGuard::blocked_reason). Becomes the reason.
  std::string mic_blocked;
  // The station's mic mode as the engine applies it: capture-only (Edge) OR
  // Pro's "Private comms" requested. Reported as comms_state.private.
  bool private_mode = false;
  std::vector<zc::RosterMember> roster;  // everyone except this station
  int64_t now_ms = 0;
};

class CommsCore {
 public:
  static constexpr int kSlots = kCommsSlots;  // == TalkbackChannels::kMaxChannels
  static constexpr uint32_t kAllSlots = (1u << kSlots) - 1u;

  CommsCore(CommsAudioPort* audio, CommsBankSink* sink);

  void SetStationName(std::string name) { station_ = std::move(name); }

  // A new meeting's bank (TalkbackChannels over this meeting's controller).
  void AttachBank(zc::TalkbackChannels* bank);
  // The meeting is over: the sink lets go of the bank and every per-meeting
  // fact (intent, backoff, keys, latches, the duck's view) is forgotten --
  // user ids are meeting-scoped and recycled.
  void DetachBank();
  bool has_bank() const { return bank_ != nullptr; }

  void Tick(const CommsInputs& in);

  // "" = accepted. Otherwise the refusal, worded for the operator.
  std::string Apply(const CommsCommand& c, int64_t now_ms);

  // The E2P comms_state line (docs/PROTOCOL.md). Pure function of the last
  // Tick's inputs, the bank and the audio status. with_peak=false renders
  // mic.peak_dbfs as null: the emitter's "did anything but the meter move".
  std::string StateJson(bool with_peak = true) const;

  bool available() const { return available_; }
  // comms_private as last requested (survives meetings; the engine feeds
  // MicGuard from it and reports CommsInputs::private_mode).
  bool private_requested() const { return private_requested_; }
  const std::string& reason() const { return reason_; }
  uint32_t keys() const { return applied_keys_; }
  // Every HELD key and ALL CALL up; latches stay (they are deliberate).
  void ReleaseHolds();
  void Log(const std::string& line);

 private:
  void Housekeeping();
  void Heal();
  void ApplyKeys();
  void Duck();
  void NoticeSends();
  std::string NameOf(unsigned int uid) const;

  CommsAudioPort* audio_;
  CommsBankSink* sink_;
  zc::TalkbackChannels* bank_ = nullptr;
  std::string station_;

  CommsInputs in_;
  CommsAudioStatus audio_status_;
  bool eligible_ = false;   // in a meeting where this station may run talkback
  bool available_ = false;
  std::string reason_ = "Not in a meeting.";

  // Bring-up. Auto-assign waits for the bank to finish forming (all 16, or
  // no new channel for kSettleMs): assigning while channels are still
  // arriving piles everyone onto the first few (ZComms only enters its main
  // loop after its bring-up loop has waited the bank out).
  static constexpr int64_t kSettleMs = 5000;
  int64_t next_create_ms_ = 0;
  int last_ready_ = 0;               // bank growth, for the auto-assign settle
  int64_t last_ready_change_ms_ = 0;
  int create_rounds_ = 0;
  std::string create_error_;
  bool announced_up_ = false;

  // Membership intent per (slot, uid) -- ZComms' model verbatim.
  std::set<std::pair<int, unsigned int>> intent_;
  std::set<unsigned int> auto_assigned_;
  std::map<std::pair<int, unsigned int>, std::pair<int, int64_t>> invite_backoff_;
  int heal_rr_ = 0;
  int64_t next_heal_ms_ = 0;
  int64_t next_house_ms_ = 0;

  // Keying: latch per slot, operator hold per slot, ALL CALL hold.
  bool private_requested_ = false;
  uint32_t latch_mask_ = 0;
  uint32_t hold_mask_ = 0;
  bool hold_all_ = false;
  // Hold leases: 0 = held until released; otherwise the steady-clock ms at
  // which the hold drops unless the key is re-sent. [16] = ALL CALL.
  int64_t hold_until_ms_[kSlots + 1] = {};
  uint32_t applied_keys_ = 0;
  uint32_t prev_keys_ = 0;

  zc::DuckPlanner duck_;
  uint32_t prev_sent_mask_ = 0;
  int64_t keyed_silent_since_ = 0;
  std::string last_audio_error_;

  std::deque<std::string> log_;  // latest last, capped
};

// Decides when a comms_state line goes out: on any change, except that a
// change in the meter alone goes out at most every kMeterMs (a live mic would
// otherwise drive the line at the full 10 Hz tick).
class CommsStateEmitter {
 public:
  static constexpr int64_t kMeterMs = 250;
  // True with *out filled when a line should be written now.
  bool Next(const CommsCore& core, int64_t now_ms, std::string* out);

 private:
  std::string last_shape_;
  std::string last_full_;
  int64_t last_emit_ms_ = 0;
};

}  // namespace edgecomms
