#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace corevideo::compositor {

// The shell decides who is ELIGIBLE for the wall; the core decides who is
// actually DRAWN. Only the core knows whether frames are arriving, and a wall
// that holds a slot for a dead feed shows a black square. The plugin accepts
// that trade; we do not have to.
constexpr int64_t kTilesStaleFrameMs = 1500;

struct TilesMemberFrameAge {
  std::string sourceId;
  bool hasFrame = false;
  int64_t lastFrameAgeMs = 0;
};

inline std::vector<std::string> admitTilesMembers(
    const std::vector<std::string>& members,
    const std::vector<TilesMemberFrameAge>& ages,
    int64_t staleAfterMs = kTilesStaleFrameMs) {
  std::vector<std::string> admitted;
  std::unordered_set<std::string> seen;
  admitted.reserve(members.size());
  for (const auto& member : members) {
    if (!seen.insert(member).second) {
      continue;
    }
    for (const auto& age : ages) {
      if (age.sourceId == member) {
        if (age.hasFrame && age.lastFrameAgeMs <= staleAfterMs) {
          admitted.push_back(member);
        }
        break;
      }
    }
  }
  return admitted;
}

// A wall's live BACKGROUND is held across a stale beat; a TILE is not.
// (Owner report, live broadcast 2026-09-09: "Tiles background still refreshing
// on cut to program, that should be seamless.")
//
// The background feed used to run through admitTilesMembers — the SAME 1500ms
// staleness gate the tiles go through — so the instant the background source's
// frameId stopped advancing for a beat the `tiles-source-bg:` layer was not
// emitted at all: PROGRAM fell through to the wall's solid colour (or the scene
// background) and the picture POPPED back when frames resumed. Across a take
// that reads as the wall reloading, which is exactly what the take hand-off in
// TilesPlanAnimation exists to remove.
//
// The two are not the same decision, and the difference is the whole reason
// this is a separate predicate rather than a widened kTilesStaleFrameMs (which
// is shared with tile admission and must NOT move — a looser threshold changes
// wall MEMBERSHIP for every source):
//   * a stale TILE occupies a slot. Holding it shows a dead guest in a seat
//     someone live could have had — the trade the header comment above
//     deliberately refuses.
//   * a stale BACKGROUND occupies nothing. It competes with no one, and a
//     backdrop frozen for a beat is visually indistinguishable from a live one,
//     whereas its absence is a full-frame colour change on air.
//
// THE FABRICATION RULE IS UNCHANGED, and it is what `hasFrame` buys: this is
// evidence, not memory. `hasFrame` is true only while a real-content frame for
// THAT source is in this tick's videoFrames gather (see the tilesMemberFrameAges_
// build in MediaCore::renderDisplayTick — a member with no matching frame is
// recorded hasFrame=false and has its freshness history erased). So a source
// that never arrived, or that has genuinely departed, is still refused exactly
// as it is today, and there is no retained layer that could outlive its source.
// That also means there is no per-bus state here to leak: the answer is derived
// from the frame gather, which is shared, so Preview and Program cannot
// contaminate each other through it the way retained animation state could.
//
// It matters that we never emit a background layer without a frame: a
// participant-video layer whose sourceId resolves to no frame renders a solid
// colorFromParticipantId() placeholder (D3D11CompositorAdapter::resolveLayers),
// i.e. an arbitrary colour slab painted OVER the wall background — worse than
// the pop this fixes. The layer also always carries a non-empty participantId,
// so core::resolveRouteSource's positional fallback (RouteSourcePolicy.h, the
// "empty route inherits videoFrames[index]" hazard) is unreachable from here.
inline bool tilesBackgroundSourceIsDrawable(const std::string& sourceId,
                                            const std::vector<TilesMemberFrameAge>& ages) {
  if (sourceId.empty()) {
    return false;
  }
  for (const auto& age : ages) {
    if (age.sourceId == sourceId) {
      return age.hasFrame;
    }
  }
  return false;
}

}  // namespace corevideo::compositor
