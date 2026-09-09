#include "core/ShowPlanGenerator.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace corevideo::core {
namespace {
using Registry = SourceRegistry;
enum class ResolutionPurpose { Identity, Video };
PlannedBinding resolve(const ShowRouteTarget& target, const Registry::Snapshot& registry,
                       ResolutionPurpose purpose, std::int64_t videoFreshAfterNs) {
  PlannedBinding result; result.intent = target;
  if (target.kind == ShowRouteKind::Blank) {
    if (target.source || target.person) result.status = PlannedBindingStatus::Missing;
    return result;
  }
  std::vector<const Registry::Source*> matches;
  if (target.kind == ShowRouteKind::FixedSource) {
    if (!target.source || target.person) { result.status = PlannedBindingStatus::Missing; return result; }
    for (const auto& source : registry.sources)
      if (source.token.sourceId.value == target.source->sourceId &&
          source.token.instanceId.value == target.source->instanceId &&
          source.token.processEpoch == target.source->processEpoch &&
          source.token.generation == target.source->generation &&
          source.availability != Registry::Availability::Departed) matches.push_back(&source);
  } else if (target.kind == ShowRouteKind::FollowPerson) {
    if (!target.person || target.source || target.person->generation == 0 ||
        std::count_if(registry.persons.begin(), registry.persons.end(), [&](const auto& p) {
          return p.id.value == target.person->id && p.generation == target.person->generation;
        }) != 1) { result.status = PlannedBindingStatus::Missing; return result; }
    for (const auto& source : registry.sources)
      if (source.kind == Registry::Kind::ParticipantVideo && source.personId &&
          source.personId->value == target.person->id && source.personGeneration == target.person->generation &&
          source.availability != Registry::Availability::Departed)
        matches.push_back(&source);
  } else {
    result.status = PlannedBindingStatus::RequiresSelection;
    return result;
  }
  if (matches.empty()) result.status = PlannedBindingStatus::Missing;
  else if (matches.size() > 1) result.status = PlannedBindingStatus::Ambiguous;
  else {
    const auto& source = *matches.front();
    result.source = PlannedSourceToken{source.token.sourceId.value, source.token.instanceId.value,
                                      source.token.processEpoch, source.token.generation};
    result.sourceKind = source.kind;
    result.hasPublication = source.hasPublication;
    if (purpose == ResolutionPurpose::Video && source.availability != Registry::Availability::Available)
      result.status = PlannedBindingStatus::Unavailable;
    else if (purpose == ResolutionPurpose::Video &&
             (!source.hasPublication || source.lastPublicationNs < videoFreshAfterNs))
      result.status = PlannedBindingStatus::Stale;
    else result.status = PlannedBindingStatus::Resolved;
  }
  return result;
}
PlannedAudioEligibility audioEligibility(const PlannedBinding& binding) {
  // Replace only this capability decision when independent medium state lands.
  return binding.status == PlannedBindingStatus::Resolved
      ? PlannedAudioEligibility::UnknownCapability : PlannedAudioEligibility::UnresolvedIdentity;
}
void appendText(std::string& out, const std::string& value) {
  out += std::to_string(value.size()); out.push_back(':'); out += value;
}
void appendBinding(std::string& out, const PlannedBinding& binding) {
  out.push_back('|'); out += std::to_string(static_cast<int>(binding.status));
  out.push_back(','); out += binding.hasPublication ? '1' : '0';
  out.push_back(','); out += binding.sourceKind ? std::to_string(static_cast<int>(*binding.sourceKind)) : "-";
  if (!binding.source) { out += ",-"; return; }
  out.push_back(','); appendText(out,binding.source->sourceId);
  appendText(out,binding.source->instanceId); appendText(out,binding.source->processEpoch);
  out += std::to_string(binding.source->generation); out.push_back(';');
}
std::string eligibilityIdentity(const ShowPlans& plans) {
  std::string out = "eligibility-v1";
  for (const auto& input : plans.render.inputs) appendBinding(out,input.binding);
  out += "|preview:" + std::to_string(static_cast<int>(plans.render.preview.status));
  for (const auto& layer : plans.render.preview.layers) appendBinding(out,layer.binding);
  out += "|program:" + std::to_string(static_cast<int>(plans.render.program.status));
  for (const auto& layer : plans.render.program.layers) appendBinding(out,layer.binding);
  for (const auto& tiles : plans.render.tiles)
    for (const auto& slot : tiles.slots) appendBinding(out,slot.binding);
  for (const auto& overlay : plans.render.overlays) appendBinding(out,overlay.binding);
  for (const auto& audio : plans.audio.routes) {
    appendBinding(out,audio.binding); out += audio.destinationValid ? "D1" : "D0";
    out += std::to_string(static_cast<int>(audio.eligibility));
  }
  for (const auto& iso : plans.output.isoSelections) {
    appendBinding(out,iso.videoBinding); appendBinding(out,iso.audioBinding);
    out += std::to_string(static_cast<int>(iso.audioEligibility));
  }
  return out;
}
PlannedScene scenePlan(const std::optional<ShowEntityRef>& ref, const ShowStateData& data,
                       const Registry::Snapshot& registry, std::int64_t videoFreshAfterNs) {
  PlannedScene result; result.scene = ref;
  if (!ref) return result;
  const auto found = data.scenes.find(ref->id);
  if (found == data.scenes.end() || found->second.generation != ref->generation) {
    result.status = PlannedBindingStatus::Missing; return result;
  }
  result.status = PlannedBindingStatus::Resolved;
  for (const auto& id : found->second.layerOrder) {
    const auto route = found->second.routes.find(id);
    if (route == found->second.routes.end()) { result.status = PlannedBindingStatus::Missing; continue; }
    result.layers.push_back({{id, route->second.generation}, route->second,
      resolve(route->second.target, registry, ResolutionPurpose::Video, videoFreshAfterNs)});
  }
  return result;
}
}
std::shared_ptr<const ShowPlans> generateShowPlans(const ShowStateSnapshot& show,
                                                 const Registry::Snapshot& registry,
                                                 ShowPlanGenerationContext context) {
  if (context.videoFreshAfterNs < 0)
    throw std::invalid_argument("Show plan freshness cutoff must use a nonnegative monotonic timestamp");
  auto plans = std::make_shared<ShowPlans>();
  ShowPlanStamp stamp{show.authorityEpoch, registry.registryEpoch, show.revision,
                      registry.decisionRevision, {}};
  const auto& data = show.data;
  for (const auto& id : data.inputOrder) {
    const auto found = data.inputs.find(id);
    if (found != data.inputs.end()) plans->render.inputs.push_back({{id, found->second.generation},
      resolve(found->second.target, registry, ResolutionPurpose::Video, context.videoFreshAfterNs)});
  }
  plans->render.preview = scenePlan(data.preview, data, registry, context.videoFreshAfterNs);
  plans->render.program = scenePlan(data.program, data, registry, context.videoFreshAfterNs);
  for (const auto& [id, tiles] : data.tiles) {
    PlannedTiles result; result.tiles = {id, tiles.generation};
    std::map<std::uint32_t, PlannedTileSlot> slots;
    std::set<ShowEntityRef> used;
    std::set<std::string> usedSources;
    const auto inputBinding = [&](const ShowEntityRef& ref) {
      const auto input = data.inputs.find(ref.id);
      if (input != data.inputs.end() && input->second.generation == ref.generation)
        return resolve(input->second.target, registry, ResolutionPurpose::Video, context.videoFreshAfterNs);
      PlannedBinding missing; missing.status = PlannedBindingStatus::Missing; return missing;
    };
    for (const auto& [slot, ref] : tiles.reservedSlots) {
      // Exclusion wins over a reservation's pixels, but never removes its slot.
      auto binding = ref && !tiles.excludedInputs.contains(*ref) ? inputBinding(*ref) : PlannedBinding{};
      if (ref && tiles.excludedInputs.contains(*ref)) binding.status = PlannedBindingStatus::Excluded;
      if (ref) used.insert(*ref);
      if (binding.source) usedSources.insert(binding.source->sourceId);
      slots.emplace(slot, PlannedTileSlot{slot, true, ref, std::move(binding)});
    }
    std::uint32_t next = 0;
    const auto append = [&](std::optional<ShowEntityRef> input, PlannedBinding binding) {
      while (slots.contains(next)) {
        if (next == std::numeric_limits<std::uint32_t>::max()) return;
        ++next;
      }
      if (binding.source) usedSources.insert(binding.source->sourceId);
      slots.emplace(next, PlannedTileSlot{next, false, input, std::move(binding)});
    };
    for (const auto& input : plans->render.inputs) {
      if (used.contains(input.input) || tiles.excludedInputs.contains(input.input)) continue;
      if (!tiles.autoFill && !tiles.includedInputs.contains(input.input)) continue;
      if (input.binding.status != PlannedBindingStatus::Resolved) continue;
      append(input.input, input.binding);
    }
    if (tiles.autoFill && tiles.allowRosterAdditions) {
      auto sources = registry.sources;
      std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) { return a.token.sourceId.value < b.token.sourceId.value; });
      // Explicit exclusion applies to its resolved source as well as its input.
      for (const auto& excluded : tiles.excludedInputs) {
        const auto binding = inputBinding(excluded);
        if (binding.source) usedSources.insert(binding.source->sourceId);
      }
      for (const auto& source : sources) {
        if (source.kind != Registry::Kind::ParticipantVideo || source.availability != Registry::Availability::Available ||
            usedSources.contains(source.token.sourceId.value)) continue;
        auto binding = resolve({ShowRouteKind::FixedSource,
          ShowSourceRef{source.token.sourceId.value, source.token.instanceId.value, source.token.processEpoch, source.token.generation}, std::nullopt},
          registry, ResolutionPurpose::Video, context.videoFreshAfterNs);
        if (binding.status == PlannedBindingStatus::Resolved) append(std::nullopt, std::move(binding));
      }
    }
    for (auto& [slot, value] : slots) result.slots.push_back(std::move(value));
    plans->render.tiles.push_back(std::move(result));
  }
  for (const auto& [id, overlay] : data.overlays) {
    const ShowRouteTarget target = overlay.source ? ShowRouteTarget{ShowRouteKind::FixedSource, overlay.source, std::nullopt} : ShowRouteTarget{};
    plans->render.overlays.push_back({{id, overlay.generation}, overlay,
      resolve(target, registry, ResolutionPurpose::Video, context.videoFreshAfterNs)});
  }
  for (const auto& [id, audio] : data.audioRoutes) {
    const auto destination = data.outputs.find(audio.destination.id);
    const auto binding = resolve(audio.source, registry, ResolutionPurpose::Identity, 0);
    // Registry Kind describes video/media identity, not audio capability. Even a
    // published participant/device frame cannot establish audio availability.
    plans->audio.routes.push_back({{id, audio.generation}, audio, binding,
      destination != data.outputs.end() && destination->second.generation == audio.destination.generation,
      audioEligibility(binding)});
  }
  for (const auto& [id, output] : data.outputs) plans->output.outputs.push_back({{id, output.generation}, output});
  for (const auto& id : data.isoOrder) {
    const auto iso = data.isoSelections.find(id);
    if (iso != data.isoSelections.end()) {
      const auto video = resolve(iso->second.target, registry, ResolutionPurpose::Video,
                                 context.videoFreshAfterNs);
      const auto audio = resolve(iso->second.target, registry, ResolutionPurpose::Identity, 0);
      plans->output.isoSelections.push_back(
          {{id, iso->second.generation}, video, audio, audioEligibility(audio)});
    }
  }
  stamp.eligibilityIdentity = eligibilityIdentity(*plans);
  plans->render.stamp = plans->audio.stamp = plans->output.stamp = std::move(stamp);
  return plans;
}
} // namespace corevideo::core
