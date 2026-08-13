#pragma once

// Plugin-host serve respawn backoff (VST round-2 spec A1; vst-host-spec §4.2).
//
// Before this policy, ensurePluginHostServeStarted respawned the isolated host
// with NO throttle — a crash-on-load plugin died in a loop and the core
// hot-relaunched it for the rest of the show. This is the house ladder used by
// CaptureReaderStallPolicy (shell) and BrowserSourceHostAdapter (core):
// retries back off 5→10→20→40→60s and after 5 consecutive failed retries the
// policy GIVES UP — the insert stays auto-bypassed (audio keeps flowing
// unprocessed, loudly) until the operator re-selects the plug-in or clicks
// "Open controls" again, which resets the ladder.
//
// A run that stays alive >= kHealthyRunMs is classified healthy: its eventual
// death (including the host's by-design 30s idle exit) resets the ladder and
// respawns immediately — backoff only ever punishes crash loops, never the
// designed idle-exit/relaunch cycle... (idle exits only happen with no plugin
// loaded; a loaded host lives for the process lifetime).
//
// PURE logic — no clocks, no threads, no locks. The caller feeds
// milliseconds-monotonic timestamps and serializes calls (MediaCore uses
// pluginHostMutex_). Unit-tested in PluginHostRespawnPolicyTest.cpp.

#include <algorithm>
#include <cstdint>

namespace corevideo::modules {

class PluginHostRespawnPolicy {
 public:
  // Stop retrying after this many consecutive respawns that never went healthy.
  static constexpr int kMaxConsecutiveFailures = 5;
  static constexpr int64_t kBaseBackoffMs = 5000;
  static constexpr int64_t kMaxBackoffMs = 60000;
  // A host alive at least this long counts as a healthy run (resets the ladder).
  static constexpr int64_t kHealthyRunMs = 30000;

  // 1st retry waits 5s, then 10s, 20s, 40s, capped at 60s. `consecutiveFailures`
  // is the number of respawn attempts that have already failed (0 before the
  // first retry) — same shape as CaptureReaderStallPolicy.
  [[nodiscard]] static int64_t backoffMsForFailureCount(int consecutiveFailures) {
    if (consecutiveFailures <= 0) {
      return kBaseBackoffMs;
    }
    const int shift = std::min(consecutiveFailures, 30);
    const int64_t scaled = kBaseBackoffMs * (int64_t{1} << shift);
    return scaled < kBaseBackoffMs ? kMaxBackoffMs : std::min(scaled, kMaxBackoffMs);
  }

  // The caller wants the host running and it is not. Returns true when a spawn
  // may proceed NOW (the caller must report the outcome via onLaunchResult).
  // Returns false while a launch is in flight, during a backoff window, or
  // after giving up. A false return also classifies the previous run when one
  // just died: healthy runs reset the ladder, short runs climb it.
  [[nodiscard]] bool requestStart(int64_t nowMs) {
    if (gaveUp_ || launchPending_) {
      return false;
    }
    if (haveLiveRun_) {
      // The run we launched has died (callers only request when the host is
      // gone). Classify it before deciding.
      const bool healthy = nowMs - lastLaunchMs_ >= kHealthyRunMs;
      haveLiveRun_ = false;
      if (healthy) {
        failures_ = 0;  // designed idle exit or a genuinely working host
      } else {
        armRetryAfterFailure(nowMs);
        return false;
      }
    }
    if (nowMs < nextAttemptAtMs_) {
      return false;
    }
    launchPending_ = true;
    return true;
  }

  // Outcome of the spawn a true requestStart authorized. launched=false
  // (CreateProcess failed) counts as a failure on the same ladder.
  void onLaunchResult(int64_t nowMs, bool launched) {
    launchPending_ = false;
    if (launched) {
      lastLaunchMs_ = nowMs;
      haveLiveRun_ = true;
    } else {
      armRetryAfterFailure(nowMs);
    }
  }

  // Operator action (re-selecting the insert, a new selection, or clicking
  // "Open controls") resets the ladder — a stuck give-up must always be
  // recoverable without a restart.
  void reset() {
    failures_ = 0;
    gaveUp_ = false;
    nextAttemptAtMs_ = 0;
  }

  // Telemetry (pluginHost.serve.respawn{attempts,gaveUp}).
  [[nodiscard]] int consecutiveFailures() const { return failures_; }
  [[nodiscard]] bool gaveUp() const { return gaveUp_; }
  [[nodiscard]] bool launchPending() const { return launchPending_; }
  [[nodiscard]] int64_t nextAttemptAtMs() const { return nextAttemptAtMs_; }

 private:
  void armRetryAfterFailure(int64_t nowMs) {
    if (failures_ >= kMaxConsecutiveFailures) {
      gaveUp_ = true;
      return;
    }
    nextAttemptAtMs_ = nowMs + backoffMsForFailureCount(failures_);
    failures_ += 1;
  }

  int failures_ = 0;              // consecutive failed respawns (retries)
  bool gaveUp_ = false;
  bool launchPending_ = false;    // a requestStart-authorized spawn is in flight
  bool haveLiveRun_ = false;      // a launch succeeded and has not been classified dead
  int64_t lastLaunchMs_ = 0;
  int64_t nextAttemptAtMs_ = 0;
};

}  // namespace corevideo::modules
