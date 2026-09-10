#pragma once

#include <string_view>

namespace corevideo::core {

// Which console controls govern an audio source (T1.6 / #455).
//
// Since PR #408 the media decoder labels each clip's PCM `media:<assetId>`
// (per-clip identity: meters, ISO, diagnostics, and so two clips never
// coalesce into one block). The shell, however, describes media audio with ONE
// generic id, "media": one "Media playback" routing row, one "media" strip and
// "media" -> bus sends. The core matched strips and sends by exact id, so every
// media frame was dropped by the FADER LAW and reached no bus.
//
// The alias: every `media:*` clip with no strip AND no send of its own is
// PRE-SUMMED into one routed source keyed "media" before any strip processing,
// so the "media" strip (fader, gate, compressor, inserts, VST) runs ONCE on the
// combined signal — exactly like a hardware desk's media-return channel — and
// the existing "media" sends route it. Clips keep their own ids everywhere
// else (mixer-session meters, diagnostics).
//
// Explicit per-clip rows win. A clip with an exact `media:<assetId>` strip or
// send stays a separate source (its own DSP chain). A clip with its own send
// rows is routed by THOSE rows alone: a clip with its own send row does NOT
// inherit missing cells from the generic "media" row. With no exact strip it is
// still governed by the "media" strip (the FADER LAW holds through the alias).
// A clip with its own strip but no send rows of its own is routed by the
// "media" row.
//
// Do NOT fix this by making the shell send per-clip ids: the routing grid
// un-routes any cell the core did not echo, so a "media" row would switch itself
// off ~2 s later (AudioRoutingMatrixViewModel.ApplyCoreSends).
inline constexpr std::string_view kMediaAudioControlSourceId = "media";
inline constexpr std::string_view kMediaAudioSourcePrefix = "media:";

// A per-clip media audio id: `media:<non-empty assetId>`.
inline bool isMediaClipAudioSourceId(std::string_view sourceId) {
  return sourceId.size() > kMediaAudioSourcePrefix.size() &&
         sourceId.substr(0, kMediaAudioSourcePrefix.size()) == kMediaAudioSourcePrefix;
}

// The id whose strip governs `sourceId` when it has none of its own. Returns
// either a static constant or `sourceId` itself (so the view lives as long as
// the argument does). Identity for everything that is not a media clip.
inline std::string_view audioControlSourceIdFor(std::string_view sourceId) {
  return isMediaClipAudioSourceId(sourceId) ? kMediaAudioControlSourceId : sourceId;
}

// Whether a PCM source is folded into the single "media" pre-sum: any media
// clip without an exact strip or exact send, plus a legacy source literally
// named "media" (so it cannot collide with the pre-sum's key).
inline bool joinsMediaAudioPreSum(std::string_view sourceId, bool hasOwnStrip, bool hasOwnSend) {
  if (sourceId == kMediaAudioControlSourceId) {
    return true;
  }
  return isMediaClipAudioSourceId(sourceId) && !hasOwnStrip && !hasOwnSend;
}

}  // namespace corevideo::core
