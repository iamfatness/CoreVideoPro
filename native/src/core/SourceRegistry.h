#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace corevideo::core {

// IDs are intentionally different types; neither names nor SDK handles are IDs.
struct PersonId { std::string value; };
struct SourceId { std::string value; };
struct SourceInstanceId { std::string value; };

class SourceRegistry final {
 public:
  enum class Kind { ParticipantVideo, ParticipantShare, Device, Media, Browser };
  enum class Availability { Available, Unavailable, Departed };
  enum class Result { Applied, Unchanged, Invalid, NotFound, Conflict, Stale, Exhausted };
  struct Format {
    int width = 0, height = 0;
    std::optional<int> fpsNumerator, fpsDenominator; // Both absent means unknown rate.
    std::string pixelFormat;
    bool operator==(const Format&) const = default;
  };
  struct Token {
    SourceId sourceId;
    SourceInstanceId instanceId;
    std::string processEpoch;
    uint64_t generation = 0;
  };
  struct Person {
    PersonId id;
    std::string displayName;
    uint64_t generation = 1;
  };
  struct Registration {
    SourceId sourceId;
    Kind kind = Kind::ParticipantVideo;
    std::optional<PersonId> personId; // Explicit operator/authority binding only.
    uint64_t personGeneration = 0;
    std::string displayName;
    std::string processEpoch;
    std::string externalId; // SDK participant/device ID, scoped by processEpoch.
    std::optional<SourceInstanceId> instanceId; // Provider-owned exact identity, if available.
    std::optional<uint64_t> requestedGeneration; // Provider fence; gaps are valid.
  };
  struct Source {
    Token token;
    Kind kind = Kind::ParticipantVideo;
    std::optional<PersonId> personId;
    uint64_t personGeneration = 0;
    std::string displayName, externalId;
    Availability availability = Availability::Available;
    bool subscriptionRequested = false;
    std::optional<bool> subscriptionObserved{false}; // nullopt means unacknowledged/unknown.
    std::optional<Format> format;
    bool hasPublication = false;
    bool hasPublicationWatermark = false; // Survives unavailable/departed until token replacement.
    uint64_t publicationSequence = 0;
    int64_t lastPublicationNs = 0; // Caller monotonic clock; never UTC.
  };
  struct Snapshot {
    std::string registryEpoch;
    uint64_t revision = 0;
    uint64_t decisionRevision = 0; // Binding/readiness changes, independent of frame traffic.
    std::vector<Person> persons;
    std::vector<Source> sources; // Stable SourceId order, includes departure tombstones.
  };
  struct Mutation { Result result; std::optional<Token> token; };

  explicit SourceRegistry(std::string registryEpoch, std::size_t maxPersons = 4'096,
                          std::size_t maxSources = 16'384,
                          std::size_t maxRetiredProcessEpochs = 4'096);
  SourceRegistry(const SourceRegistry&); // Bounded independent staging copy, including tombstones.
  Result upsertPerson(Person person);
  Mutation add(Registration registration);
  // Compare-and-replace: old callbacks can neither replace nor retire a new instance.
  Mutation replace(const Token& expected, Registration registration);
  Result setDisplayName(const Token& token, const std::string& name);
  Result setAvailability(const Token& token, Availability availability);
  Result setSubscription(const Token& token, bool requested, std::optional<bool> observed);
  // Retire every source owned by a replaced helper process as one registry
  // transaction. This fences callbacks even when a provider changes its source IDs.
  Result retireProcessEpoch(const std::string& processEpoch);
  Result publish(const Token& token, uint64_t sequence, int64_t observedNs, Format format);
  [[nodiscard]] std::shared_ptr<const Snapshot> snapshot() const;
  // Discovery only: zero/multiple matches remain explicit, with no auto-binding.
  [[nodiscard]] std::vector<PersonId> peopleNamed(const std::string& displayName) const;

 private:
  static bool sameToken(const Token& left, const Token& right);
  bool validRegistration(const Registration& registration) const;
  bool externalConflict(const Registration& registration) const;
  Mutation install(Registration registration, uint64_t generation);
  static constexpr uint64_t kMaxRevision = 9007199254740991ULL;
  mutable std::mutex mutex_;
  std::string epoch_;
  uint64_t revision_ = 0;
  uint64_t decisionRevision_ = 0;
  std::map<std::string, Person> persons_;
  std::map<std::string, Source> sources_;
  std::set<std::string> retiredProcessEpochs_;
  std::size_t maxPersons_, maxSources_, maxRetiredProcessEpochs_;
};

} // namespace corevideo::core
