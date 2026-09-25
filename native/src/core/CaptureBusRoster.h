// native/src/core/CaptureBusRoster.h
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/BoundedAsyncLog.h"
#include "core/CaptureDeviceSource.h"
#include "core/SourceBus.h"

namespace corevideo::core {

// Capture adapters → bus, one CaptureDeviceSource per "capture:<id>" frame the
// adapters emitted THIS tick. The adapter is the frame holder: it re-emits its
// last frame every tick while the device is connected and emits nothing once it
// is disconnected or before it ever delivered — which is exactly what the
// compositor drew before the bus. An absent video is cleared immediately;
// configured audio can keep the source identity alive. Zoom instead retains
// video through subscription gaps (#554); capture adapters already re-emit
// their held picture, so a missing capture frame removes video membership.
inline void publishHeldCaptureFrame(SourceBus& bus, modules::VideoFrame frame) {
  if (!bus.contains(frame.participantId)) {
    const int w = frame.pixelWidth > 0 ? frame.pixelWidth : frame.i420Width;
    const int h = frame.pixelHeight > 0 ? frame.pixelHeight : frame.i420Height;
    bus.add(std::make_shared<CaptureDeviceSource>(frame.participantId, w, h));
  }
  ISource* existing = bus.sourceFor(frame.participantId);
  if (auto* capture = dynamic_cast<CaptureDeviceSource*>(existing)) {
    capture->setLatest(std::move(frame));
    capture->holdVideo(true);
  } else if (existing) {
    static std::atomic<uint32_t> skips{0};
    if (skips++ % 300 == 0) {
      nativeLogf("[source-bus] capture frame for '%s' skipped: bus entry is kind '%s'\n",
                 frame.participantId.c_str(), existing->descriptor().kind.c_str());
    }
  }
}

inline void endHeldCaptureFrame(SourceBus& bus, const std::string& id) {
  auto* source = dynamic_cast<CaptureDeviceSource*>(bus.sourceFor(id));
  if (!source) return;
  source->holdVideo(false);
  source->clearVideo();
  if (!source->descriptor().hasAudio) bus.remove(id);
}

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
    // A foreign kind squatting on this id (e.g. a stale Zoom entry that has
    // not yet been released) would otherwise be silent UB under this cast —
    // skip the frame rather than reinterpret a different ISource type.
    ISource* existing = bus.sourceFor(f.participantId);
    if (auto* capture = dynamic_cast<CaptureDeviceSource*>(existing)) {
      capture->setLatest(f);
    } else if (existing) {
      static std::atomic<uint32_t> skips{0};
      if (skips++ % 300 == 0) {
        nativeLogf("[source-bus] capture frame for '%s' skipped: bus entry is kind '%s'\n",
                   f.participantId.c_str(), existing->descriptor().kind.c_str());
      }
    }
  }
  for (const std::string& id : bus.sourceIds()) {
    auto* source = dynamic_cast<CaptureDeviceSource*>(bus.sourceFor(id));
    if (source && !source->videoHeld() && present.find(id) == present.end()) {
      source->clearVideo();
      if (!source->descriptor().hasAudio) bus.remove(id);
    }
  }
}

}  // namespace corevideo::core
