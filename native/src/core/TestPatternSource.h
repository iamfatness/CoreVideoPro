#pragma once
#include "core/SourceBus.h"
#include "core/TestPattern.h"

namespace corevideo::core {

class TestPatternSource final : public ISource {
 public:
  explicit TestPatternSource(std::string sourceId = "test:pattern",
                             int width = 640, int height = 360)
      : bars_(makeSmpteBarsBgra(width, height)) {
    descriptor_.sourceId = std::move(sourceId);
    descriptor_.kind = "test";
    descriptor_.width = width;
    descriptor_.height = height;
    descriptor_.fpsNumerator = 60;
    descriptor_.fpsDenominator = 1;
    descriptor_.pixelFormat = "bgra";
    descriptor_.hasVideo = true;
  }

  const SourceDescriptor& descriptor() const override { return descriptor_; }

  SourceTick poll(int64_t programTime100ns) override {
    modules::VideoFrame frame;
    frame.participantId = descriptor_.sourceId;
    frame.width = frame.pixelWidth = frame.naturalWidth = descriptor_.width;
    frame.height = frame.pixelHeight = frame.naturalHeight = descriptor_.height;
    frame.pixelStride = descriptor_.width * 4;
    frame.pixels = bars_;
    frame.frameId = ++frameId_;
    frame.timestampMs = programTime100ns / 10000;
    counters_.framesIngested = static_cast<uint64_t>(frameId_);
    counters_.lastFrameId = frameId_;
    SourceTick tick;
    tick.video.push_back(std::move(frame));
    tick.health = SourceHealth::Producing;
    return tick;
  }

  SourceIngestCounters counters() const override { return counters_; }

 private:
  SourceDescriptor descriptor_;
  std::shared_ptr<const std::vector<uint8_t>> bars_;
  int64_t frameId_ = 0;
  SourceIngestCounters counters_;
};

}  // namespace corevideo::core
