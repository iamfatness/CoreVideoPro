#pragma once
#include "core/SourceBus.h"

namespace corevideo::core {

// The no-meeting slate. Two metadata tiles, regenerated each poll, with the
// same ids and timestamps the old SyntheticZoomCaptureSource returned. The
// render tick skips this source while a Zoom engine is configured; live
// participant frames stay on their own ZoomParticipantSource entries.
class SyntheticZoomSlateSource final : public ISource {
 public:
  SyntheticZoomSlateSource() {
    descriptor_.sourceId = "zoom-slate";
    descriptor_.kind = "zoom-slate";
    descriptor_.width = 1280;
    descriptor_.height = 720;
    descriptor_.hasVideo = true;
  }

  const SourceDescriptor& descriptor() const override { return descriptor_; }

  SourceTick poll(int64_t) override {
    ++frameNumber_;
    SourceTick tick;
    tick.health = SourceHealth::Producing;
    for (const char* id : {"synthetic-speaker-1", "synthetic-speaker-2"}) {
      modules::VideoFrame frame;
      frame.participantId = id;
      frame.width = 1280;
      frame.height = 720;
      frame.naturalWidth = 1280;
      frame.naturalHeight = 720;
      frame.timestampMs = frameNumber_ * 16;
      tick.video.push_back(std::move(frame));
    }
    return tick;
  }

  SourceIngestCounters counters() const override { return {}; }

 private:
  SourceDescriptor descriptor_;
  int64_t frameNumber_ = 0;
};

}  // namespace corevideo::core
