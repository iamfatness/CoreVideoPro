#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include "modules/Interfaces.h"

namespace corevideo::core {
// Independent of Take and scene loading: a Take while black stays black.
// Render-tick time, no sleeps or CPU pixel copies. Audio/ISO are untouched.
class ProgramBlackout {
 public:
  bool enabled() const { return enabled_; }
  float opacity() const { return opacity_; }
  void set(bool enabled, double durationMs = 300) {
    if (enabled == enabled_) return; // idempotent remote retries
    enabled_ = enabled;
    start_ = opacity_;
    elapsed_ = 0;
    duration_ = std::isfinite(durationMs) ? std::clamp(durationMs, 0.0, 5000.0) : 300;
    if (duration_ == 0) opacity_ = enabled_ ? 1.f : 0.f;
  }
  void advance(double ms) {
    if (!std::isfinite(ms) || ms < 0) return;
    elapsed_ = (std::min)(duration_, elapsed_ + ms);
    const float t = duration_ > 0 ? static_cast<float>(elapsed_ / duration_) : 1.f;
    opacity_ = start_ + ((enabled_ ? 1.f : 0.f) - start_) * t;
  }
  void append(modules::CompositorRenderPlan& plan) const {
    if (opacity_ <= 0) return;
    modules::CompositorRenderPlanLayer black;
    black.layerId = "program-fade-to-black";
    black.kind = "tiles-background";
    black.order = (std::numeric_limits<int>::max)();
    black.rect = {0.f, 0.f, 1.f, 1.f};
    black.hasFillColor = true;
    black.fillColor = "#000000";
    black.hasColorGrade = true; // neutral grade, even when Program lifts shadows
    black.opacity = opacity_;
    plan.layers.push_back(std::move(black));
  }
 private:
  bool enabled_ = false;
  float opacity_ = 0, start_ = 0;
  double elapsed_ = 0, duration_ = 300;
};
}
