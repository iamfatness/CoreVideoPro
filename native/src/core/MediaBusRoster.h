// native/src/core/MediaBusRoster.h
#pragma once
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "core/MediaAssetSource.h"
#include "core/SourceBus.h"

namespace corevideo::core {

// Media owner -> bus, one MediaAssetSource per requested-key frame the owner
// emitted THIS tick, for exactly one kind ("media" — decoded clips/loops/
// backgrounds from IMediaFrameSource — or "still" — route stills from
// StillMediaFrameCache). The owner is the frame holder: it emits a frame for
// every REQUESTED key each tick (a paused clip keeps emitting its held frame);
// a key absent from the poll is no longer requested or has not decoded, and
// nothing is drawn for it today. So, like capture (#535 slice 2) and unlike
// Zoom (#554), absent from the tick == removed. The two media kinds are
// independent: syncing one kind never touches sources of the other kind, or
// of any other kind on the bus (Zoom/capture/test).
inline void syncMediaSources(SourceBus& bus, const std::vector<modules::VideoFrame>& frames,
                             std::string_view kind) {
  std::unordered_set<std::string> present;
  for (const auto& f : frames) {
    present.insert(f.participantId);
    if (!bus.contains(f.participantId)) {
      const int w = f.pixelWidth > 0 ? f.pixelWidth : f.i420Width;
      const int h = f.pixelHeight > 0 ? f.pixelHeight : f.i420Height;
      bus.add(std::make_shared<MediaAssetSource>(f.participantId, std::string(kind), w, h));
    }
    // A foreign kind squatting on this id (e.g. the OTHER media kind, or a
    // stale entry of a different kind entirely) would otherwise be silent UB
    // under this cast — skip the frame rather than reinterpret a different
    // ISource type.
    ISource* existing = bus.sourceFor(f.participantId);
    if (existing && existing->descriptor().kind == kind) {
      static_cast<MediaAssetSource*>(existing)->setLatest(f);
    }
  }
  for (const std::string& id : bus.sourceIds()) {
    const ISource* source = bus.sourceFor(id);
    if (source && source->descriptor().kind == kind && present.find(id) == present.end()) {
      bus.remove(id);
    }
  }
}

}  // namespace corevideo::core
