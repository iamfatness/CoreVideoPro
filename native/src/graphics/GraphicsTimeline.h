#pragma once

#include "rpc/Json.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace corevideo::graphics {
// Reference-space motion, applied to the resolved layer rectangle rather than
// its raster dimensions. Animation never changes a content preparation key.
struct GraphicsTransition {
  std::string type = "none", direction = "left", easing = "linear";
  double durationMs = 0;
  bool operator==(const GraphicsTransition&) const = default;
  static bool read(const rpc::Json& value, GraphicsTransition& output) {
    if (!value.isObject()) return false;
    GraphicsTransition next;
    for (const auto& [key, item] : value.asObject()) {
      if (key == "durationMs") {
        if (!item.isNumber() || !std::isfinite(item.asNumber()) || item.asNumber() < 0 || item.asNumber() > 3000) return false;
        next.durationMs = item.asNumber();
      } else {
        if (!item.isString()) return false;
        if (key == "type") next.type = item.asString();
        else if (key == "direction") next.direction = item.asString();
        else if (key == "easing") next.easing = item.asString();
        else return false;
      }
    }
    if (next.type != "none" && next.type != "fade" && next.type != "slide") return false;
    if (next.direction != "left" && next.direction != "right" && next.direction != "up" && next.direction != "down") return false;
    if (next.easing != "linear" && next.easing != "ease-in" && next.easing != "ease-out" && next.easing != "ease-in-out") return false;
    output = std::move(next);
    return true;
  }
  rpc::Json json() const {
    return rpc::Json::Object{{"type", type}, {"direction", direction}, {"durationMs", durationMs}, {"easing", easing}};
  }
};

class GraphicsTimeline {
 public:
  struct Sample { double alpha = 0, x = 0, y = 0; };
  Sample sample(double nowMs) const {
    const double t = durationMs_ == 0 ? 1 : std::clamp((nowMs - startedMs_) / durationMs_, 0., 1.);
    const double p = easing_ == "ease-in" ? t * t : easing_ == "ease-out" ? t * (2 - t) :
        easing_ == "ease-in-out" ? t * t * (3 - 2 * t) : t;
    return {from_.alpha + (to_.alpha - from_.alpha) * p,
            from_.x + (to_.x - from_.x) * p, from_.y + (to_.y - from_.y) * p};
  }
  bool settled(double nowMs) const { return durationMs_ == 0 || nowMs - startedMs_ >= durationMs_; }
  std::string phase(double nowMs) const { return settled(nowMs) ? (shown_ ? "on-air" : "hidden") : (shown_ ? "entering" : "exiting"); }
  bool targetingVisible() const { return shown_; }
  void show(const GraphicsTransition& transition, double nowMs) {
    if (shown_) return; // duplicate/repeated Show does not restart an entrance
    const auto current = phase(nowMs) == "hidden" ? hidden(transition) : sample(nowMs);
    begin(current, {1, 0, 0}, transition, nowMs); shown_ = true;
  }
  void hide(const GraphicsTransition& transition, double nowMs) {
    if (!shown_) return;
    begin(sample(nowMs), hidden(transition), transition, nowMs); shown_ = false;
  }
  void clear() { from_ = to_ = {}; durationMs_ = 0; shown_ = false; }
 private:
  static Sample hidden(const GraphicsTransition& transition) {
    Sample value;
    if (transition.type == "slide") {
      value.x = transition.direction == "left" ? -1 : transition.direction == "right" ? 1 : 0;
      value.y = transition.direction == "up" ? -1 : transition.direction == "down" ? 1 : 0;
    }
    return value;
  }
  void begin(Sample from, Sample to, const GraphicsTransition& transition, double nowMs) {
    from_ = from; to_ = to; startedMs_ = nowMs;
    durationMs_ = transition.type == "none" ? 0 : transition.durationMs;
    easing_ = transition.easing;
  }
  Sample from_, to_;
  double startedMs_ = 0, durationMs_ = 0;
  std::string easing_ = "linear";
  bool shown_ = false;
};
} // namespace corevideo::graphics
