#pragma once

#include <cstdint>
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
    int fpsNumerator = 0, fpsDenominator = 1;
    std::string pixelFormat;
  };
  struct Token {
    SourceId sourceId;
    SourceInstanceId instanceId;
    std::string processEpoch;
    uint64_t generation = 0;
  };
  struct Person { PersonId id; std::string displayName; };
  struct Registration {
    SourceId sourceId;
    Kind kind = Kind::ParticipantVideo;
    std::optional<PersonId> personId; // Explicit operator/authority binding only.
    std::string displayName;
    std::string processEpoch;
    std::string externalId; // SDK participant/device ID, scoped by processEpoch.
  };
  struct Source {
    Token token;
    Kind kind = Kind::ParticipantVideo;
    std::optional<PersonId> personId;
    std::string displayName, externalId;
    Availability availability = Availability::Available;
    bool subscriptionRequested = false;
    bool subscriptionObserved = false;
    std::optional<Format> format;
    bool hasPublication = false;
    uint64_t publicationSequence = 0;
    int64_t lastPublicationNs = 0; // Caller monotonic clock; never UTC.
  };
  struct Snapshot {
    std::string registryEpoch;
    uint64_t revision = 0;
    std::vector<Person> persons;
    std::vector<Source> sources; // Stable SourceId order, includes departure tombstones.
  };
  struct Mutation { Result result; std::optional<Token> token; };

  explicit SourceRegistry(std::string registryEpoch);
  Result upsertPerson(Person person);
  Mutation add(Registration registration);
  // Compare-and-replace: old callbacks can neither replace nor retire a new instance.
  Mutation replace(const Token& expected, Registration registration);
  Result setAvailability(const Token& token, Availability availability);
  Result setSubscription(const Token& token, bool requested, bool observed);
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
  std::map<std::string, Person> persons_;
  std::map<std::string, Source> sources_;
  std::set<std::string> retiredProcessEpochs_;
};

} // namespace corevideo::core
