#pragma once
#include <chrono>
#include <cmath>

namespace corevideo::modules {
struct WebcamLoudnessOverlay {
  bool show = false;
  bool ready = false;
  double lufs = -120;
};
// Camera-owned copy of the existing Program short-term meter. No PCM processing.
struct WebcamLoudnessState {
  using Clock = std::chrono::steady_clock;
  double lufs = -120;
  bool ready = false;
  Clock::time_point updated{};
  void update(double value, bool completeWindow, Clock::time_point now = Clock::now()) {
    lufs = value; ready = completeWindow && std::isfinite(value); updated = now;
  }
  WebcamLoudnessOverlay read(Clock::time_point now = Clock::now()) const {
    return {true, ready && updated != Clock::time_point{} && now >= updated &&
        now - updated <= std::chrono::milliseconds(500), lufs};
  }
};
}
