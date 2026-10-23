#pragma once
#include <cstdint>
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

// Command/SDK callback leaf only. No SDK calls, pipe writes or target-map locks.
class IsoVideoDemand {
 public:
  static IsoVideoDemand& instance() { static IsoVideoDemand value; return value; }
  bool set(const std::string& id, uint32_t epoch) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
          return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') || c == '-' || c == '_';
        })) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!epoch) { epochs_.erase(id); return true; }
    if (!epochs_.contains(id) && epochs_.size() >= 8) return false;
    epochs_[id] = epoch; return true;
  }
  uint32_t get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = epochs_.find(id); return it == epochs_.end() ? 0 : it->second;
  }
 private:
  std::mutex mutex_;
  std::unordered_map<std::string, uint32_t> epochs_;
};
