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
//
// A member whose newest frame is older than this is STALE. It is no longer
// dropped at this age (that was the behaviour until #1074): it is HELD.
constexpr int64_t kTilesStaleFrameMs = 1500;

// #1074 (live show 2026-10-31): the operator's uplink stalled for 1 to 3 s at
// a time, Zoom video and the RTMP pipe stopped together, and every stall past
// 1.5 s made a tile vanish, the wall reflow, and the tile pop back. The
// owner read that as "participants dropping". A stale tile is now HELD: its
// last picture stays in its slot, counted and labelled `held` in the snapshot
// (the same rule the ISO writers follow for a stalled source, #1038), until
// the gap exceeds this window measured from the member's last NEW frame.
// Only then is the member dropped. Video-off and departure drop immediately
// regardless, because the frame leaves the gather (`hasFrame` false).
//
// 4 s is the issue's "short gap" range (3 to 5 s); it is a working
// assumption, not yet an owner ruling (#427 still carries the handoff
// decision). It is a separate constant on purpose: widening
// kTilesStaleFrameMs would also delay the on-air "stalled" label and the
// per-source dropout policy, which key off the same 1.5 s.
constexpr int64_t kTilesHoldWindowMs = 4000;
static_assert(kTilesHoldWindowMs > kTilesStaleFrameMs,
              "a held tile must outlive the stale threshold or hold means nothing");

struct TilesMemberFrameAge {
  std::string sourceId;
  bool hasFrame = false;
  int64_t lastFrameAgeMs = 0;
};

// What the wall does with one member this tick.
enum class TilesMemberState { Fresh, Held, Dropped };

inline TilesMemberState classifyTilesMember(const TilesMemberFrameAge& age,
                                            int64_t staleAfterMs = kTilesStaleFrameMs,
                                            int64_t holdWindowMs = kTilesHoldWindowMs) {
  if (!age.hasFrame) {
    return TilesMemberState::Dropped;
  }
  if (age.lastFrameAgeMs <= staleAfterMs) {
    return TilesMemberState::Fresh;
  }
  return age.lastFrameAgeMs <= holdWindowMs ? TilesMemberState::Held : TilesMemberState::Dropped;
}

// The drawn members in slot order (fresh AND held), plus the subset that is
// held so the plan can label them. A member with no age entry, or with no
// frame in this tick's gather, is not drawn: nothing is ever fabricated.
struct TilesAdmission {
  std::vector<std::string> admitted;
  std::vector<std::string> held;
};

inline TilesAdmission admitTilesMembersWithHold(
    const std::vector<std::string>& members,
    const std::vector<TilesMemberFrameAge>& ages,
    int64_t staleAfterMs = kTilesStaleFrameMs,
    int64_t holdWindowMs = kTilesHoldWindowMs) {
  TilesAdmission result;
  std::unordered_set<std::string> seen;
  result.admitted.reserve(members.size());
  for (const auto& member : members) {
    if (!seen.insert(member).second) {
      continue;
    }
    for (const auto& age : ages) {
      if (age.sourceId == member) {
        switch (classifyTilesMember(age, staleAfterMs, holdWindowMs)) {
          case TilesMemberState::Fresh:
            result.admitted.push_back(member);
            break;
          case TilesMemberState::Held:
            result.admitted.push_back(member);
            result.held.push_back(member);
            break;
          case TilesMemberState::Dropped:
            break;
        }
        break;
      }
    }
  }
  return result;
}

inline std::vector<std::string> admitTilesMembers(
    const std::vector<std::string>& members,
    const std::vector<TilesMemberFrameAge>& ages,
    int64_t staleAfterMs = kTilesStaleFrameMs,
    int64_t holdWindowMs = kTilesHoldWindowMs) {
  return admitTilesMembersWithHold(members, ages, staleAfterMs, holdWindowMs).admitted;
}

// A wall's live BACKGROUND is held across a stale beat for as long as a frame
// exists; a TILE is held for a BOUNDED window (kTilesHoldWindowMs, #1074) and
// then dropped. (Owner report, live broadcast 2026-09-09: "Tiles background
// still refreshing on cut to program, that should be seamless.")
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
//   * a stale TILE occupies a slot. Holding it past the hold window shows a
//     dead guest in a seat someone live could have had — the trade the header
//     comment above refuses. Holding it THROUGH a short network gap (#1074) is
//     the opposite trade: a slot that empties and refills every time the
//     uplink hiccups reads as guests leaving, and a 4 s bound keeps a dead
//     feed from squatting.
//   * a stale BACKGROUND occupies nothing. It competes with no one, and a
//     backdrop frozen for a beat is visually indistinguishable from a live one,
//     whereas its absence is a full-frame colour change on air. It has no
//     hold window at all.
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
