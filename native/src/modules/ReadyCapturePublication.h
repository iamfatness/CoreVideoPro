#pragma once
#include "modules/Interfaces.h"
#include <optional>

namespace corevideo::modules {
// A capture-owner publication of one completed GPU image. Unlike CPU arrivals,
// these pixels need no preparation interval. Program freezes the route first
// and acquires its image only when it is about to compose. No extra pool/queue.
class ReadyCapturePublication {
 public:
  void publish(VideoFrame frame) {
    if (!frame.gpuPixels || frame.gpuPixels->monitorPrivate || frame.sourceEpoch == 0 ||
        frame.frameId <= 0 || frame.captureTimestamp100ns <= 0 ||
        frame.gpuPixels->width != frame.width || frame.gpuPixels->height != frame.height) return;
    frame.readyCapture.reset(); frame.capturePresentation.reset();
    frame.pixels.reset(); frame.i420.reset(); frame.preparedGpu.reset(); frame.monitorGpuPixels.reset();
    VideoFrame retired;
    { std::lock_guard<std::mutex> lock(mutex_); retired = std::move(latest_); latest_ = std::move(frame); }
  }
  std::optional<VideoFrame> acquire(const VideoFrame& selected) const {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !latest_.gpuPixels || latest_.participantId != selected.participantId ||
        latest_.sourceEpoch != selected.sourceEpoch || latest_.width != selected.width ||
        latest_.height != selected.height || latest_.frameId < selected.frameId) return {};
    return latest_;
  }
  void clear() {
    VideoFrame retired;
    { std::lock_guard<std::mutex> lock(mutex_); retired = std::move(latest_); latest_ = {}; }
  }
 private:
  mutable std::mutex mutex_;
  VideoFrame latest_;
};
}
