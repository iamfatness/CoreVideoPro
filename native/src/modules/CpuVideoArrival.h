#pragma once
#include "modules/CpuSourcePreparation.h"
#include "modules/Interfaces.h"

namespace corevideo::modules {
// Producer-thread tap for immutable CPU arrivals. Playback timestamps and CPU
// payloads stay authoritative; held polls reuse the original observation/token.
class CpuVideoArrival {
 public:
  CpuVideoArrival(std::string sourceId, std::shared_ptr<CpuSourcePreparation> preparation)
      : sourceId_(std::move(sourceId)), preparation_(std::move(preparation)), epoch_(nextEpoch_.fetch_add(1)) {}
  void prepare(VideoFrame& frame) {
    if (!preparation_ || frame.gpuPixels || (!frame.hasI420() && !frame.hasPixels())) return;
    const bool bgra = !frame.hasI420();
    const int width = bgra ? frame.pixelWidth : frame.i420Width;
    const int height = bgra ? frame.pixelHeight : frame.i420Height;
    const auto& cpu = bgra ? frame.pixels : frame.i420;
    const bool same = seen_ && frame.frameId == frameId_ && width == width_ && height == height_ &&
        bgra == bgra_ && (!bgra || frame.pixelStride == stride_) && cpu_.lock() == cpu;
    if (!same) {
      if (seen_ && (frame.frameId <= frameId_ || width != width_ || height != height_ || bgra != bgra_))
        epoch_ = nextEpoch_.fetch_add(1);
      seen_ = true; frameId_ = frame.frameId; width_ = width; height_ = height; bgra_ = bgra; stride_ = frame.pixelStride;
      cpu_ = cpu;
      observed100ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
      token_.reset();
    }
    if (!token_ || (token_->demand && token_->demand->stopped.load())) {
      token_ = bgra ? preparation_->offerBgra(sourceId_, epoch_, frameId_, observed100ns_, width, height, frame.pixelStride, cpu)
                    : preparation_->offer(sourceId_, epoch_, frameId_, observed100ns_, width, height, cpu);
    }
    frame.participantId = sourceId_; frame.sourceEpoch = epoch_;
    frame.captureTimestamp100ns = observed100ns_; frame.preparedGpu = token_;
  }
  // A held or queued playback frame may outlive idle GPU retirement. Refresh
  // that exact descriptor on the decoder worker without advancing the arrival
  // tap or altering playback order, CPU bytes, epoch or observation time.
  void refreshStopped(VideoFrame& frame) {
    if (!preparation_ || !frame.preparedGpu || !frame.preparedGpu->demand ||
        !frame.preparedGpu->demand->stopped.load()) return;
    auto token = frame.hasI420()
        ? preparation_->offer(sourceId_, frame.sourceEpoch, frame.frameId,
            frame.captureTimestamp100ns, frame.i420Width, frame.i420Height, frame.i420)
        : preparation_->offerBgra(sourceId_, frame.sourceEpoch, frame.frameId,
            frame.captureTimestamp100ns, frame.pixelWidth, frame.pixelHeight, frame.pixelStride, frame.pixels);
    if (token) {
      if (frame.preparedGpu == token_) token_ = token;
      frame.preparedGpu = std::move(token);
    }
  }
 private:
  inline static std::atomic<uint64_t> nextEpoch_{1};
  std::string sourceId_;
  std::shared_ptr<CpuSourcePreparation> preparation_;
  uint64_t epoch_;
  bool seen_ = false, bgra_ = false;
  int width_ = 0, height_ = 0, stride_ = 0;
  int64_t frameId_ = -1, observed100ns_ = 0;
  std::weak_ptr<const std::vector<uint8_t>> cpu_;
  std::shared_ptr<CpuSourceGpuView> token_;
};
}
