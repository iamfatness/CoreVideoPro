#pragma once

#include <algorithm>
#include <cmath>
#include <string>

namespace corevideo::core {

// Pure, deterministic on-device director kernel.
//
// This is the C++ home of the upgraded AI director. It is a faithful port of the
// TypeScript reference `selectLocalProposal` (src/engine/localDirectorProvider.ts)
// reasoning over the richer signal bundle from `src/engine/directorSignals.ts`:
// conversational dynamics (dominant speaker, cross-talk), engagement (active
// contributors, audio energy), and feed health. It agrees with the count-only
// heuristic on the unambiguous cases (screen share, lone speaker, two-up
// interview, multi-speaker panel) and only diverges where the extra signal
// genuinely helps — e.g. a single dominant speaker carrying a large room reads
// cleaner as host-focus than a sparse grid.
//
// Like the renderer engine it is decision-pure: no I/O, no model, no network. The
// host (MediaCore) derives these signals from the core's current state and asks
// the kernel for a recommendation; the kernel never mutates anything.

// Coarse, transport-neutral signal inputs, mirroring the fields the TS director
// reads off DirectorIntelligenceSignals. All plain data.
struct DirectorSignals {
  // feedHealth.liveCount — live (non video-off) participant feeds.
  int liveCount = 0;
  // feedHealth.degradedCount — feeds that are recovering or low-resolution.
  int degradedCount = 0;
  // engagement.activeContributorCount — unmuted-or-talking live participants.
  int activeContributorCount = 0;
  // engagement.meanAudioLevel — mean audio level across live feeds (0..1).
  double meanAudioLevel = 0.0;
  // speakerTurns.dominantSpeakerId !== undefined — exactly one active speaker.
  bool hasDominantSpeaker = false;
  // speakerTurns.crossTalk — 2+ feeds active simultaneously.
  bool crossTalk = false;
  // screenShare.active — an active screen share is present.
  bool screenShareActive = false;
  // screenShare.sharerId present — a known sharer participant id.
  bool screenShareHasSharer = false;
  // screenShare.sharerName, when known, for the rationale text.
  std::string screenShareSharerName;
};

// The deterministic scene recommendation, mirroring DirectorProposal.
struct DirectorRecommendation {
  std::string ruleId;
  std::string recommendedSceneId;
  int confidence = 0;
  std::string rationale;
};

inline int clampDirectorConfidence(double value) {
  const double rounded = std::round(value);
  return static_cast<int>(std::max(0.0, std::min(100.0, rounded)));
}

// Faithful port of selectLocalProposal. Deterministic and pure.
inline DirectorRecommendation recommendScene(const DirectorSignals& signals) {
  const int liveCount = signals.liveCount;
  const int contributors = signals.activeContributorCount;
  const double energy = signals.meanAudioLevel;  // 0-1 coarse room energy
  const double degradePenalty = std::min(8.0, signals.degradedCount * 2.0);
  const bool hasDominant = signals.hasDominantSpeaker;

  // Active screen share is the safest, highest-priority live layout.
  if (signals.screenShareActive) {
    const std::string rationale =
        !signals.screenShareSharerName.empty()
            ? signals.screenShareSharerName + " is sharing; lead with the shared surface."
            : "Active screen share is the safest live layout.";
    return DirectorRecommendation{
        "screen-share-priority",
        "speaker-slides",
        clampDirectorConfidence(96.0 + (signals.screenShareHasSharer ? 1.0 : -2.0) - degradePenalty),
        rationale,
    };
  }

  // At most one live feed that can carry program -> host-focus framing.
  if (liveCount <= 1) {
    return DirectorRecommendation{
        "single-speaker",
        "intro",
        clampDirectorConfidence(90.0 + energy * 6.0 - degradePenalty),
        "Single live participant; host-focus framing.",
    };
  }

  // A lone contributor carrying several live feeds (everyone else muted/quiet)
  // reads cleaner as host-focus than a sparse grid — a call the count-only
  // heuristic only makes at 3+, but the contributor signal lets us make it
  // wherever it holds.
  if (hasDominant && contributors <= 1) {
    return DirectorRecommendation{
        "single-speaker",
        "intro",
        clampDirectorConfidence(89.0 + energy * 6.0 - degradePenalty),
        "A single dominant speaker is carrying the room; host-focus is cleaner than a grid.",
    };
  }

  // Two live contributors -> two-up interview. Cross-talk shaves certainty.
  if (liveCount == 2) {
    return DirectorRecommendation{
        "focused-interview",
        "interview",
        clampDirectorConfidence((hasDominant ? 92.0 : 89.0) - (signals.crossTalk ? 4.0 : 0.0) - degradePenalty),
        "Two live contributors; keep the conversation two-up.",
    };
  }

  // Three or more live contributors without slides -> panel. An energetic,
  // contested floor (multiple simultaneous speakers) reinforces keeping everyone
  // visible rather than chasing a single talker.
  const double panelConfidence = 90.0 + (contributors >= 3 ? 3.0 : 0.0) + (energy > 0.4 ? 2.0 : 0.0) -
                                 (signals.crossTalk ? 2.0 : 0.0) - degradePenalty;
  return DirectorRecommendation{
      "panel-discussion",
      "panel",
      clampDirectorConfidence(panelConfidence),
      "Multiple live contributors without slides; keep everyone visible.",
  };
}

}  // namespace corevideo::core
