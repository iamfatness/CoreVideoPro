#pragma once
#include "modules/Interfaces.h"
#include <array>
#include <mutex>

namespace corevideo::modules {
// A ref-only recording branch before latest-picture monitor sampling. This leaf
// owns no pixels, conversion, encoder calls, or callbacks into the media core.
class CpuIsoArrivalQueue {
 public:
  static constexpr size_t kMaxFrames = 4, kMaxBytes = 32u * 1024u * 1024u;
  using Stats = CaptureDeviceInfo::CpuArrivalEvidence;
  void require(bool demanded) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.demanded = demanded;
    if (!demanded) {
      stats_.released += size_;
      for (auto& frame : pending_) frame = {};
      size_ = head_ = 0; stats_.bytes = 0;
    }
  }
  void offer(const VideoFrame& frame) {
    if (!frame.hasI420()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.observed;
    if (!stats_.demanded) return;
    const auto bytes = frame.i420->size();
    if (bytes > kMaxBytes) { ++stats_.overflowed; return; }
    while (size_ && (size_ >= kMaxFrames || stats_.bytes > kMaxBytes - bytes)) {
      stats_.bytes -= pending_[head_].i420->size(); pending_[head_] = {};
      head_ = (head_ + 1) % kMaxFrames; --size_; ++stats_.overflowed;
    }
    pending_[(head_ + size_) % kMaxFrames] = frame;
    ++size_; stats_.bytes += bytes; ++stats_.accepted;
  }
  std::vector<VideoFrame> take() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VideoFrame> result;
    result.reserve(size_);
    for (size_t i = 0; i < size_; ++i) result.push_back(std::move(pending_[(head_ + i) % kMaxFrames]));
    stats_.drained += result.size(); size_ = head_ = 0; stats_.bytes = 0;
    return result;
  }
  Stats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = stats_; result.pending = size_; return result;
  }
 private:
  mutable std::mutex mutex_;
  Stats stats_;
  std::array<VideoFrame, kMaxFrames> pending_;
  size_t head_ = 0, size_ = 0;
};
}
