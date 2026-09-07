#pragma once

#include <algorithm>
#include <cstdint>

namespace corevideo::core {

// Single-thread-owned CPU submission accounting. It neither observes GPU
// completion nor proves that a frame reached a display, encoder, or sender.
// Call recordCompletion after CPU submission, then advance once per loop.
class AnchoredFrameDeadlineTracker {
 public:
  static constexpr std::int64_t kFramesPerSecond = 60;
  static constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;

  // Slot zero completes at ceil(1 second / 60). Rounding each absolute
  // rational deadline upward loses <1ns, rather than accumulating drift.
  static std::int64_t deadlineOffsetNs(std::int64_t slotIndex) {
    const auto count = slotIndex + 1;
    return (count / kFramesPerSecond) * kNanosecondsPerSecond +
        ((count % kFramesPerSecond) * kNanosecondsPerSecond + kFramesPerSecond - 1) /
            kFramesPerSecond;
  }

  std::int64_t nextDeadlineOffsetNs() const { return deadlineOffsetNs(slotIndex_); }
  std::int64_t slotIndex() const { return slotIndex_; }
  std::int64_t completedSlots() const { return completedSlots_; }
  std::int64_t deadlineMisses() const { return deadlineMisses_; }
  std::int64_t skippedSlots() const { return skippedSlots_; }
  std::int64_t maximumCompletionLatenessNs() const { return maximumCompletionLatenessNs_; }
  std::int64_t lastCompletionLatenessNs() const { return lastCompletionLatenessNs_; }

  // Returns false for a duplicate completion of the same slot. Counters retain
  // every completion immediately, including a final partial reporting window.
  bool recordCompletion(std::int64_t elapsedNs) {
    if (completedCurrentSlot_) return false;
    completedCurrentSlot_ = true;
    ++completedSlots_;
    lastCompletionLatenessNs_ = (std::max)(std::int64_t{0}, elapsedNs - nextDeadlineOffsetNs());
    if (lastCompletionLatenessNs_ > 0) ++deadlineMisses_;
    maximumCompletionLatenessNs_ = (std::max)(maximumCompletionLatenessNs_, lastCompletionLatenessNs_);
    return true;
  }

  // Advance after pacing. If >maxCatchUpFrames behind, explicitly discard all
  // expired pending slots and keep the original anchor. Returns skipped count
  // for this call. Calling without completing accounts the abandoned slot too.
  std::int64_t advance(std::int64_t elapsedNs, std::int64_t maxCatchUpFrames = 3) {
    const auto before = skippedSlots_;
    if (!completedCurrentSlot_) ++skippedSlots_;
    ++slotIndex_;
    completedCurrentSlot_ = false;
    const auto catchUp = (std::max)(std::int64_t{0}, maxCatchUpFrames);
    if (elapsedNs > deadlineOffsetNs(slotIndex_ + catchUp)) {
      // First slot with deadline strictly after elapsedNs. Quotient/remainder
      // avoid overflowing elapsedNs*60 for long-running processes.
      const auto firstFutureSlot = (elapsedNs / kNanosecondsPerSecond) * kFramesPerSecond +
          ((elapsedNs % kNanosecondsPerSecond) * kFramesPerSecond) / kNanosecondsPerSecond;
      if (firstFutureSlot > slotIndex_) {
        skippedSlots_ += firstFutureSlot - slotIndex_;
        slotIndex_ = firstFutureSlot;
      }
    }
    return skippedSlots_ - before;
  }

 private:
  std::int64_t slotIndex_ = 0;
  std::int64_t completedSlots_ = 0;
  std::int64_t deadlineMisses_ = 0;
  std::int64_t skippedSlots_ = 0;
  std::int64_t maximumCompletionLatenessNs_ = 0;
  std::int64_t lastCompletionLatenessNs_ = 0;
  bool completedCurrentSlot_ = false;
};

}  // namespace corevideo::core
