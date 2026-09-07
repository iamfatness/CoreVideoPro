#pragma once
#include <cstdint>
#include <optional>

namespace corevideo::core {

// Immutable 60Hz content-to-playout mapping. All timestamps use the caller's
// monotonic nanosecond clock. A stall never moves the anchor or increases delay.
// This is scheduling policy, not proof of GPU readiness or successful output.
class ProgramPlayoutTimeline {
 public:
  struct DueSlot {
    std::int64_t slot = 0;          // original production slot, zero-based
    std::int64_t deadlineNs = 0;    // absolute scheduled delivery timestamp
    std::int64_t skippedSlots = 0; // expired slots omitted by this decision
  };

  // A replacement buffer starts at its first global production slot, retaining
  // the original clock without counting the previous buffer's history as loss.
  ProgramPlayoutTimeline(int bufferFrames, std::int64_t anchorNs, std::int64_t initialSlot = 0)
      : bufferFrames_(bufferFrames == 2 ? 2 : 3), anchorNs_(anchorNs),
        nextSlot_(initialSlot < 0 ? 0 : initialSlot) {}

  int bufferFrames() const { return bufferFrames_; }
  std::int64_t anchorNs() const { return anchorNs_; }
  std::int64_t nextSlot() const { return nextSlot_; }
  std::int64_t skippedSlots() const { return skippedSlots_; }

  // A pre-anchor packet belongs to no production slot and must be rejected.
  std::int64_t productionSlot(std::int64_t producedNs) const {
    return producedNs < anchorNs_ ? -1 : periodsAt(producedNs - anchorNs_);
  }

  std::int64_t deadlineNs(std::int64_t productionSlot) const {
    return anchorNs_ + periodOffsetNs(productionSlot + bufferFrames_);
  }
  std::int64_t nextDeadlineNs() const { return deadlineNs(nextSlot_); }
  bool isExpired(std::int64_t productionSlot) const { return productionSlot < nextSlot_; }

  // The delivery loop consumes this decision even when the corresponding
  // packet is absent/GPU-not-ready, recording an underrun separately. Retrying
  // an expired packet would silently increase content latency after a stall.
  std::optional<DueSlot> takeDue(std::int64_t nowNs) {
    if (nowNs < nextDeadlineNs()) return std::nullopt;
    const auto newestDueSlot = periodsAt(nowNs - anchorNs_) - bufferFrames_;
    if (newestDueSlot < nextSlot_) return std::nullopt;
    const auto skipped = newestDueSlot - nextSlot_;
    skippedSlots_ += skipped;
    nextSlot_ = newestDueSlot + 1;
    return DueSlot{newestDueSlot, deadlineNs(newestDueSlot), skipped};
  }

 private:
  static constexpr std::int64_t kSecondNs = 1'000'000'000;
  static constexpr std::int64_t kRate = 60;
  static std::int64_t periodsAt(std::int64_t elapsedNs) {
    return (elapsedNs / kSecondNs) * kRate + ((elapsedNs % kSecondNs) * kRate) / kSecondNs;
  }
  static std::int64_t periodOffsetNs(std::int64_t periods) {
    // Ceil every absolute rational deadline independently: <1ns rounding,
    // never the accumulating 40us/s error of a constant16666us period.
    return (periods / kRate) * kSecondNs +
        ((periods % kRate) * kSecondNs + kRate - 1) / kRate;
  }
  const int bufferFrames_;
  const std::int64_t anchorNs_;
  std::int64_t nextSlot_ = 0;
  std::int64_t skippedSlots_ = 0;
};

}  // namespace corevideo::core
