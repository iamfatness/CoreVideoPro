#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

namespace corevideo::zoom {

// Limits unsolicited zoom-video-frame events to a sustainable preview rate
// (~10–15fps) so stdio JSON-RPC stays responsive during live meetings.
class FrameEmitThrottle {
 public:
  explicit FrameEmitThrottle(int minIntervalMs = 66) : minIntervalMs_(minIntervalMs) {}

  [[nodiscard]] int minIntervalMs() const { return minIntervalMs_; }

  [[nodiscard]] bool shouldEmit(
      const std::string& participantId,
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
    if (participantId.empty() || minIntervalMs_ <= 0) {
      return !participantId.empty();
    }

    const auto found = lastEmit_.find(participantId);
    if (found != lastEmit_.end()) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - found->second).count();
      if (elapsed < minIntervalMs_) {
        return false;
      }
    }

    lastEmit_[participantId] = now;
    return true;
  }

  void reset() { lastEmit_.clear(); }

 private:
  int minIntervalMs_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastEmit_;
};

}  // namespace corevideo::zoom