#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace corevideo::rpc {

// Structural surface announcements are latest per surface, not disposable
// thumbnails. One bounded queue, ordered by the latest announcement for each
// key. The caller owns synchronization; retired payloads leave in PushResult.
class ResourceEventLane {
 public:
  static constexpr std::size_t Capacity = 64;
  static constexpr std::size_t ByteBudget = 2 * 1024 * 1024;
  using Stamp = std::chrono::steady_clock::time_point;
  struct Item { std::string key, message; Stamp stamp; };
  struct PushResult { bool admitted = false; std::string retired; };

  PushResult push(std::string key, std::string message, Stamp stamp = std::chrono::steady_clock::now()) {
    PushResult result;
    auto existing = items_.end();
    for (auto it = items_.begin(); it != items_.end(); ++it) if (it->key == key) { existing = it; break; }
    const auto oldBytes = existing == items_.end() ? 0 : existing->message.size() + existing->key.size();
    const auto newBytes = message.size() + key.size();
    if (key.empty() || key.size() > 1024 || (existing == items_.end() && items_.size() == Capacity) ||
        newBytes > ByteBudget || bytes_ - oldBytes > ByteBudget - newBytes) {
      ++refused_; result.retired = std::move(message); return result;
    }
    if (existing != items_.end()) {
      result.retired = std::move(existing->message);
      items_.erase(existing); ++coalesced_;
    }
    bytes_ = bytes_ - oldBytes + newBytes;
    items_.push_back({std::move(key), std::move(message), stamp});
    result.admitted = true;
    return result;
  }
  Item popFront() {
    Item result = std::move(items_.front()); items_.pop_front();
    bytes_ -= result.key.size() + result.message.size(); return result;
  }
  bool empty() const { return items_.empty(); }
  std::size_t size() const { return items_.size(); }
  std::size_t bytes() const { return bytes_; }
  std::uint64_t refused() const { return refused_; }
  std::uint64_t coalesced() const { return coalesced_; }
 private:
  std::deque<Item> items_;
  std::size_t bytes_ = 0;
  std::uint64_t refused_ = 0, coalesced_ = 0;
};

}  // namespace corevideo::rpc
