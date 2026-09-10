#pragma once

#include <set>
#include <string>
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
// The alias: the "media" strip and "media" sends govern every `media:*` source.
// Each clip keeps its own id everywhere else. An EXPLICIT per-clip strip or send
// (one whose id is exactly `media:<assetId>`) still wins over the alias.
//
// Do NOT fix this by making the shell send per-clip ids: the routing grid
// un-routes any cell the core did not echo, so a "media" row would switch itself
// off ~2 s later (AudioRoutingMatrixViewModel.ApplyCoreSends).
inline constexpr std::string_view kMediaAudioControlSourceId = "media";
inline constexpr std::string_view kMediaAudioSourcePrefix = "media:";

// The id whose strip/sends govern `sourceId` when it has none of its own.
// Identity for everything that is not a media clip.
inline std::string audioControlSourceIdFor(const std::string& sourceId) {
  if (sourceId.size() > kMediaAudioSourcePrefix.size() &&
      std::string_view(sourceId).substr(0, kMediaAudioSourcePrefix.size()) == kMediaAudioSourcePrefix) {
    return std::string(kMediaAudioControlSourceId);
  }
  return sourceId;
}

// The source id whose SENDS route `sourceId`: its own when any send names it
// exactly (an explicit per-source route wins as a whole), otherwise its alias.
// `sendSourceIds` is the set of sourceIds named by the routing sends.
inline std::string routingSendSourceIdFor(const std::string& sourceId,
                                          const std::set<std::string>& sendSourceIds) {
  if (sendSourceIds.count(sourceId) != 0) {
    return sourceId;
  }
  return audioControlSourceIdFor(sourceId);
}

}  // namespace corevideo::core
