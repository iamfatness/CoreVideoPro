#pragma once

#include <cstdint>

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
struct TakeRecordPolicy {
  struct Observation {
    bool hasWallAfter = false;         // the taken scene carries a Tiles wall
    bool wallAdoptedSettled = false;   // adoptSettledFrom() returned true
    bool liveBackgroundExpected = false;   // the wall declares a live source background
    bool liveBackgroundEmitted = false;    // ...and it was in the first program frame
    std::uint64_t subscriptionChurnDelta = 0;  // real re-subscribes across the take
  };

  struct Verdict {
    // How the wall arrived on Program.
    const char* wall = "none";       // none | adopted-settled | reset
    // The one-word answer to "did the wall rebuild or cut".
    const char* verdict = "no-wall";  // cut | rebuilt | no-wall
    // Whether anything other than the render plan could explain a rebuild.
    bool backgroundDropped = false;
    bool subscriptionsChurned = false;
  };

  [[nodiscard]] static Verdict evaluate(const Observation& observation) {
    Verdict verdict;
    verdict.backgroundDropped =
        observation.liveBackgroundExpected && !observation.liveBackgroundEmitted;
    verdict.subscriptionsChurned = observation.subscriptionChurnDelta > 0;
    if (!observation.hasWallAfter) {
      verdict.wall = "none";
      verdict.verdict = "no-wall";
      return verdict;
    }
    if (!observation.wallAdoptedSettled) {
      verdict.wall = "reset";
      verdict.verdict = "rebuilt";
      return verdict;
    }
    verdict.wall = "adopted-settled";
    // The animator cut cleanly. If the background never made the first frame,
    // or a subscription was torn down in the same tick, the operator can still
    // have seen a rebuild — say so instead of certifying a clean cut.
    verdict.verdict =
        (verdict.backgroundDropped || verdict.subscriptionsChurned) ? "rebuilt" : "cut";
    return verdict;
  }
};

}  // namespace corevideo::core
