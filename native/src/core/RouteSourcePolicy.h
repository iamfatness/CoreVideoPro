#pragma once

#include <string>
#include <string_view>
#include <optional>
#include "rpc/Json.h"
#include <charconv>
#include <cmath>
#include <cstdint>
#include <utility>

namespace corevideo::core {

struct ExactRouteSourceRef {
  std::string sourceId, instanceId, processEpoch, kind;
  uint64_t generation{0};
  bool operator==(const ExactRouteSourceRef&) const = default;
};
struct ExactRouteSourceIntent {
  // Presence with invalid/null content is rejection, never legacy compatibility.
  std::optional<ExactRouteSourceRef> reference;
};
inline bool validExactRouteSourceRef(const ExactRouteSourceRef& ref) {
  const auto text = [](const std::string& s) {
    if (s.empty() || s.size() > 512) return false;
    for (size_t i = 0; i < s.size();) {
      const auto c = static_cast<unsigned char>(s[i++]);
      if (c < 0x80) continue;
      unsigned code = 0, minimum = 0; size_t count = 0;
      if (c >= 0xC2 && c <= 0xDF) { code = c & 31; count = 1; minimum = 0x80; }
      else if (c >= 0xE0 && c <= 0xEF) { code = c & 15; count = 2; minimum = 0x800; }
      else if (c >= 0xF0 && c <= 0xF4) { code = c & 7; count = 3; minimum = 0x10000; }
      else return false;
      if (count > s.size() - i) return false;
      while (count--) { const auto next = static_cast<unsigned char>(s[i++]);
        if ((next & 0xC0) != 0x80) return false;
        code = (code << 6) | (next & 63);
      }
      if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
    }
    return true;
  };
  return text(ref.sourceId) && text(ref.instanceId) && text(ref.processEpoch) &&
      ref.generation > 0 && ref.generation <= 9007199254740991ULL &&
      (ref.kind == "camera" || ref.kind == "share");
}
inline std::optional<ExactRouteSourceIntent> parseExactRouteSource(const rpc::Json& route) {
  const auto* value = route.get("exactSourceRef");
  if (!value) return std::nullopt;
  ExactRouteSourceIntent intent;
  if (!value->isObject()) return intent;
  ExactRouteSourceRef ref;
  for (auto pair : {std::pair{"sourceId", &ref.sourceId}, {"instanceId", &ref.instanceId},
                    {"processEpoch", &ref.processEpoch}, {"kind", &ref.kind}}) {
    const auto* field = value->get(pair.first);
    if (!field || !field->isString() || field->asString().size() > 512) return intent;
    *pair.second = field->asString();
  }
  const auto* generation = value->get("generation");
  if (!generation || !generation->isNumber()) return intent;
  if (const auto lexeme = generation->numberLexeme()) {
    const auto parsed = std::from_chars(lexeme->data(), lexeme->data() + lexeme->size(), ref.generation);
    if (parsed.ec != std::errc{} || parsed.ptr != lexeme->data() + lexeme->size()) return intent;
  } else {
    const auto number = generation->asNumber();
    if (!std::isfinite(number) || number < 1 || number > 9007199254740991.0 || std::floor(number) != number) return intent;
    ref.generation = static_cast<uint64_t>(number);
  }
  if (validExactRouteSourceRef(ref)) intent.reference = std::move(ref);
  return intent;
}

// Shared Windows/macOS runtime source-binding policy. Shells supply routing
// intent; the compositor consumes this decision. No UI, roster mutation or I/O.
struct RouteSourcePolicyInput {
  std::string_view mode;
  std::string_view mediaAssetId;
  std::string_view mediaAssetPath;
  std::string_view captureDeviceId;
  std::string_view participantId;
  std::optional<std::string_view> positionalFallbackParticipantId;
  const ExactRouteSourceIntent* exactSource{nullptr};
  // Must identify the exact immutable frame being bound, not a roster entry.
  const ExactRouteSourceRef* frameIdentity{nullptr};
};

struct RouteSourceBinding {
  std::string kind;
  std::string sourceId;
  std::string participantId;
  enum class Status { Legacy, ExactAvailable, Missing, Rejected } status{Status::Legacy};
};

inline RouteSourceBinding resolveRouteSource(const RouteSourcePolicyInput& input) {
  if (input.exactSource) {
    const auto& ref = input.exactSource->reference;
    if (!ref || !validExactRouteSourceRef(*ref)) return {"missing-source", {}, {}, RouteSourceBinding::Status::Rejected};
    if (!input.frameIdentity || *input.frameIdentity != *ref)
      return {"missing-source", {}, {}, RouteSourceBinding::Status::Missing};
    return {ref->kind == "camera" ? "participant-video" : "screen-share",
            ref->sourceId, {}, RouteSourceBinding::Status::ExactAvailable};
  }
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
