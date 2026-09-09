#include "core/AtomicTakeCoordinator.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace corevideo::core {
AtomicTakeCoordinator::AtomicTakeCoordinator(std::string epoch, uint64_t revision,
    uint64_t previewRevision, size_t capacity, Apply apply)
    : epoch_(std::move(epoch)), revision_(revision), previewRevision_(previewRevision),
      capacity_(capacity), apply_(std::move(apply)) {
  if (epoch_.empty() || revision > kMaxRevision || previewRevision > kMaxRevision || !capacity || !apply_)
    throw std::invalid_argument("Invalid Take coordinator configuration");
}

bool AtomicTakeCoordinator::valid(const Fingerprint& f) {
  const auto validColor = [](const std::string& value) {
    return value.size() == 7 && value.front() == '#' &&
        std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
  };
  const bool validDirection = f.transition.direction == "left-to-right" ||
      f.transition.direction == "right-to-left" || f.transition.direction == "top-to-bottom" ||
      f.transition.direction == "bottom-to-top";
  const bool knownKind = f.transition.kind == Transition::Kind::Cut ||
      f.transition.kind == Transition::Kind::Fade || f.transition.kind == Transition::Kind::Dip ||
      f.transition.kind == Transition::Kind::Wipe;
  const bool timed = f.transition.durationNs > 0 && f.transition.durationNs <= 5'000'000'000ULL;
  const bool transition = knownKind &&
      ((f.transition.kind == Transition::Kind::Cut && f.transition.durationNs == 0 &&
        f.transition.direction.empty() && f.transition.dipColor.empty()) ||
       (f.transition.kind == Transition::Kind::Fade && timed &&
        f.transition.direction.empty() && f.transition.dipColor.empty()) ||
       (f.transition.kind == Transition::Kind::Dip && timed &&
        f.transition.direction.empty() && validColor(f.transition.dipColor)) ||
       (f.transition.kind == Transition::Kind::Wipe && timed && validDirection &&
        f.transition.dipColor.empty()));
  // Bound ledger strings as well as entry count. IDs are opaque UTF-8 bytes.
  return f.expectedRevision <= kMaxRevision && f.previewRevision <= kMaxRevision &&
      !f.mediaProcessEpoch.empty() && f.mediaProcessEpoch.size() <= 512 &&
      f.mediaGeneration > 0 && f.mediaGeneration <= kMaxRevision && transition &&
      !f.preparationToken.empty() && f.preparationToken.size() <= 512 && f.preparationRevision <= kMaxRevision;
}

AtomicTakeCoordinator::Error AtomicTakeCoordinator::prepare(const std::string& epoch, const Fingerprint& f) {
  std::lock_guard lock(mutex_);
  if (epoch != epoch_) return Error::AuthorityEpoch;
  if (!valid(f)) return Error::Invalid;
  if (applying_) return Error::Busy;
  if (f.expectedRevision != revision_) return Error::StaleRevision;
  if (f.previewRevision != previewRevision_) return Error::StalePreview;
  if (lastPreparation_) {
    if (f.preparationRevision < lastPreparation_->preparationRevision) return Error::NotPrepared;
    if (f.preparationRevision == lastPreparation_->preparationRevision) {
      if (!(f == *lastPreparation_)) return Error::OperationConflict;
      return prepared_ ? Error::None : Error::NotPrepared;
    }
    if (f.preparationToken == lastPreparation_->preparationToken) return Error::OperationConflict;
  }
  lastPreparation_ = f;
  prepared_ = f;
  return Error::None;
}

AtomicTakeCoordinator::Error AtomicTakeCoordinator::updatePreview(const std::string& epoch,
    uint64_t expectedRevision, uint64_t previewRevision) {
  std::lock_guard lock(mutex_);
  if (epoch != epoch_) return Error::AuthorityEpoch;
  if (previewRevision > kMaxRevision) return Error::Invalid;
  if (applying_) return Error::Busy;
  if (expectedRevision != revision_) return Error::StaleRevision;
  if (previewRevision < previewRevision_) return Error::StalePreview;
  if (previewRevision == previewRevision_) return Error::None;
  if (revision_ == kMaxRevision) return Error::RevisionExhausted;
  previewRevision_ = previewRevision;
  ++revision_;
  prepared_.reset();
  return Error::None;
}

AtomicTakeCoordinator::Reply AtomicTakeCoordinator::take(const Request& request) {
  const Request frozen = request;
  uint64_t reservedRevision = 0;
  {
    std::lock_guard lock(mutex_);
    const auto reject = [&](Error error) {
      Outcome out; out.error = error; out.resultRevision = revision_;
      out.authorityEpoch = frozen.authorityEpoch; out.operationId = frozen.operationId;
      return Reply{out, false, error == Error::Busy};
    };
    if (frozen.authorityEpoch != epoch_) return reject(Error::AuthorityEpoch);
    if (frozen.operationId.empty() || frozen.operationId.size() > 512 || !valid(frozen.fingerprint)) return reject(Error::Invalid);
    if (tombstones_.contains(frozen.operationId)) return reject(Error::OperationExpired);
    const auto old = records_.find(frozen.operationId);
    if (old != records_.end()) {
      if (!(old->second.fingerprint == frozen.fingerprint)) return reject(Error::OperationConflict);
      return {old->second.outcome, true};
    }
    if (records_.size() + tombstones_.size() >= capacity_) return reject(Error::Capacity);
    if (applying_) return reject(Error::Busy);
    if (frozen.fingerprint.expectedRevision != revision_) return reject(Error::StaleRevision);
    if (frozen.fingerprint.previewRevision != previewRevision_) return reject(Error::StalePreview);
    if (revision_ == kMaxRevision) return reject(Error::RevisionExhausted);
    if (!prepared_ || !(*prepared_ == frozen.fingerprint)) return reject(Error::NotPrepared);
    reservedRevision = revision_ + 1;
    Outcome accepted;
    accepted.authorityEpoch = epoch_;
    accepted.operationId = frozen.operationId;
    accepted.pending = true;
    accepted.accepted = true;
    accepted.resultRevision = reservedRevision;
    records_.emplace(frozen.operationId, Record{frozen.fingerprint, accepted, true});
    applying_ = true;
    prepared_.reset();
  }
  ApplyResult applied;
  // No owner lock across foreign code. Concurrent/reentrant duplicate calls see
  // accepted/pending; other mutations cannot overtake this reservation.
  try { applied = apply_(frozen, reservedRevision); }
  catch (const std::exception& error) { applied.failure = error.what(); }
  catch (...) { applied.failure = "Unknown Take apply failure"; }
  std::lock_guard lock(mutex_);
  auto& record = records_.at(frozen.operationId);
  record.pending = false;
  record.outcome.pending = false;
  record.outcome.applied = applied.applied;
  if (applied.applied) {
    revision_ = reservedRevision;
    record.outcome.rendered = record.pendingRendered;
    record.outcome.delivered = record.pendingDelivered;
  }
  else {
    record.outcome.error = Error::ApplyFailed;
    record.outcome.failure = applied.failure.substr(0, 512);
    record.outcome.resultRevision = revision_;
    record.pendingRendered = false;
    record.pendingDelivered = false;
  }
  applying_ = false;
  return {record.outcome, false};
}

AtomicTakeCoordinator::Error AtomicTakeCoordinator::observe(const Request& request,
    uint64_t appliedRevision, bool delivered) {
  std::lock_guard lock(mutex_);
  if (request.authorityEpoch != epoch_) return Error::AuthorityEpoch;
  if (tombstones_.contains(request.operationId)) return Error::OperationExpired;
  const auto found = records_.find(request.operationId);
  if (found == records_.end()) return Error::StaleObservation;
  auto& record = found->second;
  if (!(record.fingerprint == request.fingerprint) || record.outcome.resultRevision != appliedRevision)
    return Error::StaleObservation;
  if (record.pending) {
    if (delivered && !record.pendingRendered) return Error::NotApplied;
    if (delivered) record.pendingDelivered = true;
    else record.pendingRendered = true;
    return Error::None;
  }
  if (!record.outcome.applied || (delivered && !record.outcome.rendered)) return Error::NotApplied;
  if (delivered) record.outcome.delivered = true;
  else record.outcome.rendered = true;
  return Error::None;
}

AtomicTakeCoordinator::Error AtomicTakeCoordinator::evict(const std::string& epoch, const std::string& id) {
  std::lock_guard lock(mutex_);
  if (epoch != epoch_) return Error::AuthorityEpoch;
  if (tombstones_.contains(id)) return Error::None;
  const auto found = records_.find(id);
  if (found == records_.end()) return Error::StaleObservation;
  if (found->second.pending) return Error::Busy;
  tombstones_.insert(id); // Allocate before removal: failure cannot lose protection.
  records_.erase(found);
  return Error::None;
}

AtomicTakeCoordinator::Snapshot AtomicTakeCoordinator::snapshot() const {
  std::lock_guard lock(mutex_);
  return {epoch_, revision_, previewRevision_, records_.size(), tombstones_.size(), applying_};
}
} // namespace corevideo::core
