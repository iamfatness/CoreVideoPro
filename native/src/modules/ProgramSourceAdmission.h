#pragma once
#include "modules/Interfaces.h"

namespace corevideo::modules {
// Runs after CPU playout/guest trim. Never changes the selected CPU descriptor
// or acquires a future image. One GPU-only last-valid descriptor per source;
// ISO/CPU pools are not retained. Optional passes do not touch this cache.
class ProgramSourceAdmissionPolicy {
 public:
  static constexpr size_t kMaxSources = 64;
  struct Result { VideoFrame image; modules::ProgramSourceAdmission evidence; };
  template<class CanRead> Result select(const VideoFrame& requested, int64_t tick, CanRead canRead) {
    for (auto it = held_.begin(); it != held_.end();)
      it = tick - it->second.tick > 300 ? held_.erase(it) : std::next(it);
    Result result;
    auto& proof = result.evidence;
    proof.sourceId = requested.participantId; proof.requestedEpoch = requested.sourceEpoch;
    proof.requestedFrameId = requested.frameId; proof.requestedCapture100ns = requested.captureTimestamp100ns;
    proof.state = "unavailable"; proof.reason = "preparation-unavailable";
    result.image = requested;
    result.image.pixels.reset(); result.image.i420.reset(); result.image.preparedGpu.reset();
    result.image.gpuPixels.reset(); result.image.monitorGpuPixels.reset();
    const int width = requested.hasI420() ? requested.i420Width : requested.hasPixels() ? requested.pixelWidth : requested.width;
    const int height = requested.hasI420() ? requested.i420Height : requested.hasPixels() ? requested.pixelHeight : requested.height;
    auto previous = held_.find(requested.participantId);
    if (previous != held_.end() && (requested.sourceEpoch == 0 ||
        previous->second.image.sourceEpoch != requested.sourceEpoch ||
        previous->second.image.gpuPixels->width != width || previous->second.image.gpuPixels->height != height)) {
      held_.erase(previous); previous = held_.end();
      proof.reason = "source-epoch-or-dimensions-changed";
    }
    auto gpu = requested.gpuPixels;
    if (!gpu && requested.preparedGpu) {
      const auto& token = requested.preparedGpu;
      if (token->sourceId == requested.participantId && token->sourceEpoch == requested.sourceEpoch &&
          token->frameId == requested.frameId && token->captureTimestamp100ns == requested.captureTimestamp100ns &&
          token->width == width && token->height == height) {
        gpu = token->acquire(true);
        proof.reason = token->demand && token->demand->stopped.load() ? "preparation-stopped" : "preparation-pending";
      } else proof.reason = "preparation-identity-mismatch";
    }
    if (proof.reason == "preparation-stopped") {
      if (previous != held_.end()) held_.erase(previous);
      return result; // A removed/failed producer cannot prove its held surface.
    }
    const bool identity = gpu && !gpu->monitorPrivate && gpu->width == width && gpu->height == height &&
        (gpu->sourceId.empty() || (gpu->sourceId == requested.participantId && gpu->sourceEpoch == requested.sourceEpoch &&
          gpu->sourceFrameId == requested.frameId && gpu->sourceCaptureTimestamp100ns == requested.captureTimestamp100ns));
    if (gpu && !identity) proof.reason = "gpu-identity-mismatch";
    if (identity && canRead(gpu)) {
      result.image.gpuPixels = gpu;
      proof.state = "ready"; proof.reason = "gpu-completed-admitted";
      if (requested.preparedGpu) requested.preparedGpu->consumed.store(true);
      if (!requested.participantId.empty() && requested.sourceEpoch != 0) {
        if (previous != held_.end()) previous->second = {result.image, tick};
        else if (held_.size() < kMaxSources) held_.emplace(requested.participantId, Held{result.image, tick});
      }
    } else {
      if (identity) proof.reason = "gpu-consumer-or-read-lease-unavailable";
      if (previous != held_.end() && previous->second.image.frameId <= requested.frameId &&
          previous->second.image.captureTimestamp100ns <= requested.captureTimestamp100ns && canRead(previous->second.image.gpuPixels)) {
        result.image = previous->second.image; previous->second.tick = tick; proof.state = "held";
      }
    }
    if (result.image.gpuPixels) {
      proof.actualEpoch = result.image.sourceEpoch; proof.actualFrameId = result.image.frameId;
      proof.actualCapture100ns = result.image.captureTimestamp100ns;
    }
    return result;
  }
 private:
  struct Held { VideoFrame image; int64_t tick = 0; };
  std::map<std::string, Held> held_;
};
}
