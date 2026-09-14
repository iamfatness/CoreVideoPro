#pragma once
#include "compositor/TilesAnimator.h"
#include "modules/Interfaces.h"

namespace corevideo::compositor {
// Advance once on the actual render tick. Snapshot/prefetch callers build plans
// without touching this state; multiview only reads the latest sampled positions.
class TilesPlanAnimation {
 public:
  void reset() { animator_.reset(); key_.clear(); sampled_.clear(); }

  // Release a wall that is present but not animating (or not present at all).
  // Plan-free BY DESIGN (review round 4, Finding 1): the caller has no real
  // plan to give this wall on this path, and passing a FOREIGN one (e.g. the
  // program plan, on behalf of a preview wall that shares its object) would
  // only be safe as long as advance() returns before ever touching `plan` —
  // an invariant that lives in a different file from the call site depending
  // on it, and silently breaks into on-air geometry corruption the moment
  // advance() is reordered or gains code above its early return. Removing the
  // hazard is cheaper than documenting it: this takes no plan and cannot ever
  // read one. Idempotent, matching advance()'s early-return semantics
  // exactly: an ALREADY-released wall (key_ empty) reports it did NOT reset —
  // without this a caller that releases every tick regardless of presence
  // would read "reset" forever, turning the generation into a tick counter
  // instead of a restart signal.
  bool releaseIfIdle() { const bool had = !key_.empty(); reset(); return had; }

  // Returns true when this call RESET the animator (a departure/disable, or a
  // different wall key arriving) - the caller's only truthful signal of "did
  // this wall restart", with no std::function/allocation on the render tick.
  // `plan` is read ONLY on this present-and-enabled path (target extraction +
  // applyLatest at the end) — a caller with no real plan for this wall must
  // use releaseIfIdle() above instead of passing one in, never a plan built
  // for a DIFFERENT wall.
  bool advance(modules::CompositorRenderPlan& plan, const std::string& wallKey,
      bool present, bool enabled, double durationMs, double nowMs) {
    if (!present || !enabled) return releaseIfIdle();
    // A DIFFERENT wall never inherits this one's geometry. (sampled_ is cleared
    // too: the all-stale guard below would otherwise let a new wall's first
    // frames be drawn at the previous wall's tile rects.)
    bool didReset = false;
    if (key_ != wallKey) { animator_.reset(); key_ = wallKey; sampled_.clear(); didReset = true; }
    std::vector<TilesAnimationTarget> targets;
    for (const auto& layer : plan.layers) {
      if (layer.kind == "participant-video" && layer.layerId.rfind("tile:", 0) == 0)
        targets.push_back({layer.layerId.substr(5), {layer.rect.x, layer.rect.y, layer.rect.width, layer.rect.height}});
    }
    // An all-stale tick is a TRANSIENT, not a departure. A wall that is still
    // present but momentarily has no admitted member (every frame aged out for
    // a beat — see kTilesStaleFrameMs; CLAUDE.md notes this happens on a Tiles
    // take before first frames land) must not have its retained tiles wiped:
    // sampling an empty target set erases every state AND consumes the
    // animator's adoption, so the instant frames return the ENTIRE wall
    // replays its entrance from alpha 0 — exactly the "redraw in PGM" this
    // fix exists to remove. Only a wall that has actually drawn tiles
    // preserves them; a cold wall's first tick is untouched, so a genuinely
    // new wall behaves exactly as it always has.
    if (targets.empty() && !sampled_.empty()) return didReset;
    sampled_ = animator_.sample(targets, nowMs, enabled, durationMs, plan.width, plan.height);
    applyLatest(plan, wallKey);
    return didReset;
  }
  void applyLatest(modules::CompositorRenderPlan& plan, const std::string& wallKey) const {
    if (key_ != wallKey) return;
    for (auto& layer : plan.layers) {
      const bool video = layer.kind == "participant-video" && layer.layerId.rfind("tile:", 0) == 0;
      const bool glow = layer.kind == "tiles-glow" && layer.layerId.rfind("tiles-glow:", 0) == 0;
      if (!video && !glow) continue;
      const auto id = layer.layerId.substr(video ? 5 : 11);
      auto it = std::find_if(sampled_.begin(), sampled_.end(), [&](const auto& tile) { return tile.id == id; });
      if (it == sampled_.end()) continue;
      layer.rect = {it->rect.x, it->rect.y, it->rect.width, it->rect.height};
      layer.opacity *= it->alpha;
    }
  }
 private:
  TilesAnimator animator_;
  std::string key_;
  std::vector<AnimatedTile> sampled_;
};
}
