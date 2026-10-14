#pragma once
#include <chrono>

namespace corevideo::core {
// Ephemeral UI consumer interest. A lost shell/unload cannot leave exports alive.
class SourcePreviewDemand {
 public:
  using Clock = std::chrono::steady_clock;
  void set(bool enabled, Clock::time_point now = Clock::now()) {
    expires_ = enabled ? now + std::chrono::seconds(3) : Clock::time_point{};
  }
  bool active(Clock::time_point now = Clock::now()) const { return now < expires_; }
 private:
  Clock::time_point expires_{};
};
}  // namespace corevideo::core
