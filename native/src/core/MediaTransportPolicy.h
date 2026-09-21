#pragma once
#include <optional>
#include <string>
namespace corevideo::core {
struct MediaTransportDesired {
  std::string sourceId;   // "media:<assetId>" | "background:<assetId>"
  std::string assetId, path, kind;
  bool loop = false;      // route.mediaAssetLoop; backgrounds always true
  bool onProgram = false, onPreview = false;
};
enum class MediaTransportState { Cued, Live, Paused, Ended };
enum class MediaTransportAction {
  None,         // nothing to do
  OpenLive,     // open a decoder, roll from 0, audio on            -> Live
  OpenCued,     // open a decoder, poster at 0, no audio            -> Cued
  Resume,       // the cued decoder rolls from 0, audio on          -> Live
  RestartCued,  // new decoder at 0 behind the held frame, no audio -> Cued
  Reopen,       // path changed: release + OpenLive/OpenCued        -> Live|Cued
  Pause,        // operator pause                                   -> Paused
  Play,         // operator play from the frozen position           -> Live
  Release       // absent from both buses: retire decoder + source
};
enum class MediaOperatorAction { Pause, Play };
inline const char* mediaTransportStateName(MediaTransportState s) {
  switch (s) { case MediaTransportState::Cued: return "cued"; case MediaTransportState::Live: return "live";
               case MediaTransportState::Paused: return "paused"; default: return "ended"; }
}
// Scene-driven decision. `previous`/`current` are the entry's desired rows before and after
// the command (nullopt = not desired). `state` is the entry's current state (ignored when
// previous is nullopt). Returns the action and the state that results from it.
struct MediaTransportDecision { MediaTransportAction action; MediaTransportState next; };
inline MediaTransportDecision decideMediaTransport(const std::optional<MediaTransportDesired>& previous,
                                                   const std::optional<MediaTransportDesired>& current,
                                                   MediaTransportState state) {
  using A = MediaTransportAction; using S = MediaTransportState;
  if (!current) return {previous ? A::Release : A::None, state};
  const bool wantsLive = current->onProgram || current->loop;   // a loop is live on any bus
  if (!previous) return wantsLive ? MediaTransportDecision{A::OpenLive, S::Live} : MediaTransportDecision{A::OpenCued, S::Cued};
  if (previous->path != current->path || previous->loop != current->loop)
    return wantsLive ? MediaTransportDecision{A::Reopen, S::Live} : MediaTransportDecision{A::Reopen, S::Cued};
  if (current->loop) return {A::None, S::Live};
  const bool wasOnProgram = previous->onProgram;
  if (!wasOnProgram && current->onProgram) return {A::Resume, S::Live};          // enters Program
  if (wasOnProgram && !current->onProgram) return {A::RestartCued, S::Cued};     // leaves Program, still cued
  return {A::None, state};                                                        // same bus membership
}
// Operator pause/play. Returns nullopt with `reason` filled when refused (loop, or not on Program).
inline std::optional<MediaTransportDecision> decideMediaOperator(const MediaTransportDesired& current,
    MediaTransportState state, MediaOperatorAction action, std::string& reason) {
  using A = MediaTransportAction; using S = MediaTransportState;
  if (current.loop) { reason = current.sourceId + " is a loop and cannot be paused or played."; return std::nullopt; }
  if (!current.onProgram) { reason = current.sourceId + " is not on Program; pause/play applies to the Program clip only."; return std::nullopt; }
  if (action == MediaOperatorAction::Pause) {
    if (state == S::Live) return MediaTransportDecision{A::Pause, S::Paused};
    return MediaTransportDecision{A::None, state};
  }
  if (state == S::Paused) return MediaTransportDecision{A::Play, S::Live};
  if (state == S::Ended) return MediaTransportDecision{A::OpenLive, S::Live};
  return MediaTransportDecision{A::None, state};
}
}  // namespace corevideo::core
