#include "core/ShowPlanGenerator.h"
#include <algorithm>
#include <limits>

namespace corevideo::core {
namespace {
using Registry = SourceRegistry;
PlannedBinding resolve(const ShowRouteTarget& target, const Registry::Snapshot& registry) {
  PlannedBinding result; result.intent = target;
  if (target.kind == ShowRouteKind::Blank) {
    if (target.source || target.person) result.status = PlannedBindingStatus::Missing;
    return result;
  }
  std::vector<const Registry::Source*> matches;
  if (target.kind == ShowRouteKind::FixedSource) {
    if (!target.source || target.person) { result.status = PlannedBindingStatus::Missing; return result; }
    for (const auto& source : registry.sources)
      if (source.token.sourceId.value == target.source->id && source.token.generation == target.source->generation &&
          source.availability == Registry::Availability::Available) matches.push_back(&source);
  } else if (target.kind == ShowRouteKind::FollowPerson) {
    if (!target.person || target.source || target.person->generation == 0 ||
        std::count_if(registry.persons.begin(), registry.persons.end(), [&](const auto& p) {
          return p.id.value == target.person->id && p.generation == target.person->generation;
        }) != 1) { result.status = PlannedBindingStatus::Missing; return result; }
    for (const auto& source : registry.sources)
      if (source.kind == Registry::Kind::ParticipantVideo && source.personId &&
          source.personId->value == target.person->id && source.personGeneration == target.person->generation && source.availability == Registry::Availability::Available)
        matches.push_back(&source);
  } else {
    result.status = PlannedBindingStatus::RequiresSelection;
    return result;
  }
  if (matches.empty()) result.status = PlannedBindingStatus::Missing;
  else if (matches.size() > 1) result.status = PlannedBindingStatus::Ambiguous;
  else {
    const auto& source = *matches.front();
    result.status = PlannedBindingStatus::Resolved;
    result.source = PlannedSourceToken{source.token.sourceId.value, source.token.instanceId.value,
                                      source.token.processEpoch, source.token.generation};
    result.sourceKind = source.kind;
    result.hasPublication = source.hasPublication;
  }
  return result;
}
PlannedAudioEligibility audioEligibility(const PlannedBinding& binding) {
  // Replace only this capability decision when independent medium state lands.
  return binding.status == PlannedBindingStatus::Resolved
      ? PlannedAudioEligibility::UnknownCapability : PlannedAudioEligibility::UnresolvedIdentity;
}
PlannedScene scenePlan(const std::optional<ShowEntityRef>& ref, const ShowStateData& data,
                       const Registry::Snapshot& registry) {
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
    result.layers.push_back({{id, route->second.generation}, route->second, resolve(route->second.target, registry)});
  }
  return result;
}
}
std::shared_ptr<const ShowPlans> generateShowPlans(const ShowStateSnapshot& show,
                                                 const Registry::Snapshot& registry) {
  auto plans = std::make_shared<ShowPlans>();
  const ShowPlanStamp stamp{show.authorityEpoch, registry.registryEpoch, show.revision, registry.revision};
  plans->render.stamp = plans->audio.stamp = plans->output.stamp = stamp;
  const auto& data = show.data;
  for (const auto& id : data.inputOrder) {
    const auto found = data.inputs.find(id);
    if (found != data.inputs.end()) plans->render.inputs.push_back({{id, found->second.generation}, resolve(found->second.target, registry)});
  }
  plans->render.preview = scenePlan(data.preview, data, registry);
  plans->render.program = scenePlan(data.program, data, registry);
  for (const auto& [id, tiles] : data.tiles) {
    PlannedTiles result; result.tiles = {id, tiles.generation};
    std::map<std::uint32_t, PlannedTileSlot> slots;
    std::set<ShowEntityRef> used;
    std::set<std::string> usedSources;
    const auto inputBinding = [&](const ShowEntityRef& ref) {
      const auto input = data.inputs.find(ref.id);
      if (input != data.inputs.end() && input->second.generation == ref.generation) return resolve(input->second.target, registry);
      PlannedBinding missing; missing.status = PlannedBindingStatus::Missing; return missing;
    };
    for (const auto& [slot, ref] : tiles.reservedSlots) {
      // Exclusion wins over a reservation's pixels, but never removes its slot.
      auto binding = ref && !tiles.excludedInputs.contains(*ref) ? inputBinding(*ref) : PlannedBinding{};
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
        append(std::nullopt, resolve({ShowRouteKind::FixedSource, ShowEntityRef{source.token.sourceId.value, source.token.generation}, std::nullopt}, registry));
      }
    }
    for (auto& [slot, value] : slots) result.slots.push_back(std::move(value));
    plans->render.tiles.push_back(std::move(result));
  }
  for (const auto& [id, overlay] : data.overlays) {
    const ShowRouteTarget target = overlay.source ? ShowRouteTarget{ShowRouteKind::FixedSource, overlay.source, std::nullopt} : ShowRouteTarget{};
    plans->render.overlays.push_back({{id, overlay.generation}, overlay, resolve(target, registry)});
  }
  for (const auto& [id, audio] : data.audioRoutes) {
    const auto destination = data.outputs.find(audio.destination.id);
    const auto binding = resolve(audio.source, registry);
    // Registry Kind describes video/media identity, not audio capability. Even a
    // published participant/device frame cannot establish audio availability.
    plans->audio.routes.push_back({{id, audio.generation}, audio, binding,
      destination != data.outputs.end() && destination->second.generation == audio.destination.generation,
      audioEligibility(binding)});
  }
  for (const auto& [id, output] : data.outputs) plans->output.outputs.push_back({{id, output.generation}, output});
  for (const auto& id : data.isoOrder) {
    const auto iso = data.isoSelections.find(id);
    if (iso != data.isoSelections.end()) plans->output.isoSelections.push_back({{id, iso->second.generation}, resolve(iso->second.target, registry)});
  }
  return plans;
}
} // namespace corevideo::core
