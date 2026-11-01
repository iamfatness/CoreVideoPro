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

// #1071: only a ZOOM member ages out on a frozen frameId. A Zoom guest whose
// frameId stops advancing is a stalled feed (the I4 frozen-but-subscribed
// defect), and leaving its seat empty is the right trade. A LOCAL member
// (capture:/media:) legitimately re-serves the same frameId for long stretches
// while perfectly healthy: an idle desktop share, a paused clip, a PNG logo
// (StillMediaFrameCache keeps a stable frameId by design; see the on-air
// "stalled" rule in MediaCore::annotateLayerSource, #535 slice 4a R1). Ageing
// those out would empty a Tiles slot 1.5 s after seating a static source and
// reflow the wall around it. So a local member is admitted on `hasFrame` alone
// — the frame is still in this tick's gather, so nothing is fabricated — and
// it drops the tick its frame LEAVES the gather (unplugged, un-routed,
// departed). Whether a frozen capture device is a stall is capture liveness,
// #562's decision, not a frame-age heuristic that cannot tell a dead camera
// from a still slide.
inline bool tilesMemberAgesOut(const std::string& sourceId) {
  return sourceId.rfind("zoom:", 0) == 0;
}

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
        if (age.hasFrame && (!tilesMemberAgesOut(member) || age.lastFrameAgeMs <= staleAfterMs)) {
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
// participant-video layer whose sourceId resolves to no frame renders the
// bus-health slate (warming/failed, #535 slice 4a — see
// compositor::slateColorFor in D3D11CompositorAdapter::resolveLayers),
// i.e. a slab painted OVER the wall background — worse than the pop this fixes. The layer also always carries a non-empty participantId,
// so a sourceless route cannot be invented here (#480: empty routes render
// BLANK, never videoFrames[index]).
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
