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
    descriptor_.hasVideo = width > 0 && height > 0;
  }
  const SourceDescriptor& descriptor() const override { return descriptor_; }

  void setLatest(modules::VideoFrame frame) {
    latest_ = std::move(frame);
    hasFrame_ = true;
    descriptor_.hasVideo = true;
    descriptor_.width = latest_.i420Width > 0 ? latest_.i420Width
        : (latest_.pixelWidth > 0 ? latest_.pixelWidth : latest_.width);
    descriptor_.height = latest_.i420Height > 0 ? latest_.i420Height
        : (latest_.pixelHeight > 0 ? latest_.pixelHeight : latest_.height);
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

  void clearVideo() {
    latest_ = {};
    hasFrame_ = false;
    descriptor_.hasVideo = false;
    descriptor_.width = descriptor_.height = 0;
  }
  void clearAudio() {
    pendingAudio_.clear();
    descriptor_.hasAudio = false;
  }
  void stageAudio(modules::AudioFrame frame) {
    descriptor_.hasAudio = true;
    pendingAudio_.push_back(std::move(frame));
  }
  std::vector<modules::AudioFrame> pollAudio(int64_t) override {
    std::vector<modules::AudioFrame> frames;
    frames.swap(pendingAudio_);
    return frames;
  }

 private:
  SourceDescriptor descriptor_;
  modules::VideoFrame latest_;
  bool hasFrame_ = false;
  SourceIngestCounters counters_;
  std::vector<modules::AudioFrame> pendingAudio_;
};

}  // namespace corevideo::core
