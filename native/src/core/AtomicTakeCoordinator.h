#pragma once
#include "core/ShowPreparationTransaction.h"
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace corevideo::core {

// Control-plane reservation only. No renderer, clock, output, or SDK ownership.
class AtomicTakeCoordinator final {
 public:
  struct Transition {
    enum class Kind { Cut, Fade, Dip, Wipe };
    Kind kind = Kind::Cut;
    uint64_t durationNs = 0;
    std::string direction;
    std::string dipColor;
    bool operator==(const Transition&) const = default;
  };
  struct Fingerprint {
    uint64_t expectedRevision = 0, previewRevision = 0;
    std::string mediaProcessEpoch;
    uint64_t mediaGeneration = 0;
    Transition transition;
    // Prepared input basis remains at expectedRevision; the certificate's
    // planRevision is the prospective result (expectedRevision + 1). These are
    // different phases, not two increments of the authoritative revision.
    ShowPlanStamp expectedPlanStamp;
    std::string expectedPlanId;
    ShowPreparationToken preparation;
    bool operator==(const Fingerprint&) const = default;
  };
  struct Request {
    std::string authorityEpoch, operationId;
    Fingerprint fingerprint;
  };
  enum class Error {
    None, Invalid, AuthorityEpoch, OperationConflict, OperationExpired,
    Capacity, StaleRevision, StalePreview, NotPrepared, Busy, RevisionExhausted,
    ApplyFailed, NotApplied, StaleObservation
  };
  struct Outcome {
    Error error = Error::None;
    std::string authorityEpoch, operationId;
    bool pending = false;
    bool accepted = false, applied = false, rendered = false, delivered = false;
    uint64_t resultRevision = 0;
    std::string failure;
  };
  struct Reply { Outcome outcome; bool replayed = false; bool retryable = false; };
  struct ApplyResult { bool applied = false; std::string failure; };
  // Fast control-state CAS only: no SDK/GPU/I/O/waits. Preparation/resources must
  // already exist. False/exception must mean no control-state mutation occurred.
  // Lifetime: the owner must outlive every call, including the callback.
  using Apply = std::function<ApplyResult(Request, uint64_t reservedRevision)>;
  struct Snapshot {
    std::string authorityEpoch;
    uint64_t revision = 0, previewRevision = 0;
    size_t detailedResults = 0, tombstones = 0;
    bool applying = false;
  };

  AtomicTakeCoordinator(std::string authorityEpoch, uint64_t revision,
                        uint64_t previewRevision, size_t operationCapacity, Apply apply);
  // Preparation is an explicit certificate for this entire immutable fingerprint.
  // Only a completed ShowPreparationTransaction can issue the certificate.
  // The operation retains its leases through replay-ledger eviction.
  // One preparation transaction owner/monotonic transaction sequence per epoch.
  Error prepare(const std::string& authorityEpoch, const Fingerprint& fingerprint,
                std::shared_ptr<const PreparedShowCertificate> certificate);
  Error updatePreview(const std::string& authorityEpoch, uint64_t expectedRevision,
                      uint64_t previewRevision);
  // Busy is retryable and does not retain the new ID. Pending duplicates return
  // the same accepted operation identity, never applied/rendered/delivered success.
  Reply take(const Request& request);
  // Exact request+applied revision fences observations from old media/operations.
  // Delivered does not manufacture a missing rendered observation.
  Error observe(const Request& request, uint64_t appliedRevision, bool delivered);
  Error evict(const std::string& authorityEpoch, const std::string& operationId);
  [[nodiscard]] Snapshot snapshot() const;

 private:
  struct Record {
    Fingerprint fingerprint;
    Outcome outcome;
    std::shared_ptr<const PreparedShowCertificate> certificate;
    bool pending = false, pendingRendered = false, pendingDelivered = false;
  };
  static bool valid(const Fingerprint& fingerprint);
  static constexpr uint64_t kMaxRevision = 9007199254740991ULL;
  mutable std::mutex mutex_;
  const std::string epoch_;
  uint64_t revision_, previewRevision_;
  const size_t capacity_;
  const Apply apply_;
  std::optional<Fingerprint> prepared_;
  std::shared_ptr<const PreparedShowCertificate> preparedCertificate_;
  std::optional<Fingerprint> lastPreparation_;
  std::map<std::string, Record> records_;
  std::set<std::string> tombstones_;
  bool applying_ = false;
};
} // namespace corevideo::core
