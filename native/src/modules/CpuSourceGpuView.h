#pragma once
#include "modules/GpuVideoFrame.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace corevideo::modules {
struct CpuSourceGpuDemand {
  std::atomic<int64_t> selectedFrameId{-1};
  std::atomic<int64_t> lastDemand100ns{0};
  std::atomic<bool> stopped{false};
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
  std::atomic<std::weak_ptr<const GpuVideoFrame>> ready;
  std::atomic<bool> consumed{false};

  std::shared_ptr<const GpuVideoFrame> acquire(bool select) {
    if (!demand || demand->stopped.load()) return {};
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
