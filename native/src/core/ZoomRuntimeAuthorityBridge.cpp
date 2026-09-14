#include "core/ZoomRuntimeAuthorityBridge.h"
#include <limits>
#include <set>

namespace corevideo::core {
ZoomRuntimeAuthorityBridge::Result ZoomRuntimeAuthorityBridge::convert(
    const modules::ZoomEngineRuntime::AuthorityObservation& input, size_t maxSources) {
  using Runtime = modules::ZoomEngineRuntime;
  constexpr uint64_t maxSafe = 9007199254740991ULL;
  const auto text = [](const std::string& s) { return !s.empty() && s.size() <= 512; };
  if (!maxSources || maxSources > kMaxSources) return {Status::Invalid, {}};
  if (input.sources.size() > maxSources) return {Status::Capacity, {}};
  if (!input.valid || !text(input.processEpoch) || !input.processGeneration ||
      input.processGeneration > maxSafe || !input.sequence || input.sequence > maxSafe)
    return {Status::Invalid, {}};
  std::set<std::string> ids, instances;
  std::set<std::pair<uint32_t, Runtime::AuthoritySource::Kind>> external;
  // Validate everything before copying or producing any adapter input.
  for (const auto& source : input.sources) {
    if (source.durablePersonId) return {Status::UnsupportedDurableIdentity, {}};
    if (!source.participantId || !text(source.sourceId) || !text(source.instanceId) ||
        !source.generation || source.generation > maxSafe ||
        (source.kind != Runtime::AuthoritySource::Kind::Camera && source.kind != Runtime::AuthoritySource::Kind::Share) ||
        !ids.insert(source.sourceId).second || !instances.insert(source.instanceId).second ||
        !external.emplace(source.participantId, source.kind).second ||
        (source.subscriptionObserved.value_or(false) && !source.available)) return {Status::Invalid, {}};
    if (source.publication) {
      const auto& p = *source.publication;
      if (!source.available || !p.sequence || p.sequence > maxSafe || p.observedNs < 0 ||
          !p.width || !p.height || p.width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
          p.height > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
          p.pixelFormat.empty() || p.pixelFormat.size() > 128 ||
          (p.fpsNumerator && (!*p.fpsNumerator || *p.fpsNumerator > static_cast<uint32_t>(std::numeric_limits<int>::max()))))
        return {Status::Invalid, {}};
    }
  }
  ZoomSourceAuthorityAdapter::Observation output;
  output.processEpoch = input.processEpoch;
  output.sequence = input.sequence;
  output.sources.reserve(input.sources.size());
  for (const auto& source : input.sources) {
    ZoomSourceAuthorityAdapter::Source mapped;
    mapped.id = source.sourceId;
    mapped.instanceId = source.instanceId;
    mapped.incarnation = source.generation;
    mapped.externalId = std::to_string(source.participantId);
    mapped.kind = source.kind == Runtime::AuthoritySource::Kind::Camera
        ? SourceRegistry::Kind::ParticipantVideo : SourceRegistry::Kind::ParticipantShare;
    mapped.videoAvailable = source.available;
    mapped.subscriptionRequested = source.subscriptionRequested;
    mapped.subscriptionObserved = source.subscriptionObserved;
    if (source.publication) {
      const auto& p = *source.publication;
      ZoomSourceAuthorityAdapter::Publication publication;
      publication.sequence = p.sequence; publication.observedNs = p.observedNs;
      publication.width = static_cast<int>(p.width); publication.height = static_cast<int>(p.height);
      publication.pixelFormat = p.pixelFormat;
      if (p.fpsNumerator) { publication.fpsNumerator = static_cast<int>(*p.fpsNumerator); publication.fpsDenominator = 1; }
      mapped.publication = std::move(publication);
    }
    output.sources.push_back(std::move(mapped));
  }
  return {Status::Converted, std::move(output)};
}
} // namespace corevideo::core
