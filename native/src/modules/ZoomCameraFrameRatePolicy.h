#pragma once

#include <algorithm>
#include <cmath>

namespace corevideo::modules {

// A local playout ceiling. Zoom's raw renderer can request resolution but has
// no per-subscription receive-fps control. Keep SDK arrival statistics separate.
struct ZoomCameraFrameRatePolicy {
  static int clamp(int fps) {
    return fps == 15 || fps == 24 || fps == 25 || fps == 30 ? fps : 60;
  }

  struct Budget {
    double tokens = 2.0;
    double lastAtMs = -1.0;

    bool accept(double atMs, int maximumFps) {
      const int limit = clamp(maximumFps);
      if (limit == 60) return true;  // preserve the existing unthrottled path
      if (!std::isfinite(atMs) || atMs < 0) return false;
      if (lastAtMs < 0 || atMs < lastAtMs) {
        lastAtMs = atMs;
        tokens = 2.0;
      } else {
        tokens = std::min(2.0, tokens + (atMs - lastAtMs) * limit / 1000.0);
        lastAtMs = atMs;
      }
      if (tokens < 1.0) return false;
      tokens -= 1.0;
      return true;
    }
  };
};

}  // namespace corevideo::modules
