#pragma once
#include "modules/Interfaces.h"
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <unordered_set>

namespace corevideo::core {
// Owns ISO arrivals, independently of monitor supersession and Program pacing.
// This mutex is a leaf: no encoder, pixel conversion, or core callback under it.
class IsoVideoIngress {
 public:
  struct Counters {
    uint64_t distinctSubmitted = 0, duplicateRejected = 0, queueOverflowed = 0;
  };
  static std::string canonical(const std::string& id) {
    return id.empty() || id.find(':') != std::string::npos ? id : "zoom:" + id;
  }
  void observe(const std::vector<std::string>& selected,
               const std::vector<modules::VideoFrame>& frames,
               const std::vector<modules::VideoFrame>& cpuArrivals,
               int64_t arrival100ns) {
    std::unordered_set<std::string> ids;
    for (const auto& id : selected) ids.insert(canonical(id));
    std::unordered_set<std::string> liveIds;
    for (const auto& frame : frames) liveIds.insert(canonical(frame.participantId));
    for (const auto& frame : cpuArrivals) liveIds.insert(canonical(frame.participantId));
    std::lock_guard<std::mutex> lock(mutex_);
    // Keep a completed CPU view between conversion arrivals while that source
    // remains live. A disconnected or deselected source releases its lease.
    for (auto it = latest_.begin(); it != latest_.end();) {
      if (!ids.count(it->first) || !liveIds.count(it->first)) it = latest_.erase(it);
      else ++it;
    }
    const auto admit = [&](const modules::VideoFrame& frame) {
      if (!frame.hasPixels() && !frame.hasI420()) return;
      const auto id = canonical(frame.participantId);
      if (!ids.count(id) || id.rfind("media:", 0) == 0) return;
      latest_[id] = frame;
      auto& counter = counters_[id];
      const auto prior = lastQueued_.find(id);
      const auto identity = std::make_pair(frame.sourceEpoch, frame.frameId);
      if (prior != lastQueued_.end() && prior->second == identity) {
        ++counter.duplicateRejected; return;
      }
      lastQueued_[id] = identity;
      size_t pending = 0;
      for (const auto& entry : pending_) if (entry.sourceId == id) ++pending;
      if (pending >= 4) {
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
          if (it->sourceId == id) { pending_.erase(it); ++counter.queueOverflowed; break; }
        }
      }
      modules::IsoSourceVideoFrame entry{id, {}, frame};
      entry.timelineTimestamp100ns = frame.captureTimestamp100ns > 0
          ? frame.captureTimestamp100ns : arrival100ns;
      pending_.push_back(std::move(entry));
      ++counter.distinctSubmitted;
    };
    // An independent CPU stream has its own identities and time. Never relabel
    // it with the newer GPU image's identity or collapse a batch to one slot.
    std::unordered_set<std::string> independentIds;
    for (const auto& frame : cpuArrivals) independentIds.insert(canonical(frame.participantId));
    for (const auto& frame : frames) if (!independentIds.count(canonical(frame.participantId))) admit(frame);
    for (const auto& frame : cpuArrivals) admit(frame);
  }
  std::vector<modules::IsoSourceVideoFrame> latest(const std::vector<std::string>& selected) const {
    std::vector<modules::IsoSourceVideoFrame> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& raw : selected) {
      const auto id = canonical(raw);
      const auto it = latest_.find(id);
      if (it != latest_.end()) result.push_back({id, {}, it->second});
    }
    return result;
  }
  std::map<std::string, Counters> counters() const {
    std::lock_guard<std::mutex> lock(mutex_); return counters_;
  }
  std::vector<modules::IsoSourceVideoFrame> drain(std::chrono::milliseconds wait = std::chrono::milliseconds(20)) {
    std::vector<modules::IsoSourceVideoFrame> result;
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait_for(lock, wait, [this] { return !pending_.empty(); });
    result.swap(pending_);
    return result;
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_.clear(); pending_.clear(); lastQueued_.clear(); counters_.clear();
  }
  void notify() { ready_.notify_one(); }
 private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::map<std::string, modules::VideoFrame> latest_;
  std::map<std::string, std::pair<uint64_t, int64_t>> lastQueued_;
  std::map<std::string, Counters> counters_;
  std::vector<modules::IsoSourceVideoFrame> pending_;
};
}
