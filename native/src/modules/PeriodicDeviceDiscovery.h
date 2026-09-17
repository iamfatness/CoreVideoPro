#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace corevideo::modules {

// OS discovery runs without the reader lock. Snapshot callers only transfer a
// completed result, so a slow driver cannot hold the media core's render lock.
template <typename T>
class PeriodicDeviceDiscovery {
 public:
  using Discover = std::function<std::optional<T>()>;
  PeriodicDeviceDiscovery(Discover discover, std::chrono::milliseconds interval)
      : worker_([this, discover = std::move(discover), interval] {
          for (;;) {
            std::unique_ptr<T> result;
            try {
              if (auto value = discover()) result = std::make_unique<T>(std::move(*value));
            } catch (...) {
              // Keep the last known device list on discovery failure.
            }
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_) return;
            if (result) pending_ = std::move(result);
            if (wake_.wait_for(lock, interval, [this] { return stop_; })) return;
          }
        }) {}
  ~PeriodicDeviceDiscovery() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    wake_.notify_one();
    worker_.join();
  }
  PeriodicDeviceDiscovery(const PeriodicDeviceDiscovery&) = delete;
  PeriodicDeviceDiscovery& operator=(const PeriodicDeviceDiscovery&) = delete;

  std::unique_ptr<T> takeLatest() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::move(pending_);
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  std::unique_ptr<T> pending_;
  std::thread worker_;
};

}  // namespace corevideo::modules
