#pragma once
#include <cstdint>

namespace corevideo::modules {
// Preserve the rational frame rate in the 100 ns MF clock. Repeatedly adding
// floor(10,000,000 / 60) runs 14.4 ms early per hour and changes the reader's
// phase relative to Program publication. Fractional ticks never accumulate.
class VirtualCameraCadence {
 public:
  explicit VirtualCameraCadence(uint32_t fps = 60) { reset(fps); }
  void reset(uint32_t fps) { fps_ = fps ? fps : 30; remainder_ = 0; }
  int64_t nextInterval100ns() {
    remainder_ += 10000000u % fps_;
    const auto extra = remainder_ / fps_;
    remainder_ %= fps_;
    return int64_t(10000000u / fps_) + int64_t(extra);
  }
 private:
  uint32_t fps_ = 60;
  uint64_t remainder_ = 0;
};
}
