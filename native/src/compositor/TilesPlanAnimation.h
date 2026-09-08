#pragma once
#include "compositor/TilesAnimator.h"
#include "modules/Interfaces.h"

namespace corevideo::compositor {
// Advance once on the actual render tick. Snapshot/prefetch callers build plans
// without touching this state; multiview only reads the latest sampled positions.
class TilesPlanAnimation {
 public:
  void reset() { animator_.reset(); key_.clear(); sampled_.clear(); }
  void advance(modules::CompositorRenderPlan& plan, const std::string& wallKey,
      bool present, bool enabled, double durationMs, double nowMs) {
    if (!present || !enabled) { reset(); return; }
    if (key_ != wallKey) { animator_.reset(); key_ = wallKey; }
    std::vector<TilesAnimationTarget> targets;
    for (const auto& layer : plan.layers) {
      if (layer.kind == "participant-video" && layer.layerId.rfind("tile:", 0) == 0)
        targets.push_back({layer.layerId.substr(5), {layer.rect.x, layer.rect.y, layer.rect.width, layer.rect.height}});
    }
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
