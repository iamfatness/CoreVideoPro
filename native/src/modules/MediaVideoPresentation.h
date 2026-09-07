#pragma once
#include "modules/Interfaces.h"
#include <deque>
#include <functional>

namespace corevideo::modules {
struct ScheduledMediaVideo {
  VideoFrame frame;
  int64_t due100ns = 0;
};
// Optional private decode capability: prefetch yields immutable pixels before
// they become due; only the render consumer chooses presentation time.
class IMediaVideoPrefetch {
 public:
  virtual ~IMediaVideoPrefetch() = default;
  virtual void setMediaWakeCallback(std::function<void()> callback) {}
  virtual std::vector<ScheduledMediaVideo> prefetchMediaVideo(
      const std::vector<CompositorRenderPlanLayer>& layers, int64_t nowMs) = 0;
};
class MediaVideoPresentation {
 public:
  bool hasRoom() const { return queued_.size() < 3; }
  void push(ScheduledMediaVideo sample) {
    if (!hasRoom() || !sample.frame.hasPixels()) return;
    if (hasIdentity_ && sample.frame.frameId == lastFrameId_) return;
    hasIdentity_ = true; lastFrameId_ = sample.frame.frameId;
    queued_.push_back(std::move(sample));
  }
  const VideoFrame& select(int64_t target100ns) {
    while (!queued_.empty() && queued_.front().due100ns <= target100ns) {
      current_ = std::move(queued_.front().frame); queued_.pop_front();
    }
    return current_;
  }
  size_t queued() const { return queued_.size(); }
 private:
  std::deque<ScheduledMediaVideo> queued_;
  VideoFrame current_;
  bool hasIdentity_ = false;
  int64_t lastFrameId_ = 0;
};
} // namespace corevideo::modules
