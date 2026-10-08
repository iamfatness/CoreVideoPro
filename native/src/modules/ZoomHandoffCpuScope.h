#pragma once
#include "core/BoundedAsyncLog.h"
#include <chrono>
#include <cstdint>

namespace corevideo::modules {
// Opt-in CPU wall-time attribution, including scheduler preemption. A slow
// mutex interval alone does not identify its owner or prove driver/GPU work.
class ZoomHandoffCpuScope {
 public:
  ZoomHandoffCpuScope(bool enabled, const char* operation) noexcept
      : enabled_(enabled), operation_(operation) {
    if (enabled_) started_ = now();
  }
  void acquired() noexcept { if (enabled_) acquired_ = now(); }
  std::int64_t thumbnailStarted() const noexcept { return enabled_ ? now() : 0; }
  void thumbnailFinished(std::int64_t started) noexcept {
    if (enabled_) { thumbnailNs_ += now() - started; ++thumbnails_; }
  }
  ~ZoomHandoffCpuScope() {
    if (!enabled_) return;
    const auto ended = now();
    if (ended - started_ < 8000000) return;
    ::corevideo::core::nativeLogf(
        "[zoom-handoff-cpu-v1] operation=%s started_at_ns=%lld acquired_at_ns=%lld ended_at_ns=%lld lock_observed=%u wait_ns=%lld body_ns=%lld thumbnail_encode_ns=%lld thumbnails=%llu total_ns=%lld clock=steady-nanoseconds owner_cause_verified=0\n",
        operation_, static_cast<long long>(started_), static_cast<long long>(acquired_),
        static_cast<long long>(ended), acquired_ != 0 ? 1u : 0u,
        static_cast<long long>((acquired_ ? acquired_ : ended) - started_),
        static_cast<long long>(acquired_ ? ended - acquired_ : 0),
        static_cast<long long>(thumbnailNs_), static_cast<unsigned long long>(thumbnails_),
        static_cast<long long>(ended - started_));
  }
 private:
  static std::int64_t now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  bool enabled_;
  const char* operation_; // Fixed internal labels; no participant/meeting data.
  std::int64_t started_ = 0, acquired_ = 0, thumbnailNs_ = 0;
  std::uint64_t thumbnails_ = 0;
};
}
