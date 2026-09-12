#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace corevideo::core {

// WHO DOES A FOLLOW-SPEAKER ROUTE SHOW THIS TICK? (#478, controller ruling N2)
//
// A follow-speaker (`active-speaker` mode) route shows the DIRECTED speaker —
// ZoomActiveSpeakerDirector's choice among the shell's sources. But the directed
// id alone is not safe to bind:
//   * the director keeps an incumbent for 60 s after they LEAVE, and their
//     subscription (so their frames) is retired the moment they go;
//   * an off-air change (a Preview re-cue, disarming ISO, a wall edit) can drop
//     the speaker from the sources mid-talk, which retires their video in the
//     same spine tick;
//   * Zoom reuses per-meeting user ids, so an id remembered from the LAST meeting
//     can name a different person in this one.
// Binding an id with no frame paints the compositor's colorFromParticipantId
// slab ON PROGRAM. So the binding is frame-validated: the directed speaker if
// they have a content frame THIS tick, else the most recent previously directed
// speaker who does, else NOBODY — the caller renders the layer empty (never a
// slab, never the positional fallback). The history is scoped to one meeting:
// the caller passes the engine's speaker epoch, which moves on join, leave,
// Engine off and every new engine process, and a moved epoch forgets it.
//
// Pure: no locks, no I/O; the caller owns it (MediaCore, under coreMutex).
class FollowSpeakerHold {
 public:
  static constexpr std::size_t kMaxRemembered = 8;

  // Record this tick's directed speaker ("" when none) under `epoch`.
  void observe(std::uint64_t epoch, const std::string& directedSpeakerId) {
    if (epoch != epoch_) {
      epoch_ = epoch;
      recent_.clear();
    }
    if (directedSpeakerId.empty()) {
      return;
    }
    recent_.erase(std::remove(recent_.begin(), recent_.end(), directedSpeakerId), recent_.end());
    recent_.insert(recent_.begin(), directedSpeakerId);
    if (recent_.size() > kMaxRemembered) {
      recent_.resize(kMaxRemembered);
    }
  }

  // The most recent directed speaker for whom `hasFrame(id)` is true, or "".
  template <typename HasFrame>
  [[nodiscard]] std::string pick(HasFrame&& hasFrame) const {
    for (const auto& id : recent_) {
      if (hasFrame(id)) {
        return id;
      }
    }
    return {};
  }

  void clear() {
    recent_.clear();
  }

  [[nodiscard]] const std::vector<std::string>& remembered() const { return recent_; }

 private:
  std::uint64_t epoch_ = 0;
  std::vector<std::string> recent_;
};

}  // namespace corevideo::core
