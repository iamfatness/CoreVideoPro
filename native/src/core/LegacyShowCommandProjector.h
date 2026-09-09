#pragma once
#include "core/ShowStateOwner.h"

namespace corevideo::core {
// These DTOs are after legacy transport decoding and explicit identity mapping.
// Names/roster positions are never accepted as identity. Generation allocation
// remains with the authority; this projector never guesses/reuses a generation.
template<class Intent> struct LegacyNamedShowIntent {
  std::string id;
  Intent intent;
};
struct LegacySceneDto {
  std::string id;
  std::uint64_t generation{1};
  std::string label;
  std::vector<LegacyNamedShowIntent<ShowLayerIntent>> layers; // Explicit order.
};
struct LegacyTilesDto {
  std::string id;
  std::uint64_t generation{1};
  bool autoFill{true}, allowRosterAdditions{false};
  std::vector<ShowEntityRef> includedInputs, excludedInputs;
  struct Slot { std::uint32_t index; std::optional<ShowEntityRef> input; };
  std::vector<Slot> reservedSlots; // Explicit null is a persistent manual hole.
};
struct LegacyBusDto {
  bool supplied{false};
  std::optional<ShowEntityRef> scene; // Supplied + null explicitly clears bus.
};
struct LegacyShowCommandDto {
  std::string authorityEpoch;
  std::uint64_t expectedRevision{0};
  // Omitted collection preserves prior state. Present empty collection clears it.
  // Present nonempty collection replaces the entire corresponding collection.
  std::optional<std::vector<LegacyNamedShowIntent<ShowInputIntent>>> inputs;
  std::optional<std::vector<LegacySceneDto>> scenes;
  std::optional<std::vector<LegacyTilesDto>> tiles;
  std::optional<std::vector<LegacyNamedShowIntent<ShowOverlayIntent>>> overlays;
  std::optional<std::vector<LegacyNamedShowIntent<ShowIsoSelectionIntent>>> isoSelections;
  std::optional<std::vector<LegacyNamedShowIntent<ShowAudioRouteIntent>>> audioRoutes;
  std::optional<std::vector<LegacyNamedShowIntent<ShowOutputIntent>>> outputs;
  LegacyBusDto preview, program;
  // Transitions and Take operations intentionally do not belong in this DTO.
};
struct LegacyShowProjection {
  enum class Error { None, Conflict, Invalid, Capacity };
  Error error{Error::None};
  std::optional<ShowStateData> candidate;
};
class LegacyShowCommandProjector final {
 public:
  // No owner mutation, callbacks, source discovery, subscriptions or I/O. A valid
  // result still needs owner CAS/tombstone validation against the same revision.
  static LegacyShowProjection project(const ShowStateSnapshot& base,
                                     const LegacyShowCommandDto& command,
                                     std::size_t maxEntities = 4096,
                                     std::size_t maxTextBytes = 4 * 1024 * 1024);
};
} // namespace corevideo::core
