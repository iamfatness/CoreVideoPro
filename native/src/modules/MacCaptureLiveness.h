#pragma once
// Capture liveness for the Mac adapters (Sources Phase 2 plan, Task 7). The Windows SHM
// bridge reports liveness through ShmCapturePreparation (Live on a sample, Lost with a
// reason); the AVFoundation and ScreenCaptureKit adapters reported nothing, so every Mac
// capture row read "unreported" and the on-air dropout rules never applied.
//
// cadenceTrusted is the one platform judgement: a camera delivers continuously, so its
// silence means trouble (stalled at 1.5 s, failed at 5 s, CaptureLivenessPolicy.h);
// ScreenCaptureKit sends a frame only when the screen changes, so its silence is not loss
// and only an explicit stop counts.
//
// Pure and clock-free (callers pass steady_clock ns), so the decision is testable without
// a device. Not thread-safe: each owner holds it under its own state mutex.
#include "modules/Interfaces.h"

#include <chrono>
#include <cstdint>

namespace corevideo::modules {

class MacCaptureLiveness {
 public:
  explicit MacCaptureLiveness(bool cadenceTrusted) : cadenceTrusted_(cadenceTrusted) {}

  static int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  // A real delivery from the device (never a re-emit of the held picture).
  void sample(int64_t nowNs) {
    value_ = {CaptureLivenessState::Live, cadenceTrusted_, nowNs, 0, CaptureLossReason::None};
  }

  // An explicit loss. Repeating the same reason keeps the first loss time, so the 2 s hold
  // is not restarted by a re-report (the ShmCapturePreparation observeLoss rule).
  void lost(CaptureLossReason reason, int64_t nowNs) {
    if (value_.state == CaptureLivenessState::Lost && value_.reason == reason) return;
    if (value_.state != CaptureLivenessState::Lost) value_.lostAtNs = nowNs;
    value_.state = CaptureLivenessState::Lost;
    value_.cadenceTrusted = cadenceTrusted_;
    value_.reason = reason;
  }

  // Unreported until the first delivery or loss.
  [[nodiscard]] CaptureLiveness value() const { return value_; }

 private:
  bool cadenceTrusted_;
  CaptureLiveness value_{};
};

}  // namespace corevideo::modules
