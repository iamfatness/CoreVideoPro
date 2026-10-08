#pragma once
#include "core/BoundedAsyncLog.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace corevideo::modules {
// Optional CPU call-scope attribution. Includes OS scheduling/preemption;
// never reports GPU duration or proves the cause of a slow driver call.
class GpuSubmissionCpuScope {
 public:
  enum class Kind { ProducerTransfer, Readiness };
  static bool requested() {
    const auto* value = std::getenv("COREVIDEO_QA_GPU_SUBMISSION_TIMING");
    return value && std::string(value) == "1";
  }
  GpuSubmissionCpuScope(bool enabled, const char* owner, std::uint64_t epoch, std::int64_t frame,
      Kind kind = Kind::ProducerTransfer) noexcept
      : enabled_(enabled), owner_(owner), epoch_(epoch), frame_(frame), kind_(kind) {
    if (enabled_) start_ = previous_ = now();
  }
  // In order: slot reservation, keyed acquire, GPU-copy call, keyed release,
  // queue publication. Early returns retain an explicit partial stage count.
  void boundary() noexcept {
    if (!enabled_ || completed_ >= intervals_.size()) return;
    const auto stamp = now();
    intervals_[completed_++] = stamp - previous_; previous_ = stamp;
  }
  ~GpuSubmissionCpuScope() {
    if (!enabled_) return;
    const auto ended = now();
    if (ended - start_ < 8000000) return;
    // Core-owned bounded asynchronous logger; no file I/O or DLL worker here.
    if (kind_ == Kind::Readiness) {
      ::corevideo::core::nativeLogf(
          "[gpu-readiness-cpu-v1] owner=%s frame=%lld completed_stages=%u prewarm_ns=%lld readiness_ns=%lld partial_ns=%lld total_ns=%lld gpu_duration_verified=0 driver_cause_verified=0\n",
          owner_, static_cast<long long>(frame_), completed_, static_cast<long long>(intervals_[0]),
          static_cast<long long>(intervals_[1]), static_cast<long long>(ended - previous_),
          static_cast<long long>(ended - start_));
      return;
    }
    ::corevideo::core::nativeLogf(
        "[gpu-submit-cpu-v1] owner=%s epoch=%llu frame=%lld completed_stages=%u reserve_ns=%lld acquire_ns=%lld copy_ns=%lld release_ns=%lld queue_ns=%lld partial_ns=%lld total_ns=%lld gpu_duration_verified=0 driver_cause_verified=0\n",
        owner_, static_cast<unsigned long long>(epoch_), static_cast<long long>(frame_), completed_,
        static_cast<long long>(intervals_[0]), static_cast<long long>(intervals_[1]),
        static_cast<long long>(intervals_[2]), static_cast<long long>(intervals_[3]),
        static_cast<long long>(intervals_[4]), static_cast<long long>(ended - previous_),
        static_cast<long long>(ended - start_));
  }
 private:
  static std::int64_t now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  bool enabled_;
  const char* owner_; // Fixed internal branch label; never a meeting/source name.
  std::uint64_t epoch_;
  std::int64_t frame_, start_ = 0, previous_ = 0;
  Kind kind_;
  unsigned completed_ = 0;
  std::array<std::int64_t, 5> intervals_{};
};
}
