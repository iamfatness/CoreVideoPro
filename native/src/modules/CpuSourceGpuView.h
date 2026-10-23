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
// Exchange an immutable weak descriptor; publication cannot make an already
// completed image temporarily unavailable to a racing compositor.
class CpuSourceGpuPublication {
 public:
  void store(std::weak_ptr<const GpuVideoFrame> image) {
    std::atomic_store(&image_, std::make_shared<const std::weak_ptr<const GpuVideoFrame>>(std::move(image)));
  }
  std::weak_ptr<const GpuVideoFrame> load() const {
    const auto image = std::atomic_load(&image_);
    return image ? *image : std::weak_ptr<const GpuVideoFrame>{};
  }
 private:
  std::shared_ptr<const std::weak_ptr<const GpuVideoFrame>> image_;
};
enum class CpuPreparationCapacity : uint8_t {
  None, Sources, PendingTokens, RetiringGenerations, ActiveGenerations, Quarantine, Residency
};
inline const char* cpuPreparationCapacityReason(CpuPreparationCapacity reason) {
  switch (reason) {
    case CpuPreparationCapacity::Sources: return "preparation-source-capacity";
    case CpuPreparationCapacity::PendingTokens: return "preparation-pending-token-capacity";
    case CpuPreparationCapacity::RetiringGenerations: return "preparation-retiring-generation-capacity";
    case CpuPreparationCapacity::ActiveGenerations: return "preparation-active-generation-capacity";
    case CpuPreparationCapacity::Quarantine: return "preparation-quarantine-capacity";
    case CpuPreparationCapacity::Residency: return "preparation-residency-capacity";
    default: return "preparation-pending";
  }
}
struct CpuSourceGpuDemand {
  // A format replacement may drain the same participant lifetime through CPU
  // playout. Selecting its successor is the explicit retirement boundary.
  std::weak_ptr<CpuSourceGpuDemand> predecessor;
  std::atomic<int64_t> selectedFrameId{-1};
  std::atomic<int64_t> lastDemand100ns{0};
  std::atomic<bool> stopped{false};
  std::atomic<bool> failed{false};
  std::atomic<CpuPreparationCapacity> capacity{CpuPreparationCapacity::None};
};

// CPU playout/trim/ISO descriptors retain this identity, not a GPU pool slot.
// Each completion publishes a NEW wrapper; a weak pointer to reusable mutable
// pool storage would let an old CPU descriptor resurrect different pixels.
struct CpuSourceGpuView {
  std::string sourceId;
  uint64_t sourceEpoch = 0;
  uint64_t continuityLifetime = 0;
  int64_t frameId = 0, captureTimestamp100ns = 0;
  int width = 0, height = 0;
  int cpuStride = 0; // BGRA upload stride; I420 retains its tightly packed planes
  std::weak_ptr<const std::vector<uint8_t>> cpu;
  std::shared_ptr<CpuSourceGpuDemand> demand;
  CpuSourceGpuPublication ready;
  std::atomic<bool> consumed{false};
  std::atomic<bool> completionPublished{false};
  std::atomic<bool> superseded{false};

  std::shared_ptr<const GpuVideoFrame> acquire(bool select) {
    if (!demand || demand->stopped.load() || demand->failed.load() || superseded.load()) return {};
    if (select) {
      if (auto old = demand->predecessor.lock()) old->stopped.store(true);
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
