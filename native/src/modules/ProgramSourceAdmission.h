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
    for (auto it = pending_.begin(); it != pending_.end();)
      it = tick - it->second.tick > 300 ? pending_.erase(it) : std::next(it);
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
    auto pending = pending_.find(requested.participantId);
    if (pending != pending_.end() && (requested.sourceEpoch == 0 || pending->second.image.sourceEpoch != requested.sourceEpoch ||
        pending->second.token->width != width || pending->second.token->height != height ||
        !pending->second.token->demand || pending->second.token->demand->stopped.load() || pending->second.token->demand->failed.load() || pending->second.token->superseded.load())) {
      pending_.erase(pending); pending = pending_.end();
    }
    if (pending != pending_.end() && pending->second.token->completionPublished.load() && pending->second.token->ready.load().expired()) {
      pending_.erase(pending); pending = pending_.end();
    }
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
        if (token->demand && token->demand->failed.load()) proof.reason = "preparation-failed";
        if (token->superseded.load()) proof.reason = "preparation-superseded";
      } else proof.reason = "preparation-identity-mismatch";
    }
    if (proof.reason == "preparation-stopped" || proof.reason == "preparation-failed") {
      if (previous != held_.end()) held_.erase(previous);
      pending_.erase(requested.participantId);
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
      pending_.erase(requested.participantId);
      if (!requested.participantId.empty() && requested.sourceEpoch != 0) {
        if (previous != held_.end()) previous->second = {result.image, tick};
        else if (held_.size() < kMaxSources) held_.emplace(requested.participantId, Held{result.image, tick});
      }
    } else {
      if (identity) proof.reason = "gpu-consumer-or-read-lease-unavailable";
      if (pending != pending_.end()) {
        auto completed = pending->second.token->acquire(false);
        if (completed && !completed->monitorPrivate && pending->second.image.frameId <= requested.frameId &&
            pending->second.image.captureTimestamp100ns <= requested.captureTimestamp100ns && canRead(completed)) {
          result.image = pending->second.image; result.image.gpuPixels = std::move(completed);
          pending->second.token->consumed.store(true);
          proof.state = "held"; proof.reason = "previous-selection-completed";
          if (previous != held_.end()) previous->second = {result.image, tick};
          else if (held_.size() < kMaxSources) held_.emplace(requested.participantId, Held{result.image, tick});
          pending_.erase(pending); pending = pending_.end();
        }
      }
      // Keep one selected identity until its asynchronous completion is read.
      // The token holds weak CPU/GPU payloads; this never retains CPU/ISO pixels.
      if (pending == pending_.end() && requested.preparedGpu && !requested.preparedGpu->superseded.load() && requested.sourceEpoch != 0 &&
          requested.preparedGpu->sourceId == requested.participantId && requested.preparedGpu->sourceEpoch == requested.sourceEpoch &&
          requested.preparedGpu->frameId == requested.frameId && requested.preparedGpu->captureTimestamp100ns == requested.captureTimestamp100ns &&
          requested.preparedGpu->width == width && requested.preparedGpu->height == height && pending_.size() < kMaxSources) {
        auto descriptor = requested; descriptor.pixels.reset(); descriptor.i420.reset(); descriptor.gpuPixels.reset();
        descriptor.monitorGpuPixels.reset(); descriptor.preparedGpu.reset();
        pending_.emplace(requested.participantId, Pending{std::move(descriptor), requested.preparedGpu, tick});
      } else if (pending != pending_.end()) pending->second.tick = tick;
      if (proof.state != "held" && previous != held_.end() && previous->second.image.frameId <= requested.frameId &&
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
  struct Pending { VideoFrame image; std::shared_ptr<CpuSourceGpuView> token; int64_t tick = 0; };
  std::map<std::string, Held> held_;
  std::map<std::string, Pending> pending_;
};
}
