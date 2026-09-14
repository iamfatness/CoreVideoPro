#pragma once
#include "core/DesiredShowCheckpoint.h"
#include <map>
#include <memory>
#include <mutex>

namespace corevideo::core {
// Metadata ledger only; it neither becomes the production show owner nor mutates
// source/output intent. Input is a complete capture-boundary observation, not a
// command delta. Missing domains remain unsupported, never treated as empty.
class LegacyCaptureLedger final {
 public:
  struct Config {
    size_t maxEntities{4096}, maxGenerationMarks{65536}, maxRetainedBytes{8*1024*1024};
    // Empty allocates a fresh random process-lifetime epoch. Explicit epochs are
    // for deterministic fixtures or a process startup identity allocator only.
    std::string authorityEpoch;
  };
  struct Snapshot {
    std::string authorityEpoch;
    uint64_t revision{0};
    DesiredShowCheckpoint::Capture capture;
    DesiredShowCheckpoint::Result projection;
    bool hasCapture{false};
  };
  enum class Status { Changed, Unchanged, Conflict, Invalid, Capacity, Exhausted };
  struct Result { Status status; std::shared_ptr<const Snapshot> snapshot; };
  LegacyCaptureLedger();
  explicit LegacyCaptureLedger(Config config);
  std::shared_ptr<const Snapshot> snapshot() const;
  // Captured generation/revision fields are not trusted: this ledger allocates
  // them from exact observed entity values. Caller must already have stable IDs.
  // Source tokens, requested booleans and unsupported policy flags are untouched.
  struct Retirement {
    enum class Kind { Scene, Route, Output };
    Kind kind;
    std::string id, sceneId; // sceneId is required only for scene-local Route.
  };
  Result publish(DesiredShowCheckpoint::Capture capture, std::vector<Retirement> retirements = {});
 private:
  struct Mark { uint64_t generation{0}; std::optional<std::string> activeValue; bool operator==(const Mark&) const = default; };
  using Marks = std::map<std::vector<std::string>, Mark>;
  struct State {
    std::shared_ptr<const Snapshot> snapshot;
    Marks marks;
    std::string canonical;
  };
  Config config_;
  mutable std::mutex mutex_;
  std::shared_ptr<const State> state_;
};
} // namespace corevideo::core
