#pragma once
#include "modules/CpuSourceGpuView.h"
#include <functional>

namespace corevideo::modules {
// Actual decoded-arrival tap. Offers occur before source playout/guest trim;
// Program selects the same token later and resolves only its exact ready view.
class I420SourcePreparation {
 public:
  enum class Policy { PrepareArrivals, PrepareSelected };
  static constexpr size_t kMaxSources = 64, kMaxActive = 16, kPendingPerSource = 28;
  struct Stats { bool requested = false, supported = false;
    uint64_t prepared = 0, refused = 0, superseded = 0, failed = 0;
    size_t sources = 0, active = 0;
  };
  // Test-only resource/upload interleaving hooks; no command/settings/environment accepts them.
  explicit I420SourcePreparation(bool enabled, std::function<void(const std::string&)> beforeResources = {},
      std::function<void(const std::string&)> afterUpload = {}, Policy policy = Policy::PrepareArrivals);
  I420SourcePreparation(bool enabled, Policy policy) : I420SourcePreparation(enabled, {}, {}, policy) {}
  ~I420SourcePreparation();
  std::shared_ptr<CpuSourceGpuView> offer(const std::string& sourceId, uint64_t epoch, int64_t frameId,
      int64_t captureTimestamp100ns, int width, int height,
      const std::shared_ptr<const std::vector<uint8_t>>& cpu);
  Stats stats() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
