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
  // Composed: a source the CORE renders rather than captures (the Tiles wall
  // today; lower-thirds and graphics in slice 3). It has no SDK handle, no
  // person, and is never subscribed - so the capture-only fields below stay
  // nullopt for it, and "nullopt" means NOT APPLICABLE, never false.
  enum class Kind { ParticipantVideo, ParticipantShare, Device, Media, Browser, Composed };
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
    std::string displayName;
    // nullopt = NOT APPLICABLE to this kind (e.g. a Composed wall). Never read as false.
    std::optional<std::string> externalId;
    std::optional<Availability> availability;
    std::optional<bool> subscriptionRequested;
    std::optional<bool> subscriptionObserved; // nullopt means unacknowledged/unknown/not applicable.
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
    // Stable SourceId order, includes departure tombstones for every non-Composed
    // kind (Availability::Departed, kept for diagnostics). A Composed source is
    // the exception: it never tombstones, it VANISHES via removeComposed() -
    // see that method's comment for why a wall cannot be tombstoned at all.
    std::vector<Source> sources;
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
  // Composed-only: erases the source outright rather than tombstoning it.
  // Every other kind's departure is Availability::Departed, kept deliberately
  // (retireProcessEpoch, setAvailability) so a late callback or a diagnostic
  // read can still see what a provider incarnation was. A composed source has
  // no provider process and no callback to fence against - its lifetime is
  // "named by a live scene" (parent spec section 2), so once nothing names it
  // the tombstone would just be a permanent, meaningless entry, and it would
  // make add() answer Conflict forever for a wall id that is free to reuse.
  // Refuses (Invalid) for any other kind: erasing a real source's record is
  // the false-erasure this registry exists to prevent from the other direction.
  // Takes a bare SourceId, deliberately not a Token: the caller must be the
  // SOLE owner of a composed source's lifetime. replace() exists in this class
  // precisely so an old callback cannot retire a new instance it no longer
  // owns (compare-and-replace against the expected Token) - removal has no
  // such fence, so it must never grow a second writer.
  Result removeComposed(const SourceId& sourceId);
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
