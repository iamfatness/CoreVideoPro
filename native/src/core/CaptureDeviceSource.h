// native/src/core/CaptureDeviceSource.h
#pragma once
#include "core/SourceBus.h"
#include <atomic>

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
    descriptor_.hasVideo = width > 0 && height > 0;
  }
  const SourceDescriptor& descriptor() const override { return descriptor_; }

  void setLatest(modules::VideoFrame frame) {
    // The bus source owns this video incarnation. Format/pixel epochs may change
    // without changing the subject; clearVideo or a new source starts a new one.
    if (!hasFrame_) generation_ = ++nextGeneration_;
    frame.sourceGeneration = generation_;
    latest_ = std::move(frame);  // shared_ptr payload: no pixel copy
    hasFrame_ = true;
    tombstoneOrder_ = 0;
    descriptor_.hasVideo = true;
    descriptor_.width = latest_.pixelWidth > 0 ? latest_.pixelWidth : latest_.i420Width;
    descriptor_.height = latest_.pixelHeight > 0 ? latest_.pixelHeight : latest_.i420Height;
    if (latest_.hasGpuPixels()) {
      descriptor_.width = latest_.gpuPixels->width;
      descriptor_.height = latest_.gpuPixels->height;
    }
    // diagnostic only: counts setLatest calls, NOT the deduped per-frame
    // count the snapshot publishes (SourceBus::Entry::counters) — the
    // adapter re-emits the same held frame every tick while connected, so
    // this increments once per tick regardless of whether the frameId
    // actually advanced.
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

  void endVideo() {
    if (!hasFrame_) return;
    const auto liveness = latest_.captureLiveness;
    clearVideo();
    if (liveness.state != modules::CaptureLivenessState::Unreported) {
      latest_.captureLiveness = liveness;
      tombstoneOrder_ = ++nextTombstone_;
    }
  }

  std::uint64_t tombstoneOrder() const { return tombstoneOrder_; }

  void recordOnAirObservation(int64_t nowNs) {
    const auto& liveness = latest_.captureLiveness;
    if (liveness.state == modules::CaptureLivenessState::Unreported) {
      onAirObservation_.reset();
      return;
    }
    const auto decision = tombstoneOrder_ == 0 ? decideCaptureOnAir(liveness, nowNs)
        : CaptureOnAirDecision{CaptureOnAirPhase::Failed, SourceHealth::Failed, 0, true};
    onAirObservation_ = CaptureOnAirObservation{
        decision, (std::max)(int64_t{0}, nowNs - liveness.lastSampleNs), liveness.reason};
  }

  const std::optional<CaptureOnAirObservation>& onAirObservation() const { return onAirObservation_; }

  void clearVideo() {
    latest_ = {};
    hasFrame_ = false;
    tombstoneOrder_ = 0;
    onAirObservation_.reset();
    descriptor_.hasVideo = false;
    descriptor_.width = descriptor_.height = 0;
  }
  // Set when the device delivered this picture through deliverVideo. The
  // browser poll must not treat the source as gone just because this tick's
  // browser list does not name it.
  void holdVideo(bool hold) { videoHeld_ = hold; }
  bool videoHeld() const { return videoHeld_; }
  void prepareAudio(bool configured) {
    pendingAudio_.clear();
    descriptor_.hasAudio = configured;
  }
  void stageAudio(modules::AudioFrame frame) {
    descriptor_.hasAudio = true;
    pendingAudio_.push_back(std::move(frame));
  }
  std::vector<modules::AudioFrame> pollAudio(int64_t) override {
    std::vector<modules::AudioFrame> result;
    result.swap(pendingAudio_);
    return result;
  }

 private:
  inline static std::atomic<uint64_t> nextGeneration_{0};
  inline static std::atomic<uint64_t> nextTombstone_{0};
  std::uint64_t tombstoneOrder_ = 0;
  std::optional<CaptureOnAirObservation> onAirObservation_;
  uint64_t generation_ = 0;
  SourceDescriptor descriptor_;
  modules::VideoFrame latest_;
  bool hasFrame_ = false;
  bool videoHeld_ = false;
  SourceIngestCounters counters_;
  std::vector<modules::AudioFrame> pendingAudio_;
};

}  // namespace corevideo::core
