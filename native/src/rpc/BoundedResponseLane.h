#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace corevideo::rpc {

// High-priority stdout responses. A slow shell cannot grow this without bound.
// Overflow drops the oldest response and counts it. Push never waits, so a full
// lane cannot stall the render or audio workers behind a flush.
class BoundedResponseLane {
 public:
  static constexpr std::size_t kMaxDepth = 32;
  using Stamp = std::chrono::steady_clock::time_point;

  // Returns true when the oldest queued response was dropped to make room.
  bool push(std::string message, Stamp stamp = std::chrono::steady_clock::now()) {
    bool dropped = false;
    if (items_.size() >= kMaxDepth) {
      items_.pop_front();
      ++dropped_;
      dropped = true;
    }
    items_.emplace_back(std::move(message), stamp);
    return dropped;
  }

  bool empty() const { return items_.empty(); }
  std::size_t size() const { return items_.size(); }
  std::uint64_t dropped() const { return dropped_; }

  std::pair<std::string, Stamp> popFront() {
    auto item = std::move(items_.front());
    items_.pop_front();
    return item;
  }

 private:
  std::deque<std::pair<std::string, Stamp>> items_;
  std::uint64_t dropped_ = 0;
};

}  // namespace corevideo::rpc
