#pragma once
#include "core/LegacyShowCommandProjector.h"
#include <array>

namespace corevideo::core {
// Capture metadata only: no access to MediaCore, SDK, registry, files or clocks.
// Optional generations must come from a persistent authority mapping. The
// adapter never fills them from positions, names, SDK IDs or a constant 1.
class DesiredShowCheckpoint final {
 public:
  enum class Domain : uint32_t { Program=1, Preview=2, Scenes=4, Inputs=8, Tiles=16, Overlays=32, Iso=64, Audio=128, Outputs=256 };
  enum class Reason { None, Uncaptured, MissingGeneration, MissingSourceIdentity, UnsupportedPolicy, Invalid };
  static constexpr uint32_t allDomains = 511;
  static constexpr uint32_t emptyOnlyDomains = 8|16|32|64|128;
  struct Bus {
    bool captured{false};
    std::optional<std::string> sceneId; // Captured null is explicit blank.
    std::optional<uint64_t> generation;
  };
  struct Route {
    std::string id;
    std::optional<uint64_t> generation;
    // Source IDs here are already independently proven full provider tokens.
    ShowRouteTarget target;
    int32_t x{0}, y{0}, width{1'000'000}, height{1'000'000};
    bool visible{true};
    bool unsupportedPolicies{false}; // e.g. crop/fit/opacity/chroma/grade/media playback.
  };
  struct Scene {
    std::string id, label;
    std::optional<uint64_t> generation;
    std::vector<Route> orderedRoutes;
    bool unsupportedPolicies{false}; // Background/Tiles animation/layout not represented here.
  };
  struct Output {
    std::string id;
    std::optional<uint64_t> generation;
    ShowOutputIntent::Kind kind{ShowOutputIntent::Kind::Program};
    bool requested{false};
    bool unsupportedPolicies{false}; // Format/routing/session policy requiring richer intent.
  };
  struct Capture {
    std::string legacyAuthorityEpoch;
    std::optional<uint64_t> legacyRevision;
    Bus program, preview;
    std::optional<std::vector<Scene>> scenes;
    std::optional<std::vector<Output>> outputs;
    // Caller positively observed these domains empty at the same capture boundary.
    // Absence of a capture hook is never evidence of emptiness.
    uint32_t knownEmptyDomains{0};
  };
  struct Result {
    bool validBasis{false}, complete{false};
    uint32_t supportedDomains{0}, unsupportedDomains{allDomains};
    std::array<Reason,9> reasons{}; // Bit index in Domain.
    LegacyShowCommandDto desired;
  };
  static Result project(const Capture&, size_t maxEntities=4096, size_t maxTextBytes=2*1024*1024);
};
} // namespace corevideo::core
