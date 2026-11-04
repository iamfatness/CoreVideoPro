#pragma once

#include <algorithm>
#include <cmath>

namespace corevideo::graphics {

struct GraphicsCrawlPlayback {
  double speed = 120;
  double passWidth = 0;
  bool loop = true;
};

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

  bool paused() const { return paused_; }
  double nextBoundary(double nowMs, const GraphicsCrawlPlayback& playback) const {
    const auto passMs = playback.passWidth * 1000. / playback.speed;
    return (std::floor(elapsed(nowMs) / passMs) + 1.) * passMs;
  }

  bool reached(double nowMs, double boundaryMs) const {
    return !paused_ && elapsed(nowMs) >= boundaryMs;
  }

  void adopt(double boundaryMs) { elapsedMs_ -= boundaryMs; }

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
