// native/src/core/CaptureDeviceSource.h
#pragma once
#include "core/SourceBus.h"

namespace corevideo::core {

// One capture device (UVC/camera, screen, browser host, SRT-ingest transport) on
// the source bus (#535 slice 2). The ADAPTER owns the frame: it keeps the last
// BGRA frame and re-emits it every tick while the device is connected, so this
// source only mirrors the frame MediaCore's capture poll handed it this tick.
// Keyed by the adapter's own "capture:<deviceId>" frame key — never re-keyed.
class CaptureDeviceSource final : public ISource {
 public:
  CaptureDeviceSource(std::string sourceId, int width, int height) {
    descriptor_.sourceId = std::move(sourceId);
    descriptor_.kind = "capture";
    descriptor_.width = width;
    descriptor_.height = height;
    descriptor_.pixelFormat = "bgra";
    descriptor_.hasVideo = true;
  }
  const SourceDescriptor& descriptor() const override { return descriptor_; }

  void setLatest(modules::VideoFrame frame) {
    latest_ = std::move(frame);  // shared_ptr payload: no pixel copy
    hasFrame_ = true;
    counters_.framesIngested += 1;
    counters_.lastFrameId = latest_.frameId;
  }

  SourceTick poll(int64_t) override {
    SourceTick tick;
    if (hasFrame_) {
      tick.video.push_back(latest_);
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
