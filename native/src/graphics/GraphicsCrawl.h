#pragma once

#include <algorithm>
#include <cmath>

namespace corevideo::graphics {

struct GraphicsCrawlSample {
  double distance = 0;
  bool complete = false;
};

// Samples only the caller's monotonic media ticks.
class GraphicsCrawlClock {
 public:
  void restart(double nowMs) {
    anchorMs_ = nowMs;
    elapsedMs_ = 0;
    paused_ = false;
  }

  void pause(double nowMs) {
    if (paused_) return;
    elapsedMs_ = elapsed(nowMs);
    paused_ = true;
  }

  void resume(double nowMs) {
    if (!paused_) return;
    anchorMs_ = nowMs;
    paused_ = false;
  }

  GraphicsCrawlSample sample(double nowMs, double speed, double passWidth, bool loop) const {
    const double distance = elapsed(nowMs) * speed / 1000.;
    if (loop) return {std::fmod(distance, passWidth), false};
    return {(std::min)(distance, passWidth), distance >= passWidth};
  }

 private:
  double elapsed(double nowMs) const {
    return elapsedMs_ + (paused_ ? 0. : (std::max)(0., nowMs - anchorMs_));
  }

  double anchorMs_ = 0;
  double elapsedMs_ = 0;
  bool paused_ = false;
};

inline double crawlItemX(double windowWidth, double itemStart, double itemWidth,
    double distance, bool rightward) {
  return rightward ? distance - itemStart - itemWidth : windowWidth + itemStart - distance;
}

} // namespace corevideo::graphics
