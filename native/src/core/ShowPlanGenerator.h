#pragma once
#include "core/ShowStateOwner.h"
#include "core/SourceRegistry.h"

namespace corevideo::core {
struct ShowPlanStamp {
  std::string authorityEpoch, registryEpoch;
  std::uint64_t controlRevision{0}, registryRevision{0};
  bool operator==(const ShowPlanStamp&) const = default;
};
struct PlannedSourceToken {
  std::string sourceId, instanceId, processEpoch;
  std::uint64_t generation{0};
  bool operator==(const PlannedSourceToken&) const = default;
};
enum class PlannedBindingStatus { Blank, Resolved, Missing, Ambiguous, RequiresSelection };
struct PlannedBinding {
  ShowRouteTarget intent;
  PlannedBindingStatus status{PlannedBindingStatus::Blank};
  std::optional<PlannedSourceToken> source;
  std::optional<SourceRegistry::Kind> sourceKind;
  // Historical observation only. Neither this nor Resolved proves current pixels,
  // deadline freshness, an audio stream, or writer eligibility.
  bool hasPublication{false};
  bool operator==(const PlannedBinding&) const = default;
};
struct PlannedInput {
  ShowEntityRef input;
  PlannedBinding binding;
  bool operator==(const PlannedInput&) const = default;
};
struct PlannedLayer {
  ShowEntityRef route;
  ShowLayerIntent intent;
  PlannedBinding binding;
  bool operator==(const PlannedLayer&) const = default;
};
struct PlannedScene {
  std::optional<ShowEntityRef> scene;
  PlannedBindingStatus status{PlannedBindingStatus::Blank};
  std::vector<PlannedLayer> layers;
  bool operator==(const PlannedScene&) const = default;
};
struct PlannedTileSlot {
  std::uint32_t slot{0};
  bool reserved{false};
  std::optional<ShowEntityRef> input;
  PlannedBinding binding;
  bool operator==(const PlannedTileSlot&) const = default;
};
struct PlannedTiles {
  ShowEntityRef tiles;
  std::vector<PlannedTileSlot> slots;
  bool operator==(const PlannedTiles&) const = default;
};
struct PlannedOverlay {
  ShowEntityRef overlay;
  ShowOverlayIntent intent;
  PlannedBinding binding;
  bool operator==(const PlannedOverlay&) const = default;
};
struct RenderPlan {
  ShowPlanStamp stamp;
  std::vector<PlannedInput> inputs;
  PlannedScene preview, program;
  std::vector<PlannedTiles> tiles;
  std::vector<PlannedOverlay> overlays;
  bool operator==(const RenderPlan&) const = default;
};
enum class PlannedAudioEligibility { UnknownCapability, UnresolvedIdentity };
struct PlannedAudioRoute {
  ShowEntityRef route;
  ShowAudioRouteIntent intent;
  PlannedBinding binding;
  bool destinationValid{false};
  PlannedAudioEligibility eligibility{PlannedAudioEligibility::UnresolvedIdentity};
  bool operator==(const PlannedAudioRoute&) const = default;
};
struct AudioPlan {
  ShowPlanStamp stamp;
  std::vector<PlannedAudioRoute> routes;
  bool operator==(const AudioPlan&) const = default;
};
struct PlannedIso {
  ShowEntityRef selection;
  PlannedBinding binding;
  bool operator==(const PlannedIso&) const = default;
};
struct PlannedOutput {
  ShowEntityRef output;
  ShowOutputIntent intent;
  bool operator==(const PlannedOutput&) const = default;
};
struct OutputPlan {
  ShowPlanStamp stamp;
  std::vector<PlannedOutput> outputs;
  std::vector<PlannedIso> isoSelections;
  bool operator==(const OutputPlan&) const = default;
};
struct ShowPlans {
  RenderPlan render;
  AudioPlan audio;
  OutputPlan output;
  bool operator==(const ShowPlans&) const = default;
};
// Pure control-plane projection. Inputs must be immutable snapshots. No clock,
// rendering, source reads, callbacks, global state, or mutation occurs here.
// The pair of authority/registry revisions is the deterministic plan identity;
// a runtime publisher assigns its own plan generation when adopting this result.
std::shared_ptr<const ShowPlans> generateShowPlans(const ShowStateSnapshot& show,
                                                 const SourceRegistry::Snapshot& registry);
} // namespace corevideo::core
