#pragma once
#include "modules/I420SourcePreparation.h"
#include "modules/Interfaces.h"
#include <atomic>
#include <chrono>

namespace corevideo::modules {
// Capture-thread identity and preparation tap. Polling a held picture copies
// the resulting descriptor; it never offers the same arrival a second time.
class I420CaptureArrival {
 public:
  I420CaptureArrival(std::string sourceId, std::shared_ptr<I420SourcePreparation> preparation)
      : sourceId_(std::move(sourceId)), preparation_(std::move(preparation)), epoch_(nextEpoch_.fetch_add(1)) {}
  VideoFrame publish(std::shared_ptr<const std::vector<uint8_t>> cpu,
      int width, int height, bool fullRange, bool bt601) {
    VideoFrame frame;
    if (!cpu || width <= 0 || height <= 0 || (width & 1) || (height & 1) ||
        width > 7680 || height > 4320 || cpu->size() < static_cast<size_t>(width) * height * 3 / 2) return frame;
    if (width_ && (width_ != width || height_ != height)) epoch_ = nextEpoch_.fetch_add(1);
    width_ = width; height_ = height;
    frame.participantId = sourceId_;
    frame.width = frame.naturalWidth = frame.i420Width = width;
    frame.height = frame.naturalHeight = frame.i420Height = height;
    frame.frameId = ++frameId_; frame.sourceEpoch = epoch_;
    frame.captureTimestamp100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
    frame.timestampMs = frame.captureTimestamp100ns / 10'000;
    frame.i420 = std::move(cpu); frame.i420FullRange = fullRange; frame.i420Bt601 = bt601;
    if (preparation_) frame.preparedGpu = preparation_->offer(sourceId_, epoch_, frame.frameId,
        frame.captureTimestamp100ns, width, height, frame.i420);
    return frame;
  }
 private:
  std::string sourceId_;
  std::shared_ptr<I420SourcePreparation> preparation_;
  inline static std::atomic<uint64_t> nextEpoch_{1};
  uint64_t epoch_;
  int64_t frameId_ = 0;
  int width_ = 0, height_ = 0;
};
}
