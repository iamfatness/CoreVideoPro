#pragma once

#include "modules/Interfaces.h"
#include "core/BoundedAsyncLog.h"
#include "core/ComApartmentLifetime.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace corevideo::modules {

// A single monitor owner and one replaceable pending job. The producer never
// waits for GPU work, allocation, export, or backend destruction. Large payloads
// and old leases are released outside the mailbox lock.
class MonitorRenderWorker {
 public:
  using Render = std::function<MonitorRenderResult(const MonitorRenderRequest&)>;
  explicit MonitorRenderWorker(Render render)
      : thread_([this, render = std::move(render)]() mutable { run(render); }) {}
  ~MonitorRenderWorker() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    changed_.notify_one();
    if (thread_.joinable()) thread_.join(); // compositor shutdown, never a render tick
  }
  void submit(MonitorRenderRequest request) {
    auto next = std::make_shared<const MonitorRenderRequest>(std::move(request));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) return;
      if (pending_) ++superseded_;
      pending_.swap(next);
      ++submitted_;
    }
    changed_.notify_one();
  }
  std::shared_ptr<const MonitorRenderResult> latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
  }
  MonitorRenderDiagnostics diagnostics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {true, submitted_, completed_, superseded_, failed_, pending_ ? 1 : 0,
        result_ ? result_->sequence : 0, result_ ? result_->workMs : 0};
  }
 private:
  void run(Render& render) {
    core::ComApartmentLifetime apartment;
    struct ReleaseBackend { Render& render; ~ReleaseBackend() { render = {}; } } release{render};
    auto lastLog = std::chrono::steady_clock::now();
    for (;;) {
      std::shared_ptr<const MonitorRenderRequest> job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return stopping_ || pending_; });
        if (stopping_) return;
        job.swap(pending_);
      }
      const auto start = std::chrono::steady_clock::now();
      try {
        auto result = std::make_shared<MonitorRenderResult>(render(*job));
        result->sequence = job->sequence;
        result->workMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::shared_ptr<const MonitorRenderResult> immutable = std::move(result);
        {
          std::lock_guard<std::mutex> lock(mutex_);
          result_.swap(immutable);
          ++completed_;
        }
      } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++failed_;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now - lastLog >= std::chrono::seconds(2)) {
        uint64_t submitted, superseded, completed;
        double workMs;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          submitted = submitted_; superseded = superseded_; completed = completed_;
          workMs = result_ ? result_->workMs : 0;
        }
        core::nativeLogf("[monitor-worker] submitted=%llu completed=%llu superseded=%llu failed=%llu last_work_ms=%.3f capacity=1 presentationVerified=0\n",
            static_cast<unsigned long long>(submitted), static_cast<unsigned long long>(completed),
            static_cast<unsigned long long>(superseded), static_cast<unsigned long long>(failed_), workMs);
        lastLog = now;
      }
    }
  }
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::shared_ptr<const MonitorRenderRequest> pending_;
  std::shared_ptr<const MonitorRenderResult> result_;
  bool stopping_ = false;
  uint64_t submitted_ = 0, superseded_ = 0, completed_ = 0, failed_ = 0;
  std::thread thread_;
};

} // namespace corevideo::modules
