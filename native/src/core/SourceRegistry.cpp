#include "core/SourceRegistry.h"

#include <stdexcept>
#include <utility>

namespace corevideo::core {

SourceRegistry::SourceRegistry(std::string registryEpoch, std::size_t maxPersons,
    std::size_t maxSources, std::size_t maxRetiredProcessEpochs)
    : epoch_(std::move(registryEpoch)), maxPersons_(maxPersons), maxSources_(maxSources),
      maxRetiredProcessEpochs_(maxRetiredProcessEpochs) {
  if (epoch_.empty() || epoch_.size() > 512 || maxPersons_ == 0 || maxSources_ == 0 ||
      maxRetiredProcessEpochs_ == 0)
    throw std::invalid_argument("SourceRegistry requires an authority epoch and positive capacities");
}

bool SourceRegistry::sameToken(const Token& a, const Token& b) {
  return a.sourceId.value == b.sourceId.value && a.instanceId.value == b.instanceId.value &&
      a.processEpoch == b.processEpoch && a.generation == b.generation;
}

SourceRegistry::Result SourceRegistry::upsertPerson(Person person) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (person.id.value.empty() || person.id.value.size() > 512 || person.displayName.size() > 4096 ||
      person.generation == 0 || person.generation > kMaxRevision)
    return Result::Invalid;
  const auto existing = persons_.find(person.id.value);
  if (existing != persons_.end()) {
    if (person.generation < existing->second.generation) return Result::Stale;
    if (existing->second.displayName == person.displayName &&
        existing->second.generation == person.generation) return Result::Unchanged;
  }
  if (existing == persons_.end() && persons_.size() >= maxPersons_) return Result::Exhausted;
  if (revision_ == kMaxRevision) return Result::Exhausted;
  const auto key = person.id.value;
  const bool identityChanged = existing == persons_.end() || existing->second.generation != person.generation;
  persons_[key] = std::move(person);
  ++revision_;
  if (identityChanged) ++decisionRevision_;
  return Result::Applied;
}

bool SourceRegistry::validRegistration(const Registration& r) const {
  const bool knownKind = r.kind == Kind::ParticipantVideo || r.kind == Kind::ParticipantShare ||
      r.kind == Kind::Device || r.kind == Kind::Media || r.kind == Kind::Browser;
  return knownKind && (!r.requestedGeneration || (*r.requestedGeneration > 0 && *r.requestedGeneration <= kMaxRevision)) &&
      (!r.instanceId || (!r.instanceId->value.empty() && r.instanceId->value.size() <= 512)) &&
      !r.sourceId.value.empty() && r.sourceId.value.size() <= 512 &&
      !r.processEpoch.empty() && r.processEpoch.size() <= 512 &&
      !retiredProcessEpochs_.contains(r.processEpoch) && !r.externalId.empty() &&
      r.externalId.size() <= 512 && r.displayName.size() <= 4096 &&
      ((!r.personId && r.personGeneration == 0) ||
       (r.personId && persons_.contains(r.personId->value) && r.personGeneration > 0 &&
        persons_.at(r.personId->value).generation == r.personGeneration));
}

bool SourceRegistry::externalConflict(const Registration& r) const {
  for (const auto& entry : sources_) {
    const auto& source = entry.second;
    if (entry.first != r.sourceId.value && source.availability != Availability::Departed &&
        r.instanceId && source.token.instanceId.value == r.instanceId->value) return true;
    if (entry.first != r.sourceId.value && source.availability != Availability::Departed &&
        source.kind == r.kind && source.token.processEpoch == r.processEpoch && source.externalId == r.externalId)
      return true;
  }
  return false;
}

SourceRegistry::Mutation SourceRegistry::install(Registration r, uint64_t generation) {
  if (revision_ == kMaxRevision || generation > kMaxRevision) return {Result::Exhausted, {}};
  Source source;
  source.token = {r.sourceId, r.instanceId.value_or(SourceInstanceId{epoch_ + ":" + std::to_string(revision_ + 1)}), r.processEpoch, generation};
  for (const auto& [id, existing] : sources_) {
    if (id != r.sourceId.value && existing.availability != Availability::Departed &&
        existing.token.instanceId.value == source.token.instanceId.value) return {Result::Conflict, {}};
  }
  source.kind = r.kind;
  source.personId = std::move(r.personId);
  source.personGeneration = r.personGeneration;
  source.displayName = std::move(r.displayName);
  source.externalId = std::move(r.externalId);
  const auto token = source.token;
  sources_[r.sourceId.value] = std::move(source);
  ++revision_;
  ++decisionRevision_;
  return {Result::Applied, token};
}

SourceRegistry::Mutation SourceRegistry::add(Registration r) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!validRegistration(r)) return {Result::Invalid, {}};
  if (sources_.find(r.sourceId.value) != sources_.end() || externalConflict(r)) return {Result::Conflict, {}};
  if (sources_.size() >= maxSources_) return {Result::Exhausted, {}};
  const auto generation = r.requestedGeneration.value_or(1);
  return install(std::move(r), generation);
}

SourceRegistry::Mutation SourceRegistry::replace(const Token& expected, Registration r) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = sources_.find(expected.sourceId.value);
  if (found == sources_.end()) return {Result::NotFound, {}};
  if (!sameToken(found->second.token, expected)) return {Result::Stale, {}};
  if (!validRegistration(r) || r.sourceId.value != expected.sourceId.value || r.kind != found->second.kind)
    return {Result::Invalid, {}};
  if (externalConflict(r)) return {Result::Conflict, {}};
  if (r.requestedGeneration && *r.requestedGeneration <= expected.generation) return {Result::Stale, {}};
  const auto generation = r.requestedGeneration.value_or(expected.generation + 1);
  return install(std::move(r), generation);
}

SourceRegistry::Result SourceRegistry::setDisplayName(const Token& token, const std::string& name) {
  // Empty labels are valid metadata; names are never identifiers.
  if (name.size() > 4096) return Result::Invalid;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = sources_.find(token.sourceId.value);
  if (found == sources_.end()) return Result::NotFound;
  auto& source = found->second;
  if (!sameToken(source.token, token) || source.availability == Availability::Departed)
    return Result::Stale;
  if (source.displayName == name) return Result::Unchanged;
  if (revision_ == kMaxRevision) return Result::Exhausted;
  source.displayName = name;
  ++revision_;
  return Result::Applied;
}

SourceRegistry::Result SourceRegistry::setAvailability(const Token& token, Availability availability) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = sources_.find(token.sourceId.value);
  if (found == sources_.end()) return Result::NotFound;
  auto& source = found->second;
  if (!sameToken(source.token, token) || source.availability == Availability::Departed) return Result::Stale;
  if (availability != Availability::Available && availability != Availability::Unavailable && availability != Availability::Departed)
    return Result::Invalid;
  if (source.availability == availability) return Result::Unchanged;
  if (revision_ == kMaxRevision) return Result::Exhausted;
  source.availability = availability;
  ++decisionRevision_;
  if (availability != Availability::Available) {
    source.subscriptionObserved = false;
    source.format.reset();
    source.hasPublication = false;
    // Keep the incarnation's publication watermark across camera off/on.
  }
  ++revision_;
  return Result::Applied;
}

SourceRegistry::Result SourceRegistry::setSubscription(const Token& token, bool requested, bool observed) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = sources_.find(token.sourceId.value);
  if (found == sources_.end()) return Result::NotFound;
  auto& source = found->second;
  if (!sameToken(source.token, token) || source.availability == Availability::Departed) return Result::Stale;
  if (observed && source.availability != Availability::Available) return Result::Invalid;
  if (source.subscriptionRequested == requested && source.subscriptionObserved == observed)
    return Result::Unchanged;
  if (revision_ == kMaxRevision) return Result::Exhausted;
  source.subscriptionRequested = requested;
  source.subscriptionObserved = observed;
  ++revision_;
  return Result::Applied;
}

SourceRegistry::Result SourceRegistry::retireProcessEpoch(const std::string& processEpoch) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (processEpoch.empty() || processEpoch.size() > 512) return Result::Invalid;
  if (retiredProcessEpochs_.contains(processEpoch)) return Result::Unchanged;
  if (retiredProcessEpochs_.size() >= maxRetiredProcessEpochs_) return Result::Exhausted;
  if (revision_ == kMaxRevision) return Result::Exhausted;
  // Persist the fence even if retirement wins the race with the first roster
  // callback. A delayed add from a dead helper must never look current.
  retiredProcessEpochs_.insert(processEpoch);
  ++decisionRevision_;
  for (auto& entry : sources_) {
    auto& source = entry.second;
    if (source.token.processEpoch != processEpoch) continue;
    source.availability = Availability::Departed;
    source.subscriptionRequested = false;
    source.subscriptionObserved = false;
    source.format.reset();
    source.hasPublication = false;
    // Retired tokens retain their publication watermark for diagnostics.
  }
  ++revision_;
  return Result::Applied;
}

SourceRegistry::Result SourceRegistry::publish(const Token& token, uint64_t sequence, int64_t observedNs, Format format) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = sources_.find(token.sourceId.value);
  if (found == sources_.end()) return Result::NotFound;
  auto& source = found->second;
  if (!sameToken(source.token, token) || source.availability != Availability::Available) return Result::Stale;
  if (sequence > kMaxRevision || observedNs < 0 || format.width <= 0 || format.height <= 0 ||
      format.fpsNumerator <= 0 || format.fpsDenominator <= 0 || format.pixelFormat.empty() ||
      format.pixelFormat.size() > 128) return Result::Invalid;
  if (source.hasPublicationWatermark &&
      (sequence <= source.publicationSequence || observedNs < source.lastPublicationNs)) return Result::Stale;
  if (revision_ == kMaxRevision) return Result::Exhausted;
  if (!source.hasPublication) ++decisionRevision_;
  source.format = std::move(format);
  source.hasPublication = true;
  source.hasPublicationWatermark = true;
  source.publicationSequence = sequence;
  source.lastPublicationNs = observedNs;
  ++revision_;
  return Result::Applied;
}

std::shared_ptr<const SourceRegistry::Snapshot> SourceRegistry::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = std::make_shared<Snapshot>();
  result->registryEpoch = epoch_;
  result->revision = revision_;
  result->decisionRevision = decisionRevision_;
  for (const auto& entry : persons_) result->persons.push_back(entry.second);
  for (const auto& entry : sources_) result->sources.push_back(entry.second);
  return result;
}

std::vector<PersonId> SourceRegistry::peopleNamed(const std::string& displayName) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<PersonId> result;
  for (const auto& entry : persons_) if (entry.second.displayName == displayName) result.push_back(entry.second.id);
  return result;
}

} // namespace corevideo::core
