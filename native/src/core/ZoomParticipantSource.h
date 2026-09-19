#pragma once
#include "core/SourceBus.h"

namespace corevideo::core {

class ZoomParticipantSource final : public ISource {
 public:
  ZoomParticipantSource(std::string participantId, int width, int height) {
    descriptor_.sourceId = std::move(participantId);
    descriptor_.kind = "zoom";
    descriptor_.width = width;
    descriptor_.height = height;
    descriptor_.pixelFormat = "i420";
    descriptor_.hasVideo = true;
  }
  const SourceDescriptor& descriptor() const override { return descriptor_; }

  void setLatest(modules::VideoFrame frame) {
    latest_ = std::move(frame);
    hasFrame_ = true;
    // diagnostic only: counts setLatest calls, NOT the deduped per-frame
    // count the snapshot publishes (SourceBus::Entry::counters) — a tick
    // that re-delivers the same held frame still increments this.
    counters_.framesIngested += 1;
    counters_.lastFrameId = latest_.frameId;
  }

  SourceTick poll(int64_t) override {
    SourceTick tick;
    if (hasFrame_) {
      tick.video.push_back(latest_);           // shared_ptr payload: cheap copy
      tick.health = SourceHealth::Producing;
    } else {
      tick.health = SourceHealth::Warming;
    }
    return tick;
  }
  SourceIngestCounters counters() const override { return counters_; }

 private:
  SourceDescriptor descriptor_;
  modules::VideoFrame latest_;
  bool hasFrame_ = false;
  SourceIngestCounters counters_;
};

}  // namespace corevideo::core
