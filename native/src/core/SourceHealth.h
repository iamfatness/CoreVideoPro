#pragma once
#include <cstdint>

namespace corevideo::core {

enum class SourceHealth { Producing, Warming, Stalled, Failed, Idle };

inline const char* sourceHealthName(SourceHealth health) {
  switch (health) {
    case SourceHealth::Producing: return "producing";
    case SourceHealth::Warming: return "warming";
    case SourceHealth::Stalled: return "stalled";
    case SourceHealth::Failed: return "failed";
    case SourceHealth::Idle: return "idle";
  }
  return "idle";
}

// #535 slice 4a (R1): the ON-AIR stall threshold, deliberately different from
// SourceBus::kStaleAfterNs (200ms). 200ms is a fast DIAGNOSTIC signal (used
// for sources[] / operator troubleshooting); an on-air CUT to black must
// never fire on ordinary cadence noise — browser sources, WGC screen
// capture, stills and paused clips legitimately re-serve the same frameId
// for long stretches while perfectly healthy, and even a live Zoom guest on
// a slow link or sharing a static screen can go well past 200ms between
// real frames. 1.5s is long enough that only a genuinely dead Zoom feed
// trips it, and it recovers on the very next new frame (lastNewFrameNs
// advances the instant one arrives — no separate "recovered" state to get
// wrong).
inline constexpr int64_t kOnAirStallNs = 1'500'000'000;

}  // namespace corevideo::core
