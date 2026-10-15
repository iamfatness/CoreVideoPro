#pragma once
#include "core/ComApartmentLifetime.h"
#include <array>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace corevideo::modules {
// Includes the executing destructor in both bounds. Saturation leaves ownership
// with the caller; no producer-side join, detached thread or overflow allocation.
template<class T, size_t Capacity = 4> class BoundedResourceRetirement {
  static_assert(Capacity > 0);
 public:
  struct Observation { uint64_t pending = 0, bytes = 0, refused = 0; };
  explicit BoundedResourceRetirement(uint64_t byteBudget) : budget_(byteBudget), worker_([this] { run(); }) {}
  ~BoundedResourceRetirement() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    changed_.notify_one();
    worker_.join(); // module shutdown only; all accepted ownership drains here.
  }
  bool tryRetire(std::unique_ptr<T>& value, uint64_t bytes) {
    if (!value) return true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_ || observation_.pending == Capacity || bytes > budget_ - observation_.bytes) {
        ++observation_.refused; return false;
      }
      auto& slot = slots_[(head_ + queued_) % Capacity];
      slot.value = std::move(value); slot.bytes = bytes; ++queued_;
      ++observation_.pending; observation_.bytes += bytes;
    }
    changed_.notify_one(); return true;
  }
  Observation observation() const { std::lock_guard<std::mutex> lock(mutex_); return observation_; }
  void noteRefusal() { std::lock_guard<std::mutex> lock(mutex_); ++observation_.refused; }
 private:
  struct Slot { std::unique_ptr<T> value; uint64_t bytes = 0; };
  void run() {
    core::ComApartmentLifetime apartment;
    for (;;) {
      Slot item;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return stopping_ || queued_; });
        if (!queued_) return;
        item = std::move(slots_[head_]); head_ = (head_ + 1) % Capacity; --queued_;
      }
      item.value.reset(); // potentially blocking destructor, outside admission mutex.
      { std::lock_guard<std::mutex> lock(mutex_); --observation_.pending; observation_.bytes -= item.bytes; }
    }
  }
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::array<Slot, Capacity> slots_{};
  size_t head_ = 0, queued_ = 0;
  const uint64_t budget_;
  Observation observation_;
  bool stopping_ = false;
  std::thread worker_;
};
}
