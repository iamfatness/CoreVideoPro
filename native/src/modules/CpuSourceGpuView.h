#pragma once
#include "modules/GpuVideoFrame.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace corevideo::modules {
// libc++ does not implement atomic<weak_ptr> on every supported target.
// Publication has no GPU calls under this lock; Program never waits for it.
class CpuSourceGpuPublication {
 public:
  void store(std::weak_ptr<const GpuVideoFrame> image) {
    std::lock_guard<std::mutex> lock(mutex_);
    image_ = std::move(image);
  }
  std::weak_ptr<const GpuVideoFrame> load() const {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    return lock.owns_lock() ? image_ : std::weak_ptr<const GpuVideoFrame>{};
  }
 private:
  mutable std::mutex mutex_;
  std::weak_ptr<const GpuVideoFrame> image_;
};
struct CpuSourceGpuDemand {
  std::atomic<int64_t> selectedFrameId{-1};
  std::atomic<int64_t> lastDemand100ns{0};
  std::atomic<bool> stopped{false};
  std::atomic<bool> failed{false};
};

// CPU playout/trim/ISO descriptors retain this identity, not a GPU pool slot.
// Each completion publishes a NEW wrapper; a weak pointer to reusable mutable
// pool storage would let an old CPU descriptor resurrect different pixels.
struct CpuSourceGpuView {
  std::string sourceId;
  uint64_t sourceEpoch = 0;
  int64_t frameId = 0, captureTimestamp100ns = 0;
  int width = 0, height = 0;
  std::weak_ptr<const std::vector<uint8_t>> cpu;
  std::shared_ptr<CpuSourceGpuDemand> demand;
  CpuSourceGpuPublication ready;
  std::atomic<bool> consumed{false};

  std::shared_ptr<const GpuVideoFrame> acquire(bool select) {
    if (!demand || demand->stopped.load() || demand->failed.load()) return {};
    if (select) {
      auto previous = demand->selectedFrameId.load();
      while (previous < frameId && !demand->selectedFrameId.compare_exchange_weak(previous, frameId)) {}
      demand->lastDemand100ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count() / 100);
    }
    auto image = ready.load().lock();
    if (!image || image->sourceId != sourceId || image->sourceEpoch != sourceEpoch || image->sourceFrameId != frameId ||
        image->sourceCaptureTimestamp100ns != captureTimestamp100ns ||
        image->width != width || image->height != height) return {};
    return image;
  }
};
}
