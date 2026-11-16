#pragma once

// When the core starts the next virtual-camera sink helper process (macOS,
// plan docs/superpowers/plans/2026-11-16-mac-vcam-provider-arrival-fix.md).
//
// Why a helper process at all: a process's CoreMediaIO view of camera-extension
// PROVIDERS is taken at its first CMIO touch, and a provider that registers
// later (first install, an extension update) never reaches it (measured on
// macOS 26.3, 2026-11-16: the core polled CAMERA_EXTENSION_NOT_FOUND 210+ times
// while every newborn process saw the camera). So the CMIO sink lives in a
// short-lived helper. Each connect attempt is a NEW process, whose first touch
// is by construction after whatever registered before it was born. The core
// never restarts to recover a camera.
//
// Pure decision: value in, value out, injected clock, no processes. Two classes
// of failed attempt, on purpose:
//
//   * The camera is not reachable (not installed, awaiting approval, mid-update,
//     producer refused). The helper ran correctly and said so. This is an
//     ordinary long-lived state (an operator approves the extension minutes
//     after turning the camera on), so it is retried FOREVER, 1 s apart at
//     first (an update's gap is under a second) and slower once it has
//     clearly settled, so an absent extension does not cost a process a second
//     for the whole show.
//   * The helper itself failed (could not be spawned, crashed, hung). That is
//     the house ladder (OutputDestinationSupervisorPolicy.h): 5 -> 10 -> 20 ->
//     40 -> 60 s, give up loudly after 5 consecutive failures, a healthy run
//     earns the budget back, an operator re-arm always clears give-up.

#include <cstdint>
#include <string>

namespace corevideo::modules {

enum class VcamHelperOutcome {
  Connected,          // the helper reached the sink and is streaming
  CameraUnreachable,  // CAMERA_EXTENSION_NOT_FOUND / _LOST, CAMERA_PRODUCER_REFUSED, CAMERA_SINK_FAILED
  HelperFailed,       // spawn failure, crash, no answer in time
};

inline constexpr std::int64_t kVcamHelperFastRetryMs = 1000;
inline constexpr int kVcamHelperFastRetries = 10;
inline constexpr std::int64_t kVcamHelperSettledRetryMs = 2000;
inline constexpr int kVcamHelperSettledRetries = 30;
inline constexpr std::int64_t kVcamHelperSlowRetryMs = 5000;
inline constexpr int kVcamHelperMaxConsecutiveFailures = 5;
inline constexpr std::int64_t kVcamHelperHealthyRunMs = 30'000;

// A helper's reported warning, as an outcome. An unknown or empty warning from
// a helper that did not connect is a helper failure: guessing "unreachable"
// would retry a broken helper forever without ever saying so.
inline VcamHelperOutcome classifyVcamHelperWarning(const std::string& warning) {
  return warning.rfind("CAMERA_", 0) == 0 ? VcamHelperOutcome::CameraUnreachable
                                           : VcamHelperOutcome::HelperFailed;
}

// True when a connect result shows the helper (a newborn process) found the
// camera device itself, whatever happened next (refused, sink failed).
inline bool vcamConnectResultSawDevice(const std::string& warning) {
  return warning.rfind("CAMERA_EXTENSION_NOT_FOUND", 0) != 0;
}

inline std::int64_t vcamHelperFailureBackoffMs(int consecutiveHelperFailures) {
  switch (consecutiveHelperFailures) {
    case 0: return 0;
    case 1: return 5'000;
    case 2: return 10'000;
    case 3: return 20'000;
    case 4: return 40'000;
    default: return 60'000;
  }
}

inline std::int64_t vcamUnreachableRetryMs(int consecutiveUnreachable) {
  if (consecutiveUnreachable <= kVcamHelperFastRetries) return kVcamHelperFastRetryMs;
  if (consecutiveUnreachable <= kVcamHelperSettledRetries) return kVcamHelperSettledRetryMs;
  return kVcamHelperSlowRetryMs;
}

struct VcamHelperSpawnDecision {
  bool spawnNow = false;
  bool gaveUp = false;
  std::int64_t waitMs = 0;  // until the next attempt is allowed (0 when spawnNow or gaveUp)
};

class VcamHelperRespawnPolicy {
 public:
  // Operator turned the camera on (again): every budget is fresh.
  void rearm() { *this = VcamHelperRespawnPolicy(); }

  VcamHelperSpawnDecision next(std::int64_t nowMs) const {
    if (gaveUp_) return {false, true, 0};
    if (!hasAttempted_ || nowMs >= nextAllowedMs_) return {true, false, 0};
    return {false, false, nextAllowedMs_ - nowMs};
  }

  void onSpawned() { ++attempts_; }

  // The outcome of the attempt (or, for a connected helper, of its start).
  void onOutcome(VcamHelperOutcome outcome, std::int64_t nowMs) {
    hasAttempted_ = true;
    switch (outcome) {
      case VcamHelperOutcome::Connected:
        consecutiveUnreachable_ = 0;
        connectedSinceMs_ = nowMs;
        connected_ = true;
        nextAllowedMs_ = nowMs;
        return;
      case VcamHelperOutcome::CameraUnreachable:
        settleRun(nowMs);
        ++consecutiveUnreachable_;
        nextAllowedMs_ = nowMs + vcamUnreachableRetryMs(consecutiveUnreachable_);
        return;
      case VcamHelperOutcome::HelperFailed:
        settleRun(nowMs);
        ++consecutiveHelperFailures_;
        if (consecutiveHelperFailures_ >= kVcamHelperMaxConsecutiveFailures) {
          gaveUp_ = true;
          return;
        }
        nextAllowedMs_ = nowMs + vcamHelperFailureBackoffMs(consecutiveHelperFailures_);
        return;
    }
  }

  bool gaveUp() const { return gaveUp_; }
  std::uint64_t attempts() const { return attempts_; }
  int consecutiveUnreachable() const { return consecutiveUnreachable_; }
  int consecutiveHelperFailures() const { return consecutiveHelperFailures_; }

 private:
  // A connected helper that ran long enough earns the helper-failure budget
  // back; a helper that connected and died at once does not.
  void settleRun(std::int64_t nowMs) {
    if (connected_ && nowMs - connectedSinceMs_ >= kVcamHelperHealthyRunMs) consecutiveHelperFailures_ = 0;
    connected_ = false;
  }

  bool hasAttempted_ = false;
  bool gaveUp_ = false;
  bool connected_ = false;
  std::int64_t nextAllowedMs_ = 0;
  std::int64_t connectedSinceMs_ = 0;
  int consecutiveUnreachable_ = 0;
  int consecutiveHelperFailures_ = 0;
  std::uint64_t attempts_ = 0;
};

}  // namespace corevideo::modules
