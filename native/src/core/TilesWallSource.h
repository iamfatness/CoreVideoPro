#pragma once

#include "compositor/TilesPlanAnimation.h"

#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace corevideo::core {

// One wall's animation, owned by the WALL rather than by a bus.
//
// Before this existed, MediaCore held programTilesAnimation_ and
// previewTilesAnimation_, and a Take handed settled state from one to the other
// (TilesPlanAnimation::adoptSettledFrom). That hand-off REFUSED a wall whose
// tiles were still flying - "mid-flight state belongs to the bus that is flying
// it" - because with two animators there is no correct answer. So a wall taken
// mid-animation re-animated on the cut, which is #448.
//
// With one animator there is nothing to hand over: both buses sample the same
// object, and a cut changes only which bus is looking at it.
class TilesWallSource final {
 public:
  // Wraps the animation so a reset can never happen without the generation
  // moving. TilesPlanAnimation::advance returns true when it reset the
  // animator; a plain bool return keeps this allocation-free on the render tick
  // (a std::function callback would not be).
  void advance(modules::CompositorRenderPlan& plan, const std::string& wallId, bool present,
               bool enabled, double durationMs, double nowMs) {
    if (animation_.advance(plan, wallId, present, enabled, durationMs, nowMs)) {
      noteReset();
    }
  }

  // Review round 4, Finding 1: release a wall that is present but not
  // animating, with no plan argument at all — see
  // TilesPlanAnimation::releaseIfIdle for why a caller must never substitute
  // a plan built for a DIFFERENT wall on this path. Same generation contract
  // as advance(): a reset (key_ was non-empty) bumps it, an already-idle wall
  // does not.
  void releaseIfIdle() {
    if (animation_.releaseIfIdle()) {
      noteReset();
    }
  }

  void applyLatest(modules::CompositorRenderPlan& plan, const std::string& wallId) const {
    animation_.applyLatest(plan, wallId);
  }

  // The take record's proof that nothing restarted (spec section 5), so
  // "did this wall restart?" is a number rather than an opinion.
  [[nodiscard]] uint64_t generation() const { return generation_; }
  void noteReset() { ++generation_; }

 private:
  compositor::TilesPlanAnimation animation_;
  uint64_t generation_ = 0;
};

// Wall id -> source. The id is the Tiles layer id, which the shell already
// emits as "tiles:<sceneId>" (TilesLayerPayloadBuilder.cs), so it is unique per
// scene and identical for the same gallery on either bus.
class TilesWallSources final {
 public:
  [[nodiscard]] TilesWallSource& forWall(const std::string& wallId) {
    return sources_[wallId];
  }

  // Lifetime is "referenced by a scene" (parent spec section 2). Anything no
  // live scene names is released; a recreated wall is a NEW wall and starts at
  // generation 0, never continuing a retired one's count.
  void releaseAllExcept(const std::vector<std::string>& liveWallIds) {
    for (auto it = sources_.begin(); it != sources_.end();) {
      bool live = false;
      for (const auto& id : liveWallIds) {
        if (it->first == id) { live = true; break; }
      }
      it = live ? std::next(it) : sources_.erase(it);
    }
  }

  // Const lookup for readers (the take record). An unknown wall has never
  // animated, so its caller reports generation 0 - which is true, not a guess.
  [[nodiscard]] const TilesWallSource* find(const std::string& wallId) const {
    const auto it = sources_.find(wallId);
    return it == sources_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] std::size_t size() const { return sources_.size(); }

 private:
  std::map<std::string, TilesWallSource> sources_;
};

}  // namespace corevideo::core
