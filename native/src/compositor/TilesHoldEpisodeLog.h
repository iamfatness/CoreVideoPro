#pragma once

#include <cstdint>

#include "compositor/TilesMembership.h"

namespace corevideo::compositor {

// #1074 review: the `[tiles-hold]` log line is per EPISODE, not per state
// transition. A degraded Zoom feed delivering one frame every 1.5 to 2 s
// classifies Fresh -> Held -> Fresh twice per frame; logging every transition
// was tens of lines a second on a 25-tile wall, from the render tick, under
// the core mutex, into a 128-slot best-effort queue that drops lines.
//
//   held       logged once per episode; re-armed only after the member has
//              been continuously Fresh for kTilesHoldLogRearmMs.
//   dropped    always logged (once per drop).
//   recovered  logged only after a `dropped`, never after a mere hold.
//
// The re-arm is 2 x kTilesStaleFrameMs, not ~1 s, on purpose: a feed that
// delivers a frame every 1.5 to 2 s is Fresh for at most 1.5 s at a stretch,
// so any re-arm at or below 1.5 s would re-log `held` for every frame of
// exactly the feed this exists to quiet.
constexpr int64_t kTilesHoldLogRearmMs = 2 * kTilesStaleFrameMs;

class TilesHoldEpisodeLog {
 public:
  // The label to log for this tick's classification, or nullptr for silence.
  // nowMs is the render tick's own clock (the same one ages are measured on).
  const char* observe(TilesMemberState state, int64_t nowMs) {
    switch (state) {
      case TilesMemberState::Held:
        freshSinceMs_ = -1;
        if (heldLogged_) return nullptr;
        heldLogged_ = true;
        return "held";
      case TilesMemberState::Dropped:
        freshSinceMs_ = -1;
        if (dropped_) return nullptr;
        dropped_ = true;
        return "dropped";
      case TilesMemberState::Fresh:
        if (dropped_) {
          dropped_ = false;
          heldLogged_ = false;
          freshSinceMs_ = -1;
          return "recovered";
        }
        if (heldLogged_) {
          if (freshSinceMs_ < 0) {
            freshSinceMs_ = nowMs;
          } else if (nowMs - freshSinceMs_ >= kTilesHoldLogRearmMs) {
            heldLogged_ = false;
            freshSinceMs_ = -1;
          }
        }
        return nullptr;
    }
    return nullptr;
  }

  // True while a `held` line has been written for an episode that has not
  // been closed by a drop or a re-arm. The no-frame drop path uses it to say
  // "dropped reason=no-frame" for a member it had announced as held.
  [[nodiscard]] bool heldAnnounced() const { return heldLogged_ && !dropped_; }

 private:
  bool heldLogged_ = false;
  bool dropped_ = false;
  int64_t freshSinceMs_ = -1;
};

}  // namespace corevideo::compositor
