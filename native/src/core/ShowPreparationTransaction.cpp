#include "core/ShowPreparationTransaction.h"
#include <set>
#include <stdexcept>
#include <utility>

namespace corevideo::core {
namespace {
constexpr std::uint64_t maxSafe = 9'007'199'254'740'991ULL;
bool validText(const std::string& s) { return !s.empty() && s.size() <= 256; }
bool validAuthority(const PreparationAuthority& a) {
  return validText(a.epoch) && a.revision <= maxSafe && a.generation > 0 && a.generation <= maxSafe;
}
bool validStamp(const ShowPlanStamp& s, const PreparationAuthority& base) {
  return s.authorityEpoch == base.epoch && validText(s.registryEpoch) &&
      s.controlRevision == base.revision && s.registryRevision <= maxSafe &&
      !s.eligibilityIdentity.empty() && s.eligibilityIdentity.size() <= 4 * 1024 * 1024;
}
bool validPlan(const ShowPreparationPlan& p, std::size_t limit) {
  if (!validAuthority(p.base) || !validText(p.id) || p.base.revision == maxSafe ||
      p.revision != p.base.revision + 1 || !validStamp(p.stamp, p.base) || p.requirements.size() > limit) return false;
  std::set<std::pair<int, std::string>> ids;
  for (const auto& r : p.requirements) {
    if (r.kind != ShowPreparationRequirement::Kind::SourceSubscription &&
        r.kind != ShowPreparationRequirement::Kind::GpuResource &&
        r.kind != ShowPreparationRequirement::Kind::OutputReservation) return false;
    if (!validText(r.id) || !validText(r.ownerEpoch) || !validText(r.ownerInstanceId) || r.generation == 0 || r.generation > maxSafe ||
        !ids.emplace(static_cast<int>(r.kind), r.id).second) return false;
  }
  return true;
}
}
bool validShowPreparationToken(const ShowPreparationToken& t) {
  return validAuthority(t.base) && validText(t.planId) && t.base.revision < maxSafe &&
      t.planRevision == t.base.revision + 1 && t.transactionGeneration > 0 &&
      t.transactionGeneration <= maxSafe && validStamp(t.stamp, t.base);
}
struct ShowPreparationTransaction::Pending {
  std::shared_ptr<const ShowPreparationPlan> plan;
  ShowPreparationToken token;
  std::vector<std::shared_ptr<const void>> leases;
  bool polling{false};
};
ShowPreparationTransaction::ShowPreparationTransaction(PreparationAuthority authority,
    std::shared_ptr<IShowResourcePreparer> preparer, std::size_t limit,
    std::size_t maxRetiredEpochs)
    : authority_(std::move(authority)), preparer_(std::move(preparer)),
      maxRequirements_(limit), maxRetiredEpochs_(maxRetiredEpochs) {
  if (!validAuthority(authority_) || !preparer_ || limit == 0 || limit > 4096 ||
      maxRetiredEpochs == 0 || maxRetiredEpochs > 65'536)
    throw std::invalid_argument("Invalid preparation coordinator configuration");
}
ShowPreparationTransaction::~ShowPreparationTransaction() {
  // Owner must stop calling methods before destruction. No worker/thread join.
  if (pending_) preparer_->cancel(pending_->token);
}
ShowPreparationTransaction::Result ShowPreparationTransaction::resultLocked(Status s) const {
  return {s, token_, applied_, (s == Status::Ready || s == Status::Applied) ? certificate_ : nullptr};
}
ShowPreparationTransaction::Result ShowPreparationTransaction::snapshot() const {
  std::lock_guard lock(mutex_); return resultLocked(status_);
}
ShowPreparationTransaction::Result ShowPreparationTransaction::begin(ShowPreparationPlan plan) {
  if (!validPlan(plan, maxRequirements_)) { auto r = snapshot(); r.status = Status::Invalid; r.certificate.reset(); return r; }
  auto frozen = std::make_shared<const ShowPreparationPlan>(std::move(plan));
  auto pending = std::make_shared<Pending>(); pending->plan = frozen;
  pending->leases.resize(frozen->requirements.size());
  std::shared_ptr<const ShowPreparationPlan> retired;
  std::shared_ptr<const PreparedShowCertificate> retiredCertificate;
  std::lock_guard lock(mutex_);
  // Replay only the same authority/plan transaction; a new epoch invalidates it.
  if (status_ != Status::Cancelled && status_ != Status::Failed && lastPlan_ && *lastPlan_ == *frozen && authority_.epoch == frozen->base.epoch &&
      authority_.generation == frozen->base.generation &&
      authority_.revision == (status_ == Status::Applied ? frozen->revision : frozen->base.revision)) return resultLocked(status_);
  if (pending_) return resultLocked(Status::Busy);
  if (!(authority_ == frozen->base)) return resultLocked(Status::Stale);
  if (certificate_ && status_ == Status::Ready && !certificate_->revoke()) return resultLocked(Status::Busy);
  if (serial_ == maxSafe) return resultLocked(Status::Exhausted);
  pending->token = {frozen->base, frozen->id, frozen->revision, ++serial_, frozen->stamp};
  retiredCertificate = std::move(certificate_);
  token_ = pending->token; retired = std::move(lastPlan_); lastPlan_ = frozen;
  pending_ = std::move(pending); status_ = Status::Preparing;
  return resultLocked(status_);
}
ShowPreparationTransaction::Result ShowPreparationTransaction::poll() {
  std::shared_ptr<Pending> pending;
  {
    std::lock_guard lock(mutex_);
    if (!pending_ || pending_->polling) return resultLocked(status_);
    pending = pending_; pending->polling = true;
  }
  auto leases = pending->leases; // Single poll owns pending data outside the lock.
  bool failed = false, complete = true;
  for (std::size_t i = 0; i < leases.size(); ++i) {
    if (leases[i]) continue;
    try {
      const auto evidence = preparer_->poll(pending->token, pending->plan->requirements[i]);
      if (!(evidence.token == pending->token) || !(evidence.requirement == pending->plan->requirements[i])) { failed = true; break; }
      if (evidence.state == ShowPreparationEvidence::State::Ready && evidence.lease) leases[i] = evidence.lease;
      else if (evidence.state == ShowPreparationEvidence::State::Pending) complete = false;
      else { failed = true; break; }
    } catch (...) { failed = true; break; }
    // Cancellation may happen during a preparer callback. Do not issue more work.
    { std::lock_guard lock(mutex_); if (pending_ != pending) break; }
  }
  std::shared_ptr<const PreparedShowCertificate> prepared;
  if (complete && !failed) prepared.reset(new PreparedShowCertificate(pending->token, pending->plan, leases));
  std::shared_ptr<const AppliedPreparedShow> retired;
  Result result;
  bool cancelResources = false;
  {
    std::lock_guard lock(mutex_);
    if (pending_ != pending) return resultLocked(status_);
    pending->polling = false;
    if (!(authority_ == pending->plan->base)) { status_ = Status::Stale; cancelResources = true; }
    else if (failed) { status_ = Status::Failed; cancelResources = true; }
    else if (complete) {
      certificate_ = std::move(prepared); status_ = Status::Ready;
    } else pending->leases = std::move(leases);
    if (status_ != Status::Preparing) pending_.reset();
    result = resultLocked(status_);
  }
  if (cancelResources) preparer_->cancel(pending->token);
  return result;
}
ShowPreparationTransaction::Result ShowPreparationTransaction::cancel(const ShowPreparationToken& token) {
  std::shared_ptr<Pending> retired;
  std::shared_ptr<const PreparedShowCertificate> retiredCertificate;
  Result result;
  {
    std::lock_guard lock(mutex_);
    if (!(token == token_)) return resultLocked(Status::Stale);
    if (!pending_) {
      if (status_ != Status::Ready) return resultLocked(status_);
      if (!certificate_->revoke()) return resultLocked(Status::Busy);
      retiredCertificate = std::move(certificate_); status_ = Status::Cancelled;
      result = resultLocked(status_);
    } else {
      retired = std::move(pending_); status_ = Status::Cancelled; result = resultLocked(status_);
    }
  }
  preparer_->cancel(token); return result;
}
bool ShowPreparationTransaction::commit(const ShowPreparationToken& token, const PreparationAuthority& expected,
    PreparationAuthority next, const std::shared_ptr<const PreparedShowCertificate>& certificate) {
  std::shared_ptr<const AppliedPreparedShow> retired;
  std::lock_guard lock(mutex_);
  if (!validAuthority(next) || !certificate || certificate != certificate_ || token != token_ ||
      certificate->token != token || certificate->state() != PreparedShowCertificate::State::Applied ||
      token.base != expected || next.epoch != expected.epoch || next.generation != expected.generation ||
      next.revision != token.planRevision) return false;
  if (status_ == Status::Applied && authority_ == next) return true;
  if (status_ != Status::Ready || authority_ != expected) return false;
  auto applied = std::make_shared<const AppliedPreparedShow>(AppliedPreparedShow{certificate->plan, certificate->leases});
  retired = std::move(applied_); applied_ = std::move(applied);
  authority_ = std::move(next); status_ = Status::Applied; return true;
}
bool ShowPreparationTransaction::advanceAuthority(const PreparationAuthority& expected, PreparationAuthority next) {
  if (!validAuthority(next)) return false;
  std::shared_ptr<Pending> retired;
  std::shared_ptr<const AppliedPreparedShow> oldApplied;
  {
    std::lock_guard lock(mutex_);
    if (!(authority_ == expected)) return false;
    if (next.epoch == authority_.epoch &&
        (next.generation < authority_.generation || next.revision < authority_.revision)) return false;
    if (next.epoch != authority_.epoch) {
      if (retiredEpochs_.contains(next.epoch)) return false;
      if (retiredEpochs_.size() >= maxRetiredEpochs_) return false;
    }
    if (next == authority_) return true;
    if (certificate_ && status_ == Status::Ready && !certificate_->revoke()) return false;
    if (next.epoch != authority_.epoch) retiredEpochs_.insert(authority_.epoch);
    authority_ = std::move(next); retired = std::move(pending_); status_ = Status::Stale;
  }
  if (retired) preparer_->cancel(retired->token);
  return true;
}
} // namespace corevideo::core
