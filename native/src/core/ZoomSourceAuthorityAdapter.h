#pragma once
#include "core/SourceRegistry.h"

namespace corevideo::core {
class ZoomSourceAuthorityAdapter final {
 public:
  struct Person {
    std::string id, name;
    uint64_t generation{1};
    bool operator==(const Person&) const = default;
  };
  struct Publication {
    uint64_t sequence{0};
    int64_t observedNs{0};
    int width{0}, height{0};
    std::optional<int> fpsNumerator, fpsDenominator;
    std::string pixelFormat;
    bool operator==(const Publication&) const = default;
  };
  struct Source {
    std::string id, externalId, personId, name;
    std::string instanceId;
    uint64_t personGeneration{0}, incarnation{1};
    SourceRegistry::Kind kind{SourceRegistry::Kind::ParticipantVideo};
    bool videoAvailable{true}, subscriptionRequested{false};
    std::optional<bool> subscriptionObserved{false};
    std::optional<Publication> publication;
    bool operator==(const Source&) const = default;
  };
  struct Observation {
    std::string processEpoch;
    uint64_t sequence{0}; // Complete roster revision, monotonically increasing per epoch.
    std::vector<Person> people;
    std::vector<Source> sources;
    bool operator==(const Observation&) const = default;
  };
  enum class Status { Applied, Unchanged, Stale, Invalid, Capacity, RegistryFailure };
  struct SyncResult {
    Status status;
    SourceRegistry::Result registryResult{SourceRegistry::Result::Unchanged};
    std::shared_ptr<const SourceRegistry::Snapshot> snapshot;
    // This adapter carries video observations only. Never infer audio readiness.
    bool audioCapabilityKnown{false};
  };
  explicit ZoomSourceAuthorityAdapter(std::string authorityEpoch, size_t maxPeople = 4096,
                                     size_t maxSources = 16384, size_t maxEpochs = 4096);
  ZoomSourceAuthorityAdapter(const ZoomSourceAuthorityAdapter&);
  SyncResult sync(Observation observation);
  std::shared_ptr<const SourceRegistry::Snapshot> snapshot() const;
 private:
  ZoomSourceAuthorityAdapter(const ZoomSourceAuthorityAdapter&, std::unique_lock<std::mutex>);
  struct Binding { Source observation; SourceRegistry::Token token; bool present{true}; };
  mutable std::mutex mutex_;
  SourceRegistry registry_; // Sole writer; callers receive immutable snapshots only.
  size_t maxPeople_, maxSources_, maxEpochs_;
  std::set<std::string> peopleIds_, sourceIds_, retiredEpochs_;
  std::map<std::string, Binding> bindings_;
  std::optional<Observation> last_;
};
} // namespace corevideo::core
