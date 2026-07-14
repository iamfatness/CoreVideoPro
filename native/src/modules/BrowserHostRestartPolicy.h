#pragma once

// Restart/backoff policy for a corevideo-browser-host.exe supervisor — the same
// discipline as the shell's CaptureReaderStallPolicy (docs/operator-performance-plan):
// exponential backoff 5 -> 10 -> 20 -> 40 -> 60 s between restart attempts, GIVE UP
// after 5 consecutive failures (leave the source loudly "failed" and wait for an
// operator reload instead of churning forever), and reset the failure streak the
// instant a real frame lands. Pure state machine (steady_clock injected as int64
// milliseconds) so it is unit-testable without processes or sleeps.

#include <cstdint>

namespace corevideo::modules {

class BrowserHostRestartPolicy {
 public:
  static constexpr int kMaxConsecutiveFailures = 5;

  // A spawn attempt failed OR the host process died before the streak reset.
  void onFailure(int64_t nowMs) {
    if (consecutiveFailures_ < kMaxConsecutiveFailures) {
      ++consecutiveFailures_;
    }
    nextAttemptAtMs_ = nowMs + backoffDelayMs(consecutiveFailures_);
  }

  // A real frame landed: the host is healthy, forget the streak.
  void onHealthy() {
    consecutiveFailures_ = 0;
    nextAttemptAtMs_ = 0;
  }

  // Operator intervention (reload / re-add): clear give-up and retry immediately.
  void reset() {
    consecutiveFailures_ = 0;
    nextAttemptAtMs_ = 0;
  }

  [[nodiscard]] bool gaveUp() const { return consecutiveFailures_ >= kMaxConsecutiveFailures; }

  [[nodiscard]] bool shouldAttemptRestart(int64_t nowMs) const {
    return !gaveUp() && nowMs >= nextAttemptAtMs_;
  }

  [[nodiscard]] int consecutiveFailures() const { return consecutiveFailures_; }

  [[nodiscard]] int64_t nextAttemptAtMs() const { return nextAttemptAtMs_; }

  // 5s, 10s, 20s, 40s, then capped at 60s.
  [[nodiscard]] static int64_t backoffDelayMs(int failureCount) {
    if (failureCount <= 1) {
      return 5000;
    }
    int64_t delay = 5000;
    for (int i = 1; i < failureCount && delay < 60000; ++i) {
      delay *= 2;
    }
    return delay < 60000 ? delay : 60000;
  }

 private:
  int consecutiveFailures_ = 0;
  int64_t nextAttemptAtMs_ = 0;
};

}  // namespace corevideo::modules
