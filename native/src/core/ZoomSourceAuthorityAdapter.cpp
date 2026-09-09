#include "core/ZoomSourceAuthorityAdapter.h"
#include <algorithm>
#include <tuple>

namespace corevideo::core {
namespace {
constexpr uint64_t maxSafe = 9007199254740991ULL;
bool text(const std::string& s) { return !s.empty() && s.size() <= 512; }
bool identityChanged(const ZoomSourceAuthorityAdapter::Source& a, const ZoomSourceAuthorityAdapter::Source& b) {
  return a.instanceId != b.instanceId || a.externalId != b.externalId || a.personId != b.personId ||
      a.personGeneration != b.personGeneration || a.kind != b.kind;
}
}
ZoomSourceAuthorityAdapter::ZoomSourceAuthorityAdapter(std::string epoch, size_t people, size_t sources, size_t epochs)
    : registry_(std::move(epoch), people, sources, epochs), maxPeople_(people), maxSources_(sources), maxEpochs_(epochs) {}
std::shared_ptr<const SourceRegistry::Snapshot> ZoomSourceAuthorityAdapter::snapshot() const {
  std::lock_guard lock(mutex_); return registry_.snapshot();
}
ZoomSourceAuthorityAdapter::SyncResult ZoomSourceAuthorityAdapter::sync(Observation observation) {
  // Canonicalize before comparing: SDK roster position is never identity.
  std::sort(observation.people.begin(), observation.people.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
  std::sort(observation.sources.begin(), observation.sources.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
  std::lock_guard lock(mutex_);
  const auto result = [&](Status status, SourceRegistry::Result detail = SourceRegistry::Result::Unchanged) {
    return SyncResult{status, detail, registry_.snapshot(), false};
  };
  if (!text(observation.processEpoch) || observation.sequence > maxSafe) return result(Status::Invalid);
  if (retiredEpochs_.contains(observation.processEpoch)) return result(Status::Stale);
  const bool newEpoch = last_ && last_->processEpoch != observation.processEpoch;
  if (last_ && !newEpoch) {
    if (observation.sequence < last_->sequence) return result(Status::Stale);
    if (observation.sequence == last_->sequence)
      return result(observation == *last_ ? Status::Unchanged : Status::Stale);
  }
  if (observation.people.size() > maxPeople_ || observation.sources.size() > maxSources_)
    return result(Status::Capacity);
  std::map<std::string, uint64_t> people;
  auto nextPeople = peopleIds_; auto nextSources = sourceIds_;
  for (const auto& person : observation.people) {
    if (!text(person.id) || person.name.size() > 4096 || !person.generation || person.generation > maxSafe ||
        !people.emplace(person.id, person.generation).second) return result(Status::Invalid);
    nextPeople.insert(person.id);
  }
  const auto current = registry_.snapshot();
  for (const auto& person : current->persons)
    if (people.contains(person.id.value) && people.at(person.id.value) < person.generation) return result(Status::Stale);
  std::set<std::string> sourceIds;
  std::set<std::string> instanceIds;
  std::set<std::pair<int, std::string>> externalIds;
  for (const auto& source : observation.sources) {
    if (!text(source.id) || !text(source.instanceId) || !instanceIds.insert(source.instanceId).second ||
        !text(source.externalId) || source.name.size() > 4096 ||
        !source.incarnation || source.incarnation > maxSafe ||
        (source.kind != SourceRegistry::Kind::ParticipantVideo && source.kind != SourceRegistry::Kind::ParticipantShare) ||
        !sourceIds.insert(source.id).second || !externalIds.emplace(static_cast<int>(source.kind), source.externalId).second ||
        (source.personId.empty() ? source.personGeneration != 0 :
         !people.contains(source.personId) || people.at(source.personId) != source.personGeneration) ||
        (source.subscriptionObserved && !source.videoAvailable)) return result(Status::Invalid);
    if (source.publication) {
      const auto& p = *source.publication;
      if (!source.videoAvailable || p.sequence > maxSafe || p.observedNs < 0 || p.width <= 0 || p.height <= 0 ||
          p.fpsNumerator <= 0 || p.fpsDenominator <= 0 || !text(p.pixelFormat)) return result(Status::Invalid);
    }
    const auto old = bindings_.find(source.id);
    if (old != bindings_.end()) {
      if (source.kind != old->second.observation.kind) return result(Status::Invalid);
      const bool sameProcess = old->second.token.processEpoch == observation.processEpoch;
      if (!sameProcess && source.incarnation <= old->second.token.generation) return result(Status::Stale);
      if (sameProcess && (source.incarnation < old->second.observation.incarnation ||
          ((!old->second.present || identityChanged(source, old->second.observation)) &&
           source.incarnation <= old->second.observation.incarnation))) return result(Status::Stale);
      if (sameProcess && source.incarnation == old->second.observation.incarnation && source.publication && old->second.observation.publication) {
        const auto& p = *source.publication; const auto& q = *old->second.observation.publication;
        if (p.sequence < q.sequence || p.observedNs < q.observedNs || (p.sequence == q.sequence && !(p == q))) return result(Status::Stale);
      }
    }
    nextSources.insert(source.id);
  }
  if (nextPeople.size() > maxPeople_ || nextSources.size() > maxSources_ || (newEpoch && retiredEpochs_.size() >= maxEpochs_))
    return result(Status::Capacity);
  const auto accepted = [](SourceRegistry::Result r) { return r == SourceRegistry::Result::Applied || r == SourceRegistry::Result::Unchanged; };
  if (newEpoch) {
    const auto r = registry_.retireProcessEpoch(last_->processEpoch);
    if (!accepted(r)) return result(Status::RegistryFailure, r);
    retiredEpochs_.insert(last_->processEpoch);
    for (auto& [id, binding] : bindings_) binding.present = false;
  }
  for (const auto& person : observation.people) {
    const auto r = registry_.upsertPerson({{person.id}, person.name, person.generation});
    if (!accepted(r)) return result(Status::RegistryFailure, r);
  }
  // Retire all removals before adding reused external handles.
  for (auto& [id, binding] : bindings_) if (binding.present && !sourceIds.contains(id)) {
    const auto r = registry_.setAvailability(binding.token, SourceRegistry::Availability::Departed);
    if (!accepted(r)) return result(Status::RegistryFailure, r);
    binding.present = false;
  }
  // Retire every replaced incarnation first, so valid simultaneous external-ID
  // swaps do not depend on lexicographic installation order.
  for (const auto& source : observation.sources) {
    auto old = bindings_.find(source.id);
    if (old != bindings_.end() && old->second.present && source.incarnation != old->second.observation.incarnation) {
      const auto r = registry_.setAvailability(old->second.token, SourceRegistry::Availability::Departed);
      if (!accepted(r)) return result(Status::RegistryFailure, r);
      old->second.present = false;
    }
  }
  for (const auto& source : observation.sources) {
    auto old = bindings_.find(source.id);
    SourceRegistry::Registration registration;
    registration.sourceId = {source.id}; registration.kind = source.kind;
    registration.instanceId = SourceInstanceId{source.instanceId};
    registration.requestedGeneration = source.incarnation;
    if (!source.personId.empty()) registration.personId = PersonId{source.personId};
    registration.personGeneration = source.personGeneration;
    registration.displayName = source.name; registration.processEpoch = observation.processEpoch; registration.externalId = source.externalId;
    SourceRegistry::Token token;
    const bool replace = old != bindings_.end() && (old->second.token.processEpoch != observation.processEpoch || !old->second.present || source.incarnation != old->second.observation.incarnation);
    if (old == bindings_.end() || replace) {
      const auto mutation = old == bindings_.end() ? registry_.add(registration) : registry_.replace(old->second.token, registration);
      if (!accepted(mutation.result) || !mutation.token) return result(Status::RegistryFailure, mutation.result);
      token = *mutation.token;
    } else token = old->second.token;
    auto r = registry_.setAvailability(token, source.videoAvailable ? SourceRegistry::Availability::Available : SourceRegistry::Availability::Unavailable);
    if (!accepted(r)) return result(Status::RegistryFailure, r);
    r = registry_.setDisplayName(token, source.name);
    if (!accepted(r)) return result(Status::RegistryFailure, r);
    r = registry_.setSubscription(token, source.subscriptionRequested, source.subscriptionObserved);
    if (!accepted(r)) return result(Status::RegistryFailure, r);
    const bool repeated = !replace && old != bindings_.end() && source.publication && old->second.observation.publication == source.publication;
    if (source.publication && !repeated) {
      const auto& p = *source.publication;
      r = registry_.publish(token, p.sequence, p.observedNs, {p.width, p.height, p.fpsNumerator, p.fpsDenominator, p.pixelFormat});
      if (!accepted(r)) return result(Status::RegistryFailure, r);
    }
    auto retained = source;
    if (!retained.publication && old != bindings_.end() && !replace) retained.publication = old->second.observation.publication;
    bindings_[source.id] = {std::move(retained), token, true};
  }
  peopleIds_ = std::move(nextPeople); sourceIds_ = std::move(nextSources); last_ = std::move(observation);
  return result(registry_.snapshot()->revision == current->revision ? Status::Unchanged : Status::Applied);
}
} // namespace corevideo::core
