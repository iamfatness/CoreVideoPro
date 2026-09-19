// native/src/core/CaptureBusRoster.h
#pragma once
#include <string>
#include <unordered_set>
#include <vector>

#include "core/CaptureDeviceSource.h"
#include "core/SourceBus.h"

namespace corevideo::core {

// Capture adapters → bus, one CaptureDeviceSource per "capture:<id>" frame the
// adapters emitted THIS tick. The adapter is the frame holder: it re-emits its
// last frame every tick while the device is connected and emits nothing once it
// is disconnected or before it ever delivered — which is exactly what the
// compositor drew before the bus. So a capture source absent from the tick is
// removed. This is the OPPOSITE of syncZoomParticipantSources on purpose: the
// Zoom engine erases its decoded frame on unsubscribe and the pre-bus store
// masked that (#554); no capture adapter erases on a subscription gap.
inline void syncCaptureSources(SourceBus& bus,
                               const std::vector<modules::VideoFrame>& captureFrames) {
  std::unordered_set<std::string> present;
  for (const auto& f : captureFrames) {
    present.insert(f.participantId);
    if (!bus.contains(f.participantId)) {
      const int w = f.pixelWidth > 0 ? f.pixelWidth : f.i420Width;
      const int h = f.pixelHeight > 0 ? f.pixelHeight : f.i420Height;
      bus.add(std::make_shared<CaptureDeviceSource>(f.participantId, w, h));
    }
    static_cast<CaptureDeviceSource*>(bus.sourceFor(f.participantId))->setLatest(f);
  }
  for (const std::string& id : bus.sourceIds()) {
    const ISource* source = bus.sourceFor(id);
    if (source && source->descriptor().kind == "capture" && present.find(id) == present.end()) {
      bus.remove(id);
    }
  }
}

}  // namespace corevideo::core
