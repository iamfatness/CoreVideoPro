#pragma once
#include <string>
#include <unordered_set>
#include <vector>

#include "core/SourceBus.h"
#include "core/ZoomParticipantSource.h"

namespace corevideo::core {

inline bool isZoomSourceId(const std::string& id) { return id.rfind("zoom:", 0) == 0; }

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

  for (const std::string& id : bus.sourceIds()) {
    if (isZoomSourceId(id) && present.find(id) == present.end()) bus.remove(id);
  }
}

}  // namespace corevideo::core
