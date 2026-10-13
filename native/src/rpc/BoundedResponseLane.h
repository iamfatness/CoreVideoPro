#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace corevideo::rpc {

// High-priority stdout responses. The server reserves byte/depth capacity with
// canPush before calling push, waiting outside all media locks. Legacy direct
// push callers retain the counted oldest-drop behavior for compatibility.
class BoundedResponseLane {
 public:
  static constexpr std::size_t kMaxDepth = 32;
  static constexpr std::size_t kMaxBytes = 16 * 1024 * 1024;
  static constexpr std::size_t kMaxMessageBytes = 4 * 1024 * 1024;
  bool canPush(std::size_t bytes) const { return size() < kMaxDepth && bytes <= kMaxMessageBytes && bytes_ + bytes <= kMaxBytes; }
  using Stamp = std::chrono::steady_clock::time_point;

  // Returns true when the oldest queued response was dropped to make room.
  bool push(std::string message, Stamp stamp = std::chrono::steady_clock::now()) {
    bool dropped = false;
    if (items_.size() >= kMaxDepth) {
      bytes_ -= items_.front().first.size();
      items_.pop_front();
      ++dropped_;
      dropped = true;
    }
    bytes_ += message.size();
    items_.emplace_back(std::move(message), stamp);
    return dropped;
  }

  bool empty() const { return items_.empty(); }
  std::size_t size() const { return items_.size(); }
  std::uint64_t dropped() const { return dropped_; }

  std::pair<std::string, Stamp> popFront() {
    auto item = std::move(items_.front());
    bytes_ -= item.first.size();
    items_.pop_front();
    return item;
  }

 private:
  std::deque<std::pair<std::string, Stamp>> items_;
  std::uint64_t dropped_ = 0;
  std::size_t bytes_ = 0;
};

}  // namespace corevideo::rpc
