#pragma once
#include "rpc/Json.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

namespace corevideo::core {
// Independent QA measurement present in BOTH trace-on and trace-off runs.
// It samples the existing work duration, never pixels, waits or a new clock on
// the render thread. This is not the delivery trace's production aggregation.
class RenderWorkDistribution {
 public:
  static constexpr std::size_t kBuckets = 32769;
  static constexpr std::int64_t kWidthNs = 1000;
  explicit RenderWorkDistribution(bool enabled = requested()) {
    if (enabled) bins_ = std::make_unique<Bins>();
  }
  void record(std::int64_t workNs) noexcept {
    if (!bins_) return;
    if (workNs < 0) { invalid_.fetch_add(1, std::memory_order_relaxed); return; }
    const auto bucket = static_cast<std::size_t>((std::min)(
        workNs / kWidthNs, static_cast<std::int64_t>(kBuckets - 1)));
    (*bins_)[bucket].fetch_add(1, std::memory_order_relaxed);
  }
  rpc::Json snapshot() const {
    rpc::Json::Object result{{"schemaVersion", "render-work-distribution-v1"}, {"enabled", bins_ != nullptr},
        {"scope", "Process-lifetime CPU render work only; opt-in independent QA collector, not GPU or output delivery."}};
    if (!bins_) return result; // Disabled peers have no invented zero distribution.
    const auto started = now();
    rpc::Json::Array values;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < kBuckets; ++i) {
      const auto count = (*bins_)[i].load(std::memory_order_relaxed);
      if (count) values.emplace_back(rpc::Json::Array{rpc::Json(static_cast<double>(i)), rpc::Json(static_cast<double>(count))});
      total += count;
    }
    const auto ended = now();
    result.emplace("bucketWidthNs", static_cast<double>(kWidthNs));
    result.emplace("bucketCapacity", static_cast<double>(kBuckets));
    result.emplace("overflowLowerBoundNs", static_cast<double>((kBuckets - 1) * kWidthNs));
    result.emplace("sampleCount", static_cast<double>(total));
    result.emplace("invalidSamples", static_cast<double>(invalid_.load(std::memory_order_relaxed)));
    result.emplace("scanStartedAtNs", std::to_string(started));
    result.emplace("scanEndedAtNs", std::to_string(ended));
    result.emplace("bins", std::move(values));
    return result;
  }
 private:
  using Bins = std::array<std::atomic<std::uint64_t>, kBuckets>;
  std::unique_ptr<Bins> bins_;
  std::atomic<std::uint64_t> invalid_{0};
  static std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  static bool requested() {
    const auto* value = std::getenv("COREVIDEO_QA_RENDER_WORK_DISTRIBUTION");
    return value && std::string(value) == "1";
  }
};
}
