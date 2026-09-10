#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace corevideo::core {

// DID THE WALL CUT, OR DID IT REBUILD?
//
// A Take of a Tiles scene is supposed to be a CUT to something already
// composited on Preview: `TilesPlanAnimation::adoptSettledFrom` moves the
// settled animator across buses so the wall continues instead of replaying its
// entrance. When adoption is refused (keys differ, or the preview wall was not
// at rest) the program animator resets and every tile animates in from scratch
// — on air that reads as the wall redrawing itself.
//
// Two other things can produce the identical picture and must not be confused
// with it, which is why they are inputs here rather than a second guess later:
//   * the wall's live background layer (`tiles-source-bg:`) missing from the
//     first program frame after the cut, and
//   * a Zoom subscription actually churning in the same tick (a resolution
//     change or a cap eviction is an engine-side renderer teardown; frames stop
//     arriving and the wall or its background goes black and re-decodes).
//
// Pure decision, in the OutputLifecyclePolicy / CaptureReaderStallPolicy shape.
//
// The wall is not the only thing that can rebuild. A Take between two scenes
// that SHARE a source (the owner's case: a gallery's media background and
// foreground on both Preview and Program) is only a cut if that source kept
// running across it. So the record also carries every frame source present on
// both sides with its SourceContinuityLedger generation before and after — a
// generation that moved is a decoder that reopened on air — and every source
// the take brought on air that had no frame on its first program tick (a cold
// start the operator watched). Either one refuses the word "cut".
struct TakeRecordPolicy {
  struct SharedSourceContinuity {
    std::string sourceId;
    std::uint64_t generationBefore = 0, generationAfter = 0;
    std::int64_t frameIdBefore = -1, frameIdAfter = -1;
  };

  struct Observation {
    bool hasWallAfter = false;         // the taken scene carries a Tiles wall
    bool wallAdoptedSettled = false;   // adoptSettledFrom() returned true
    bool liveBackgroundExpected = false;   // the wall declares a live source background
    bool liveBackgroundEmitted = false;    // ...and it was in the first program frame
    std::uint64_t subscriptionChurnDelta = 0;  // real re-subscribes across the take
    // Every frame source present in BOTH the outgoing (Program + Preview) and
    // incoming plans, with its SourceContinuityLedger generation on each side.
    std::vector<SharedSourceContinuity> sharedSources;
    // Incoming-plan sources with no frame on the first program tick that were
    // expected to have one (a media layer, or a source already running before).
    std::vector<std::string> sourcesMissingOnFirstFrame;
  };

  struct Verdict {
    // How the wall arrived on Program.
    const char* wall = "none";       // none | adopted-settled | reset
    // The one-word answer to "did the take rebuild or cut".
    const char* verdict = "no-wall";  // cut | rebuilt | no-wall
    // Whether anything other than the render plan could explain a rebuild.
    bool backgroundDropped = false;
    bool subscriptionsChurned = false;
    bool sharedSourceRestarted = false;
    std::vector<std::string> restartedSources;  // sourceIds whose generation changed
    bool sourceMissing = false;
    std::vector<std::string> missingSources;    // no frame on the first program tick
  };

  [[nodiscard]] static Verdict evaluate(const Observation& observation) {
    Verdict verdict;
    verdict.backgroundDropped =
        observation.liveBackgroundExpected && !observation.liveBackgroundEmitted;
    verdict.subscriptionsChurned = observation.subscriptionChurnDelta > 0;
    for (const auto& source : observation.sharedSources) {
      if (source.generationAfter != source.generationBefore) {
        verdict.restartedSources.push_back(source.sourceId);
      }
    }
    verdict.sharedSourceRestarted = !verdict.restartedSources.empty();
    verdict.missingSources = observation.sourcesMissingOnFirstFrame;
    verdict.sourceMissing = !verdict.missingSources.empty();
    const bool sourcesBroke = verdict.sharedSourceRestarted || verdict.sourceMissing;

    if (!observation.hasWallAfter) {
      verdict.wall = "none";
      // No wall: the take is about its sources. Nothing shared and nothing
      // missing is the old "no-wall"; a restart or a cold start is a rebuild on
      // air; shared sources that all kept running are a cut.
      if (sourcesBroke) verdict.verdict = "rebuilt";
      else verdict.verdict = observation.sharedSources.empty() ? "no-wall" : "cut";
      return verdict;
    }
    if (!observation.wallAdoptedSettled) {
      verdict.wall = "reset";
      verdict.verdict = "rebuilt";
      return verdict;
    }
    verdict.wall = "adopted-settled";
    // The animator cut cleanly. If the background never made the first frame,
    // a subscription was torn down in the same tick, or a source restarted or
    // cold-started, the operator can still have seen a rebuild — say so
    // instead of certifying a clean cut.
    verdict.verdict = (verdict.backgroundDropped || verdict.subscriptionsChurned || sourcesBroke)
                          ? "rebuilt"
                          : "cut";
    return verdict;
  }
};

}  // namespace corevideo::core
