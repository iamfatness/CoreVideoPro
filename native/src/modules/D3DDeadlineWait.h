#pragma once
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <thread>

namespace corevideo::modules {
// Immediate-context owner only. Windows sleep_for rounds sub-millisecond polls
// to a scheduler quantum; that can consume the remainder of an original GPU
// deadline even when no optional work is queued. Never extend that deadline.
class D3DDeadlineWait {
 public:
  D3DDeadlineWait() : timer_(CreateWaitableTimerExW(nullptr, nullptr,
      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)) {}
  ~D3DDeadlineWait() { if (timer_) CloseHandle(timer_); }
  D3DDeadlineWait(const D3DDeadlineWait&) = delete;
  D3DDeadlineWait& operator=(const D3DDeadlineWait&) = delete;
  void pause(int64_t remainingNs) {
    const auto delay = (std::min)(remainingNs, int64_t(100000));
    if (delay <= 0) return;
    LARGE_INTEGER due; due.QuadPart = -(std::max)(int64_t(1), (delay + 99) / 100);
    if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
      // Bounded kernel yield. The owner rechecks its original clock/deadline
      // after either signal or timeout; neither outcome means GPU completion.
      WaitForSingleObject(timer_, 1);
    } else std::this_thread::sleep_for(std::chrono::nanoseconds(delay));
  }
 private:
  HANDLE timer_ = nullptr;
};
}
