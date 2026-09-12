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
  // Carries a play/pause change to the decoder's playback clock at `nowMs`
  // without reading anything. The owned worker calls it on every transition,
  // so a paused clip's clock freezes (and resumes) at a known instant even
  // though nothing is prefetched while it is paused.
  virtual void syncMediaClock(const std::vector<CompositorRenderPlanLayer>& layers, int64_t nowMs) {}
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
  // A paused clip: keep showing the frame that is on air and do not advance,
  // whatever the queued due times say. With nothing on air yet (a cue poster),
  // the first prepared frame is shown and then held.
  const VideoFrame& hold() {
    if (!current_.hasPixels() && !queued_.empty()) { current_ = std::move(queued_.front().frame); queued_.pop_front(); }
    return current_;
  }
  // Resume: frames prepared before the pause were scheduled against the old
  // epoch. Re-time them by the paused duration (the same shift the playback
  // clock applies to its epoch) so the next image is the clip's next frame,
  // on time. Dropping them instead would skip up to a queue's worth of frames
  // on every resume, because the reader has already moved past them.
  // A cue hand-over (T1.11 / #449): this decoder is being re-keyed onto a new
  // playback identity, so its clock is about to be replaced and the frames
  // queued against the old epoch can never come due — keeping them would freeze
  // the clip on its poster forever. Drop them; KEEP `current_`, because that
  // held poster is exactly what stops Program showing a placeholder while the
  // decoder refills from the running clock. `hasIdentity_` is cleared too: the
  // refilled frames may restart their ids on the new identity, and the dedup
  // must not mistake the first of them for a repeat of the last old one.
  void dropQueued() { queued_.clear(); hasIdentity_ = false; }
  void shift(int64_t delta100ns) { for (auto& sample : queued_) sample.due100ns += delta100ns; }
  const VideoFrame& current() const { return current_; }
  bool hasFrame() const { return current_.hasPixels() || !queued_.empty(); }
  size_t queued() const { return queued_.size(); }
 private:
  std::deque<ScheduledMediaVideo> queued_;
  VideoFrame current_;
  bool hasIdentity_ = false;
  int64_t lastFrameId_ = 0;
};
} // namespace corevideo::modules
