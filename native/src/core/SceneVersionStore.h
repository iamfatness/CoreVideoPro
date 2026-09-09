#pragma once
#include "core/ShowStateOwner.h"

namespace corevideo::core {
struct SceneVersionRef {
  std::string authorityEpoch, sceneId;
  uint64_t sceneGeneration{0}, version{0};
  auto operator<=>(const SceneVersionRef&) const = default;
};
struct ImmutableSceneVersion final {
  const SceneVersionRef ref;
  // Complete ShowSceneIntent value; generation keeps its existing content-model
  // meaning and is not silently reinterpreted as sceneGeneration or version.
  const ShowSceneIntent scene;
};
class SceneVersionStore final {
 public:
  using Lease = std::shared_ptr<const ImmutableSceneVersion>;
  enum class Status { Applied, Unchanged, Invalid, Stale, Conflict, Capacity, NotFound, Pinned };
  struct Result { Status status; Lease lease; };
  struct Limits { size_t versions{128}, scenes{4096}, retiredEpochs{128}, payloadBytes{16*1024*1024}; };
  explicit SceneVersionStore(std::string authorityEpoch);
  SceneVersionStore(const SceneVersionStore&); // Copies bounded indices; shares only const payloads.
  SceneVersionStore(std::string authorityEpoch, Limits limits);
  // New scene/recreation: expectedHead=null; edits: exact current head required.
  // Caller assigns positive safe versions; forward gaps are allowed, reuse is not.
  // Exact existing ref+payload replay is read-only and does not move the head.
  Result publish(SceneVersionRef ref, ShowSceneIntent scene, std::optional<SceneVersionRef> expectedHead = {});
  struct Publication {
    SceneVersionRef reference;
    ShowSceneIntent scene;
    std::optional<SceneVersionRef> expectedHead;
  };
  struct BatchResult { Status status; std::vector<Lease> leases; };
  // At most two unique bus definitions, in caller order. Expected heads are
  // checked against the staged state (including preceding items). Exact replay
  // does not move a head. Failure returns no leases and changes no store state.
  BatchResult publishBatch(std::vector<Publication> publications);
  Result resolve(const SceneVersionRef&) const;
  // One admission point for both buses, fenced against concurrent erase/restart.
  std::pair<Result, Result> resolveBuses(const std::optional<SceneVersionRef>& program,
      const std::optional<SceneVersionRef>& preview) const;
  Result head(const std::string& sceneId) const;
  Status erase(const SceneVersionRef& expectedHead);
  // Explicit eviction only; heads and externally leased versions cannot be evicted.
  Status evict(const SceneVersionRef&);
  Status restart(const std::string& expectedEpoch, std::string nextEpoch);
 private:
  struct SceneHead { uint64_t generation{0}, highVersion{0}; bool active{false}; SceneVersionRef ref; };
  struct Entry { Lease lease; size_t bytes; };
  mutable std::mutex mutex_;
  std::string epoch_;
  Limits limits_;
  size_t bytes_{0};
  std::map<SceneVersionRef, Entry> versions_;
  std::map<std::string, SceneHead> scenes_; // Includes bounded deletion tombstones.
  std::set<std::string> retiredEpochs_;
};
}
