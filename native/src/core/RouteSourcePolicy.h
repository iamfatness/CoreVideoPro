#pragma once

#include <string>
#include <string_view>
#include <optional>

namespace corevideo::core {

// Shared Windows/macOS runtime source-binding policy. Shells supply routing
// intent; the compositor consumes this decision. No UI, roster mutation or I/O.
struct RouteSourcePolicyInput {
  std::string_view mode;
  std::string_view mediaAssetId;
  std::string_view mediaAssetPath;
  std::string_view captureDeviceId;
  std::string_view participantId;
  std::optional<std::string_view> positionalFallbackParticipantId;
  // The core's DIRECTED speaker (ZoomActiveSpeakerDirector: chosen among the
  // shell's sources, #478 R1), or the last one it directed when there is none now.
  std::string_view directedSpeakerParticipantId = {};
};

struct RouteSourceBinding {
  std::string kind;
  std::string sourceId;
  std::string participantId;
};

inline RouteSourceBinding resolveRouteSource(const RouteSourcePolicyInput& input) {
  RouteSourceBinding binding{input.mode == "screen-share" ? "screen-share" : "participant-video", {}, {}};
  if (!input.mediaAssetId.empty() && !input.mediaAssetPath.empty()) {
    binding.kind = "media-video";
    binding.sourceId = "media:" + std::string(input.mediaAssetId);
  } else if (input.mode == "capture-input" && !input.captureDeviceId.empty()) {
    binding.participantId = "capture:" + std::string(input.captureDeviceId);
    binding.sourceId = binding.participantId;
  } else if (!input.participantId.empty()) {
    // Preserve an explicitly routed guest even if no frame is currently
    // available. Roster order must never silently replace that guest.
    binding.participantId = input.participantId;
    binding.sourceId = "zoom:" + binding.participantId;
  } else if (input.mode == "active-speaker") {
    // A FOLLOW-SPEAKER route shows the directed speaker (#478 R2). It used to fall
    // through to the positional fallback below, i.e. `videoFrames[routeIndex]`,
    // which is ordered by subscription uuid — so a full-screen speaker scene
    // showed the lowest-pid SOURCE, not the speaker. With no directed speaker yet
    // it binds NOTHING: never a random source.
    if (!input.directedSpeakerParticipantId.empty()) {
      binding.participantId = std::string(input.directedSpeakerParticipantId);
      binding.sourceId = "zoom:" + binding.participantId;
    }
  } else if (input.positionalFallbackParticipantId) {
    // Compatibility with existing empty route assignments. This fallback is
    // deliberately retained, including mode=none, until the route contract can
    // distinguish an intentional blank from a legacy omitted assignment.
    binding.participantId = *input.positionalFallbackParticipantId;
    binding.sourceId = "zoom:" + binding.participantId;
  }
  return binding;
}

}  // namespace corevideo::core
