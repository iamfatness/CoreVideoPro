// native/src/core/SourceContinuityLedger.h
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace corevideo::core {

// WHICH SOURCES RESTARTED, PER SOURCE, ON EVIDENCE.
//
// A persistent source (spec 2026-09-10-persistent-sources-design §2) has one
// clock: its frame ids only ever advance. A frame id that goes BACKWARDS is a
// decoder that reopened; a source that vanishes for longer than a beat and
// comes back cold-started. Both are "restarts", and the Take record uses this
// ledger to refuse the word "cut" when any source present on both sides of a
// Take restarted across it.
//
// Pure bookkeeping (strings and ints), safe to run under coreMutex on the
// render gather. No pixels, no allocation beyond the map.
struct SourceContinuity {
  std::uint64_t generation = 0;
  std::int64_t lastFrameId = -1;
  std::int64_t lastSeenTick = -1;
};

class SourceContinuityLedger {
 public:
  static constexpr std::int64_t absentTicksBeforeRestart = 30;  // 500 ms at 60 Hz

  void observe(const std::string& sourceId, std::int64_t frameId, std::int64_t tick) {
    auto& entry = entries_[sourceId];
    const bool first = entry.generation == 0;
    const bool regressed = !first && frameId < entry.lastFrameId;
    const bool reappeared = !first && (tick - entry.lastSeenTick) > absentTicksBeforeRestart;
    if (first || regressed || reappeared) ++entry.generation;
    entry.lastFrameId = frameId;
    entry.lastSeenTick = tick;
  }

  void endTick(std::int64_t /*tick*/) {}

  [[nodiscard]] std::optional<SourceContinuity> lookup(const std::string& sourceId) const {
    const auto found = entries_.find(sourceId);
    if (found == entries_.end()) return std::nullopt;
    return found->second;
  }

  [[nodiscard]] std::map<std::string, SourceContinuity> snapshot(
      const std::vector<std::string>& sourceIds) const {
    std::map<std::string, SourceContinuity> result;
    for (const auto& id : sourceIds) {
      if (const auto found = entries_.find(id); found != entries_.end()) result.emplace(id, found->second);
    }
    return result;
  }

 private:
  std::map<std::string, SourceContinuity> entries_;
};

}  // namespace corevideo::core
