#pragma once

// T1.11 / #449 — may a Preview cue's WARM decoder be handed to Program?
//
// A clip cued in Preview and then taken changes identity twice: the `preview:`
// namespace that keeps a paused poster from replacing Program's rolling copy
// collapses, and MediaGoLiveLedger advances the generation baked into the
// playback key. The arriving request therefore matched no entry in
// OwnedMediaFrameSource, a cold decoder opened, and for the ticks before its
// first frame the compositor painted colorFromParticipantId over Program.
//
// The cue poster sits PAUSED AT FRAME 0 (MediaVideoPresentation::hold shows the
// first prepared frame and does not advance), so resuming that decoder is
// exactly the go-live contract's "roll from 0, audio on" — not an exception to
// it. That equivalence is the whole justification, and it is why
// `retiringEverPlayed` is a hard refusal: a decoder that has already rolled is
// at an arbitrary position, and adopting it would put a clip on air mid-roll
// while the take record still read `cut`.
//
// Pure and free of threads, clocks and decoders on purpose (the
// CaptureReaderStallPolicy / DeviceLossPolicy / TakeRecordPolicy shape), so
// every refusal is a unit test rather than a timing experiment.

#include <string>

namespace corevideo::modules {

// One media decoder request, reduced to the fields that decide its identity.
struct MediaSourceRequest {
  std::string sourceId;         // "media:<assetId>", or "preview:media:<assetId>" for a cue
  std::string mediaAssetId;
  std::string mediaAssetPath;
  std::string mediaPlaybackKey; // "media:<assetId>:live:<n>" for a clip; "media:<assetId>" for a loop
  bool loop = false;
};

// Parses the go-live generation from a CLIP's playback key. Returns false for a
// loop key, a foreign asset id, a missing suffix or a non-numeric one — never a
// guess, because a misparsed generation would authorise a hand-over the shell
// never asked for.
inline bool mediaGoLiveGeneration(const std::string& playbackKey, const std::string& mediaAssetId,
                                  long long& generation) {
  const std::string prefix = "media:" + mediaAssetId + ":live:";
  if (playbackKey.size() <= prefix.size()) return false;
  if (playbackKey.compare(0, prefix.size(), prefix) != 0) return false;
  const auto digits = playbackKey.substr(prefix.size());
  if (digits.empty() || digits.size() > 18) return false;
  for (const char c : digits) {
    if (c < '0' || c > '9') return false;
  }
  generation = std::stoll(digits);
  return true;
}

// True when `arriving` is the go-live successor of the Preview cue `retiring`
// and that cue has never rolled, so the warm decoder may be adopted instead of
// cold-started. Every condition is required; the caller refuses ambiguity
// (more than one retiring candidate) separately.
inline bool isCueHandoff(const MediaSourceRequest& retiring, const MediaSourceRequest& arriving,
                         bool retiringEverPlayed) {
  if (retiringEverPlayed) return false;
  if (retiring.loop || arriving.loop) return false;
  if (arriving.sourceId.empty() || arriving.mediaAssetId.empty()) return false;
  if (retiring.mediaAssetId != arriving.mediaAssetId) return false;
  if (retiring.mediaAssetPath != arriving.mediaAssetPath) return false;
  if (retiring.sourceId != "preview:" + arriving.sourceId) return false;
  long long cueGeneration = 0, liveGeneration = 0;
  if (!mediaGoLiveGeneration(retiring.mediaPlaybackKey, retiring.mediaAssetId, cueGeneration)) return false;
  if (!mediaGoLiveGeneration(arriving.mediaPlaybackKey, arriving.mediaAssetId, liveGeneration)) return false;
  return liveGeneration == cueGeneration + 1;
}

}  // namespace corevideo::modules
