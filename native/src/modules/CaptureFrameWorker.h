#pragma once
#include "core/ComApartmentLifetime.h"
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace corevideo::modules {
// One active OS frame and one pending frame. Capture callbacks never wait for
// conversion or GPU completion. Replaced OS frame ownership is released outside
// the mailbox lock. The processing callable owns all immediate-context work.
template <typename Frame> class CaptureFrameWorker {
 public:
  struct Stats { uint64_t accepted = 0, completed = 0, superseded = 0, failed = 0, refused = 0; };
  using Process = std::function<void(const Frame&)>;
  explicit CaptureFrameWorker(Process process, std::function<void()> idle = {}, std::function<void()> shutdown = {})
      : thread_([this, process = std::move(process), idle = std::move(idle), shutdown = std::move(shutdown)] {
          core::ComApartmentLifetime apartment;
          for (;;) {
            std::shared_ptr<const Frame> frame;
            {
              std::unique_lock<std::mutex> lock(mutex_);
              if (idle) ready_.wait_for(lock, std::chrono::milliseconds(10), [this] { return stopping_ || pending_; });
              else ready_.wait(lock, [this] { return stopping_ || pending_; });
              if (stopping_) break;
              frame.swap(pending_);
            }
            bool success = true;
            try { if (idle) idle(); if (frame) process(*frame); } catch (...) { success = false; }
            std::lock_guard<std::mutex> lock(mutex_);
            if (!success) ++stats_.failed; else if (frame) ++stats_.completed;
          }
          try { if (shutdown) shutdown(); } catch (...) { std::lock_guard<std::mutex> lock(mutex_); ++stats_.failed; }
        }) {}
  ~CaptureFrameWorker() { stop(); }
  bool submit(Frame frame, bool preserveOrder = false) {
    auto next = std::make_shared<const Frame>(std::move(frame));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) { ++stats_.refused; return false; }
      // CPU recording demand preserves already admitted order. Capacity refusal
      // is explicit; it does not borrow the monitor's latest-wins policy.
      if (pending_ && preserveOrder) { ++stats_.refused; return false; }
      if (pending_) ++stats_.superseded;
      pending_.swap(next);
      ++stats_.accepted;
    }
    ready_.notify_one();
    return true;
  }
  void stop() {
    std::shared_ptr<const Frame> retired;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      retired.swap(pending_);
    }
    ready_.notify_one();
    if (thread_.joinable()) thread_.join();
  }
  Stats stats() const { std::lock_guard<std::mutex> lock(mutex_); return stats_; }
 private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::shared_ptr<const Frame> pending_;
  bool stopping_ = false;
  Stats stats_;
  std::thread thread_;
};
}
