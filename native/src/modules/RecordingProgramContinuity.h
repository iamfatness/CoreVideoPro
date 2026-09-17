#pragma once
#include <cstdint>

namespace corevideo::modules {
// Counts missing scheduled pictures between real Program samples accepted by
// the writer. Includes upstream delivery and writer-queue gaps: never add this
// to queue-drop counters. It cannot prove the unobserved head/tail of a take.
class RecordingProgramContinuity {
 public:
  void observe(int64_t anchorNs, int64_t slot) {
    if (anchorNs <= 0 || slot < 0) {
      anchorNs_ = 0;
      lastSlot_ = -1;
      return;
    }
    observed_ = true;
    if (anchorNs == anchorNs_ && slot <= lastSlot_) return;
    if (anchorNs == anchorNs_ && lastSlot_ >= 0)
      missingFrames_ += slot - lastSlot_ - 1;
    anchorNs_ = anchorNs;
    lastSlot_ = slot;
  }
  int64_t missingFrames() const { return missingFrames_; }
  bool observed() const { return observed_; }
 private:
  int64_t anchorNs_ = 0;
  int64_t lastSlot_ = -1;
  int64_t missingFrames_ = 0;
  bool observed_ = false;
};
}  // namespace corevideo::modules
