#pragma once
#include "compositor/TilesAnimator.h"
#include "modules/Interfaces.h"

namespace corevideo::compositor {
// Advance once on the actual render tick. Snapshot/prefetch callers build plans
// without touching this state; multiview only reads the latest sampled positions.
class TilesPlanAnimation {
 public:
  void reset() { animator_.reset(); key_.clear(); sampled_.clear(); }

  // Carry a SETTLED wall from one bus to the other (live-show defect, owner
  // report 2026-09-09: "I can't have a total rerender from what is in preview
  // to program like it is loading for the first time").
  //
  // The wall key is sceneId + ":" + layerId and the layer id is derived from
  // the scene id, so the gallery sitting settled in PREVIEW and the same
  // gallery a Take puts on PROGRAM carry the IDENTICAL key — it is one wall
  // continuing on another bus, not a new one. Without this, the program
  // animation saw a key it had never held, reset its animator, and threw away
  // spring positions and entry alpha that were fully settled an instant
  // earlier on the other bus.
  //
  // Scoped so the two buses can never contaminate each other:
  //   * only on an EXACT key match (a different wall, or a wall the other bus
  //     never held, is refused and animates exactly as it does today),
  //   * only when the other bus's wall is SETTLED (every sampled tile atRest —
  //     mid-flight state belongs to the bus that is flying it),
  //   * the state is MOVED, and the source is reset — never aliased, so the
  //     next wall cued on the source bus starts clean.
  // Returns true if the state was carried across.
  bool adoptSettledFrom(TilesPlanAnimation& previous, const std::string& wallKey) {
    if (wallKey.empty() || key_ == wallKey) return false;
    if (previous.key_ != wallKey || previous.sampled_.empty()) return false;
    for (const auto& tile : previous.sampled_) {
      if (!tile.atRest) return false;
    }
    animator_ = std::move(previous.animator_);
    sampled_ = std::move(previous.sampled_);
    key_ = wallKey;
    previous.reset();
    return true;
  }

  void advance(modules::CompositorRenderPlan& plan, const std::string& wallKey,
      bool present, bool enabled, double durationMs, double nowMs) {
    if (!present || !enabled) { reset(); return; }
    // A DIFFERENT wall never inherits this one's geometry. (sampled_ is cleared
    // too: the all-stale guard below would otherwise let a new wall's first
    // frames be drawn at the previous wall's tile rects.)
    if (key_ != wallKey) { animator_.reset(); key_ = wallKey; sampled_.clear(); }
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
    if (targets.empty() && !sampled_.empty()) return;
    sampled_ = animator_.sample(targets, nowMs, enabled, durationMs, plan.width, plan.height);
    applyLatest(plan, wallKey);
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
