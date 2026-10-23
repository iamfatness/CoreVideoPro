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
    std::atomic_store(&latest_, std::make_shared<const VideoFrame>(std::move(frame)));
  }
  std::optional<VideoFrame> acquire(const VideoFrame& selected) const {
    const auto latest = std::atomic_load(&latest_);
    if (!latest || !latest->gpuPixels || latest->participantId != selected.participantId ||
        latest->sourceEpoch != selected.sourceEpoch || latest->width != selected.width ||
        latest->height != selected.height || latest->frameId < selected.frameId) return {};
    return *latest;
  }
  void clear() {
    std::atomic_store(&latest_, std::shared_ptr<const VideoFrame>{});
  }
 private:
  // Atomic pointer exchange covers no copying, allocation or GPU work. Readers
  // retain a completed immutable publication while its successor is prepared.
  std::shared_ptr<const VideoFrame> latest_;
};
}
