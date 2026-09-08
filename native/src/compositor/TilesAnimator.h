#pragma once

#include "compositor/CompositorLayout.h"
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace corevideo::compositor {

struct TilesAnimationTarget { std::string id; LayerRect rect; };
struct AnimatedTile { std::string id; LayerRect rect; float alpha = 1; bool atRest = true; };

// Render-thread owned. Each bus/wall owns its own instance; repeated scene sync
// must not reset it. No timers, retained departing pixels, or per-frame clock drift.
class TilesAnimator {
 public:
  void reset() { states_.clear(); hasTime_ = false; adopted_ = false; }

  std::vector<AnimatedTile> sample(const std::vector<TilesAnimationTarget>& desired,
      double nowMs, bool enabled, double durationMs, double canvasWidth = 1920,
      double canvasHeight = 1080) {
    if (!std::isfinite(nowMs)) nowMs = hasTime_ ? lastMs_ : 0;
    if (!enabled) reset();
    const bool adoption = !adopted_;
    const double dt = hasTime_ ? std::max(0.0, nowMs - lastMs_) / 1000.0 : 0;
    lastMs_ = hasTime_ ? std::max(lastMs_, nowMs) : nowMs;
    hasTime_ = enabled;
    adopted_ = enabled; // An empty first frame is still an adoption.
    const double duration = std::clamp(std::isfinite(durationMs) ? durationMs : 350.0, 100.0, 2000.0) / 1000;
    const double width = std::isfinite(canvasWidth) ? std::max(1.0, canvasWidth) : 1920;
    const double height = std::isfinite(canvasHeight) ? std::max(1.0, canvasHeight) : 1080;
    const std::array<double, 4> pixels{width, height, width, height};
    std::unordered_set<std::string> live;
    std::vector<AnimatedTile> result;
    result.reserve(std::min<size_t>(desired.size(), 64));
    for (const auto& target : desired) {
      if (result.size() == 64) break;
      const std::array<double, 4> goal{target.rect.x, target.rect.y, target.rect.width, target.rect.height};
      if (target.id.empty() || !std::all_of(goal.begin(), goal.end(), [](double v) { return std::isfinite(v); }) ||
          goal[2] <= 0 || goal[3] <= 0 || !live.insert(target.id).second) continue;
      if (!enabled) { result.push_back({target.id, target.rect, 1, true}); continue; }
      auto [it, added] = states_.try_emplace(target.id);
      auto& state = it->second;
      if (added) {
        state.position = goal;
        state.entering = !adoption;
      } else {
        for (size_t i = 0; i < 4; ++i) advance(state.position[i], state.velocity[i], goal[i], duration, dt);
        if (state.entering) state.entryElapsed += dt;
      }
      const float alpha = state.entering ? static_cast<float>(std::clamp(state.entryElapsed / duration, 0.0, 1.0)) : 1.f;
      if (alpha == 1) state.entering = false;
      bool rest = !state.entering;
      for (size_t i = 0; i < 4; ++i)
        rest = rest && std::abs(state.position[i] - goal[i]) * pixels[i] < 0.05 &&
            std::abs(state.velocity[i]) * duration * pixels[i] < 0.05;
      LayerRect rect{static_cast<float>(state.position[0]), static_cast<float>(state.position[1]),
          static_cast<float>(state.position[2]), static_cast<float>(state.position[3])};
      if (rest) { state.position = goal; state.velocity = {}; rect = target.rect; }
      result.push_back({target.id, rect, alpha, rest});
    }
    for (auto it = states_.begin(); it != states_.end(); ) {
      if (!live.contains(it->first)) it = states_.erase(it); else ++it;
    }
    return result;
  }

 private:
  struct State {
    std::array<double, 4> position{}, velocity{};
    double entryElapsed = 0;
    bool entering = false;
  };
  static void advance(double& p, double& v, double target, double duration, double dt) {
    if (dt <= 0) return;
    const double omega = 6.6384 / duration;
    const double delta = p - target, decay = std::exp(-omega * dt), c = v + omega * delta;
    p = target + (delta + c * dt) * decay;
    v = (v - omega * c * dt) * decay;
  }
  std::unordered_map<std::string, State> states_;
  bool hasTime_ = false, adopted_ = false;
  double lastMs_ = 0;
};
} // namespace corevideo::compositor
