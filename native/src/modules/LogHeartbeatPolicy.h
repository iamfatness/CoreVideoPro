#pragma once
#include <chrono>

namespace corevideo::modules {

// Cadence for idle-time status lines in the bounded core log (#1064).
//
// media-core.log is capped at 8 MB and keeps the newest 4 MB on rollover. A
// status line every 2 s (the monitor worker's, and the screen-capture ingress
// line every 120 frames) was 94 % of the retained lines, so an idle app evicted
// a show's evidence, including its [lock-hold-attribution] lines, in about
// five hours. Status lines now go out when something changed (immediately) or
// once per keepalive period, which stretches the retained window to days.
struct LogHeartbeatPolicy {
  static constexpr std::chrono::seconds kKeepalive{60};

  // everLogged: a line was written before (the first line always goes out).
  // sinceLast: time since that line. changed: a failure count, readiness or
  // other state the line reports moved since the last line.
  static bool due(bool everLogged, std::chrono::steady_clock::duration sinceLast, bool changed) noexcept {
    if (!everLogged || changed) return true;
    return sinceLast >= kKeepalive;
  }
};

}  // namespace corevideo::modules
