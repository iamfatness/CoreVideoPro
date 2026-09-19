#pragma once
#include <string>
#include <unordered_set>
#include <vector>

#include "core/SourceBus.h"
#include "core/ZoomParticipantSource.h"

namespace corevideo::core {

inline void syncZoomParticipantSources(SourceBus& bus,
                                       const std::vector<modules::VideoFrame>& zoomFrames) {
  std::unordered_set<std::string> present;
  for (const auto& f : zoomFrames) {
    present.insert(f.participantId);
    if (!bus.contains(f.participantId)) {
      const int w = f.i420Width > 0 ? f.i420Width : f.pixelWidth;
      const int h = f.i420Height > 0 ? f.i420Height : f.pixelHeight;
      bus.add(std::make_shared<ZoomParticipantSource>(f.participantId, w, h));
    }
    static_cast<ZoomParticipantSource*>(bus.sourceFor(f.participantId))->setLatest(f);
  }

  // Identify Zoom-owned sources by KIND, not id shape — the bus keys Zoom
  // sources by the raw participant id (no "zoom:" prefix, to match the
  // engine roster/continuity keying downstream in MediaCore), so a
  // prefix-based check here would never match and would leak every departed
  // participant's entry forever. Kind-based removal works regardless of id
  // shape and still never touches a non-zoom source (e.g. "test:pattern",
  // kind "test").
  for (const std::string& id : bus.sourceIds()) {
    const ISource* source = bus.sourceFor(id);
    if (source && source->descriptor().kind == "zoom" && present.find(id) == present.end()) {
      bus.remove(id);
    }
  }
}

}  // namespace corevideo::core
