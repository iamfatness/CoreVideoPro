#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace corevideo::core {

// This control-plane foundation owns values only. It does not resolve SDK handles,
// render plans, persist files, or notify subscribers. Callers prepare a candidate
// outside the owner and retry revision conflicts against the returned snapshot.
struct ShowEntityRef {
  std::string id;
  std::uint64_t generation{1};
  auto operator<=>(const ShowEntityRef&) const = default;
};

// Exact transient source identity, deliberately distinct from show/person refs.
struct ShowSourceRef {
  std::string sourceId, instanceId, processEpoch;
  std::uint64_t generation{1};
  auto operator<=>(const ShowSourceRef&) const = default;
};

enum class ShowRouteKind { Blank, FixedSource, FollowPerson, ActiveSpeaker, Spotlight, ScreenShare };
struct ShowRouteTarget {
  ShowRouteKind kind{ShowRouteKind::Blank};
  std::optional<ShowSourceRef> source;
  // Durable person identity is distinct from a source incarnation.
  std::optional<ShowEntityRef> person;
  bool operator==(const ShowRouteTarget&) const = default;
};
enum class ShowRouteResolution { Blank, Available, Missing, RequiresSelection };
// Fixed-source loss is Missing, never an automatic selector or another source.
ShowRouteResolution classifyShowRoute(const ShowRouteTarget& target,
                                     const std::set<ShowSourceRef>& available);

struct ShowInputIntent {
  std::uint64_t generation{1};
  std::string label;
  ShowRouteTarget target;
  bool operator==(const ShowInputIntent&) const = default;
};
struct ShowLayerIntent {
  std::uint64_t generation{1};
  ShowRouteTarget target;
  // Normalized parts per million avoid float/NaN equality and persistence drift.
  std::int32_t x{0}, y{0}, width{1'000'000}, height{1'000'000};
  bool visible{true};
  bool operator==(const ShowLayerIntent&) const = default;
};
struct ShowSceneIntent {
  std::uint64_t generation{1};
  std::string label;
  std::map<std::string, ShowLayerIntent> routes;
  // Explicit stacking order: identity is never inferred from array position.
  std::vector<std::string> layerOrder;
  bool operator==(const ShowSceneIntent&) const = default;
};
struct ShowOverlayIntent {
  std::uint64_t generation{1};
  std::optional<ShowSourceRef> source;
  std::string content;
  bool requestedVisible{false};
  bool operator==(const ShowOverlayIntent&) const = default;
};
struct ShowAudioRouteIntent {
  std::uint64_t generation{1};
  ShowRouteTarget source;
  ShowEntityRef destination;
  std::int32_t gainMilliDb{0};
  bool muted{false};
  bool operator==(const ShowAudioRouteIntent&) const = default;
};
struct ShowOutputIntent {
  std::uint64_t generation{1};
  enum class Kind { Program, Stream, Recorder, VirtualCamera, Monitor };
  Kind kind{Kind::Program};
  bool requested{false};
  bool operator==(const ShowOutputIntent&) const = default;
};
struct ShowTilesIntent {
  std::uint64_t generation{1};
  bool autoFill{true};
  // Roster-only people are never imported unless the operator enables this.
  bool allowRosterAdditions{false};
  std::set<ShowEntityRef> includedInputs;
  std::set<ShowEntityRef> excludedInputs;
  // null reserves an explicit blank slot; it is not eligible for auto-fill.
  std::map<std::uint32_t, std::optional<ShowEntityRef>> reservedSlots;
  bool operator==(const ShowTilesIntent&) const = default;
};
struct ShowIsoSelectionIntent {
  std::uint64_t generation{1};
  // FixedSource pins one incarnation; FollowPerson deliberately follows rejoin.
  ShowRouteTarget target;
  bool operator==(const ShowIsoSelectionIntent&) const = default;
};
struct ShowStateData {
  std::map<std::string, ShowInputIntent> inputs;
  std::vector<std::string> inputOrder;
  // Ordered operator intent, retained when sources are offline. Writer eligibility
  // is derived elsewhere and must never rewrite this selection.
  std::map<std::string, ShowIsoSelectionIntent> isoSelections;
  std::vector<std::string> isoOrder;
  std::map<std::string, ShowTilesIntent> tiles;
  std::map<std::string, ShowSceneIntent> scenes;
  std::map<std::string, ShowOverlayIntent> overlays;
  std::map<std::string, ShowAudioRouteIntent> audioRoutes;
  std::map<std::string, ShowOutputIntent> outputs;
  // null is explicit blank. Non-null must identify an existing scene generation.
  std::optional<ShowEntityRef> preview;
  std::optional<ShowEntityRef> program;
  bool operator==(const ShowStateData&) const = default;
};
struct ShowStateSnapshot {
  std::string authorityEpoch;
  std::uint64_t revision{0};
  ShowStateData data;
};
enum class ShowStateUpdateStatus {
  Changed, Unchanged, Conflict, Invalid, RevisionExhausted, CapacityExceeded
};
struct ShowStateUpdateResult {
  ShowStateUpdateStatus status;
  std::shared_ptr<const ShowStateSnapshot> snapshot;
};
class ShowStateOwner final {
 public:
  ShowStateOwner(const ShowStateOwner&); // Immutable snapshots/marks may be shared safely.
  explicit ShowStateOwner(std::string authorityEpoch,
                          std::size_t maxGenerationMarks = 65'536);
  std::shared_ptr<const ShowStateSnapshot> snapshot() const;
  ShowStateUpdateResult replace(const std::string& expectedAuthorityEpoch,
                                std::uint64_t expectedRevision, ShowStateData candidate);
 private:
  mutable std::mutex mutex_;
  using GenerationMarks = std::map<std::vector<std::string>, std::uint64_t>;
  std::shared_ptr<const ShowStateSnapshot> current_;
  std::shared_ptr<const GenerationMarks> generationMarks_;
  std::size_t maxGenerationMarks_;
};
}  // namespace corevideo::core
