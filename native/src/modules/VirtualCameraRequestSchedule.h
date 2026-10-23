#pragma once
#include "modules/VirtualCameraCadence.h"

namespace corevideo::modules {
// One serialized MF stream request owner. Cold allocation/open work happens
// before the first actual emission; it must not leave later slots in the past.
class VirtualCameraRequestSchedule {
 public:
  struct Window { int64_t target100ns, wait100ns, readDeadlineUs; };
  void reset(uint32_t fps) {
    fps_ = fps ? fps : 30; cadence_.reset(fps_); nextDue_ = 0; freshAnchored_ = false;
  }
  Window begin(int64_t now) {
    const auto periodCeil = (10000000LL + fps_ - 1) / fps_;
    if (!nextDue_ || now >= nextDue_ + periodCeil) {
      nextDue_ = now; cadence_.reset(fps_);
    }
    const auto target = nextDue_;
    nextDue_ += cadence_.nextInterval100ns();
    return {target, target > now ? target - now : 0, (nextDue_ - 20000) / 10};
  }
  bool firstFreshEmission(int64_t now) {
    if (freshAnchored_) return false;
    freshAnchored_ = true;
    cadence_.reset(fps_);
    nextDue_ = now + cadence_.nextInterval100ns();
    return true;
  }
 private:
  uint32_t fps_ = 60;
  VirtualCameraCadence cadence_;
  int64_t nextDue_ = 0;
  bool freshAnchored_ = false;
};
}
