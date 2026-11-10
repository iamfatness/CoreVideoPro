// CoreVideo Edge (not ported): the rule that the station's REAL microphone
// can never be open to the meeting room. SDK-free so it is unit-tested.
//
// Background. Talkback is silent unless our meeting audio is OPEN (ZComms law
// 1), and an open meeting mic broadcasts whatever device Zoom captures. The
// ZComms/ZoomISO answer is a never-fed virtual mic (setExternalAudioSource):
// open, but silent to the room. ZComms documents that the install call can
// SUCCEED and never take effect when the auth tier lacks the raw-data
// entitlement (public-app-key: HasRawdataLicense() false) -- then the room
// would hear this PC. So "owning" the mic (capture-only standing down and the
// engine unmuting us) waits for PROOF:
//
//   - no raw-data licence: never install, never own;
//   - install OK: still NOT owned and still muted; only when the virtual mic
//     reports onMicInitialize does the guard take ownership and unmute (owner
//     ruling 2026-11-09: no window, however short, where the real mic could
//     reach the room);
//   - no onMicInitialize within kInitTimeoutMs: stay muted, never own, give
//     up for the meeting and say so plainly (blocked_reason) -- the live gate
//     tells us whether the SDK only initialises once audio flows;
//   - once owned, any sign the virtual mic was dropped (onMicUninitialized,
//     the flag going false) revokes immediately -- ownership off, mute -- and
//     gives up for the meeting;
//   - a synchronous install refusal costs nothing (we never unmuted), so it
//     is retried every kRetryMs; only the first refusal is logged;
//   - a non-capture-only join (an operator who speaks in the meeting) is never
//     touched: no virtual mic, no unmute; their own mute state is reported.
//
// The caller performs the action Next() returns and reports install results.
// owns() is what capture-only consults; the caller mirrors it into the
// engine-wide flag, clearing it BEFORE muting.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace edgecomms {

class MicGuard {
 public:
  static constexpr int64_t kInitTimeoutMs = 5000;
  static constexpr int64_t kRetryMs = 5000;
  static constexpr int64_t kUnmuteEveryMs = 2000;

  struct In {
    bool in_meeting = false;
    bool can_talkback = false;  // controller && supported && host/co-host
    bool capture_only = true;   // Edge joins capture-only; Pro's operator does not
    bool licence = false;       // HasRawdataLicense()
    bool initialised = false;   // the virtual mic's onMicInitialize .. onMicUninitialized
    bool self_muted = true;
    int64_t now_ms = 0;
  };

  enum class Act { None, Install, Unmute, Revoke };

  Act Next(const In& in);
  void InstallResult(bool ok, const std::string& detail, int64_t now_ms);
  // onMicUninitialized: true if we owned the mic (the caller must mute NOW).
  bool Dropped();
  // Left the meeting / any status away from INMEETING / re-entry.
  void Reset();

  bool owns() const { return owns_; }
  bool installed() const { return installed_; }
  bool gave_up() const { return gave_up_; }
  // Non-empty while talkback is silent because of the mic rule; operator text
  // for the comms state reason.
  const std::string& blocked_reason() const { return blocked_; }
  // Ops-log lines produced since the last call.
  std::vector<std::string> TakeLog();

 private:
  void Log(std::string line) { log_.push_back(std::move(line)); }
  void GiveUp(const std::string& reason);

  bool installed_ = false;
  bool owns_ = false;
  bool seen_initialised_ = false;
  bool gave_up_ = false;
  bool logged_install_failure_ = false;
  bool logged_no_licence_ = false;
  bool logged_operator_muted_ = false;
  int64_t installed_at_ms_ = 0;
  int64_t retry_at_ms_ = 0;
  int64_t next_unmute_ms_ = 0;
  std::string blocked_;
  std::vector<std::string> log_;
};

}  // namespace edgecomms
