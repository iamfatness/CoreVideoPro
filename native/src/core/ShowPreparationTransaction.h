#pragma once
#include "core/ShowPlanGenerator.h"
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <utility>

namespace corevideo::core {
struct PreparationAuthority {
  std::string epoch;
  std::uint64_t revision{0}, generation{1};
  bool operator==(const PreparationAuthority&) const = default;
};
struct ShowPreparationRequirement {
  enum class Kind { SourceSubscription, GpuResource, OutputReservation };
  Kind kind{Kind::SourceSubscription};
  std::string id;
  std::uint64_t generation{1};
  std::string ownerEpoch, ownerInstanceId;
  bool operator==(const ShowPreparationRequirement&) const = default;
};
struct ShowPreparationPlan {
  PreparationAuthority base;
  std::string id;
  std::uint64_t revision{0};
  std::vector<ShowPreparationRequirement> requirements;
  ShowPlanStamp stamp; // Immutable input basis; controlRevision == base.revision.
  bool operator==(const ShowPreparationPlan&) const = default;
};
struct ShowPreparationToken {
  PreparationAuthority base;
  std::string planId;
  std::uint64_t planRevision{0}, transactionGeneration{0};
  ShowPlanStamp stamp;
  bool operator==(const ShowPreparationToken&) const = default;
};
// Certificate shape only; owning transaction must also report completion.
bool validShowPreparationToken(const ShowPreparationToken& token);

struct ShowPreparationEvidence {
  enum class State { Pending, Ready, Failed };
  ShowPreparationToken token;
  ShowPreparationRequirement requirement;
  State state{State::Pending};
  // Ready requires an owning immutable lease, not a boolean latch. Destruction
  // must enqueue retirement if actual SDK/GPU teardown could block.
  std::shared_ptr<const void> lease;
};
class IShowResourcePreparer {
 public:
  virtual ~IShowResourcePreparer() = default;
  // Nonblocking, idempotent for token + requirement. May start external work,
  // but must return immediately. Cancellation fences late ready publications.
  virtual ShowPreparationEvidence poll(const ShowPreparationToken&,
                                      const ShowPreparationRequirement&) = 0;
  // Nonblocking and idempotent; valid before, during or after poll. Implementer
  // owns asynchronous cancellation and must reject subsequent work for this token.
  virtual void cancel(const ShowPreparationToken&) noexcept = 0;
};
struct AppliedPreparedShow {
  std::shared_ptr<const ShowPreparationPlan> plan;
  std::vector<std::shared_ptr<const void>> leases;
};
class PreparedShowCertificate final {
 public:
  enum class State { Ready, Claimed, Applying, Applied, Revoked };
  State state() const { return state_.load(); }
  const ShowPreparationToken token;
  const std::shared_ptr<const ShowPreparationPlan> plan;
  const std::vector<std::shared_ptr<const void>> leases;
  PreparedShowCertificate(const PreparedShowCertificate&) = delete;
  PreparedShowCertificate& operator=(const PreparedShowCertificate&) = delete;
 private:
  friend class ShowPreparationTransaction;
  friend class AtomicTakeCoordinator;
  bool claim() const { auto expected = State::Ready; return state_.compare_exchange_strong(expected, State::Claimed); }
  bool apply() const { auto expected = State::Claimed; return state_.compare_exchange_strong(expected, State::Applying); }
  void complete(bool success) const { state_.store(success ? State::Applied : State::Revoked); }
  bool revoke() const {
    auto value = state_.load();
    while (value == State::Ready || value == State::Claimed)
      if (state_.compare_exchange_weak(value, State::Revoked)) return true;
    return value == State::Revoked;
  }
  mutable std::atomic<State> state_{State::Ready};
  PreparedShowCertificate(ShowPreparationToken t, std::shared_ptr<const ShowPreparationPlan> p,
      std::vector<std::shared_ptr<const void>> l) : token(std::move(t)), plan(std::move(p)), leases(std::move(l)) {}
};
class ShowPreparationTransaction final {
 public:
  enum class Status { Idle, Preparing, Ready, Applied, Failed, Cancelled, Stale, Invalid, Busy, Exhausted };
  struct Result {
    Status status;
    ShowPreparationToken token;
    std::shared_ptr<const AppliedPreparedShow> applied;
    std::shared_ptr<const PreparedShowCertificate> certificate;
  };
  ShowPreparationTransaction(PreparationAuthority authority,
                             std::shared_ptr<IShowResourcePreparer> preparer,
                             std::size_t maxRequirements = 64,
                             std::size_t maxRetiredEpochs = 1'024);
  ~ShowPreparationTransaction();
  // Copies/freezes input before publication; only one pending transaction.
  Result begin(ShowPreparationPlan plan);
  Result poll();
  Result cancel(const ShowPreparationToken& token);
  Result snapshot() const;
  // Called by the authoritative state owner when external state supersedes this
  // base. CAS prevents older authority notifications from overwriting newer ones.
  // Previous applied snapshot remains visible with its original identity.
  bool commit(const ShowPreparationToken& token, const PreparationAuthority& expected,
              PreparationAuthority next, const std::shared_ptr<const PreparedShowCertificate>& certificate);
  bool advanceAuthority(const PreparationAuthority& expected,
                        PreparationAuthority next);
 private:
  struct Pending;
  Result resultLocked(Status status) const;
  mutable std::mutex mutex_;
  PreparationAuthority authority_;
  std::shared_ptr<IShowResourcePreparer> preparer_;
  std::shared_ptr<Pending> pending_;
  std::shared_ptr<const AppliedPreparedShow> applied_;
  std::shared_ptr<const ShowPreparationPlan> lastPlan_;
  ShowPreparationToken token_;
  std::shared_ptr<const PreparedShowCertificate> certificate_;
  Status status_{Status::Idle};
  std::uint64_t serial_{0};
  std::size_t maxRequirements_;
  std::size_t maxRetiredEpochs_;
  std::set<std::string> retiredEpochs_;
};
} // namespace corevideo::core
