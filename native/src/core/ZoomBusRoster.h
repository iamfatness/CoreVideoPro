#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/BoundedAsyncLog.h"
#include "core/SourceBus.h"
#include "core/ZoomParticipantSource.h"

namespace corevideo::core {

// `engineRoster` is the set of participant ids the engine still reports on
// its subscription roster (the ids MediaCore's roster merge keeps). A Zoom
// source is removed ONLY when the participant is absent from BOTH this tick's
// decoded frames AND that roster. A participant whose subscription the engine
// retired (video budget eviction, spine churn around a Take) has no decoded
// frame for a while but is still on the roster: the pre-bus
// RealZoomCaptureSource store kept painting their LAST frame across that gap,
// and removing the source here instead cut program to the fallback slate for
// ~500 ms (A/B 2026-09-19, recorded). An EMPTY roster removes nothing:
// MediaCore's merge only filters when the roster is non-empty, so the old
// path drew every stored frame in that state.
inline void syncZoomParticipantSources(SourceBus& bus,
                                       const std::vector<modules::VideoFrame>& zoomFrames,
                                       const std::unordered_set<std::string>& engineRoster) {
  std::unordered_set<std::string> present;
  for (const auto& f : zoomFrames) {
    present.insert(f.participantId);
    if (!bus.contains(f.participantId)) {
      const int w = f.i420Width > 0 ? f.i420Width : f.pixelWidth;
      const int h = f.i420Height > 0 ? f.i420Height : f.pixelHeight;
      bus.add(std::make_shared<ZoomParticipantSource>(f.participantId, w, h));
    }
    // A foreign kind squatting on this id would otherwise be silent UB under
    // this cast — skip the frame rather than reinterpret a different ISource
    // type.
    ISource* existing = bus.sourceFor(f.participantId);
    if (existing && existing->descriptor().kind == "zoom") {
      static_cast<ZoomParticipantSource*>(existing)->setLatest(f);
    } else if (existing) {
      static std::atomic<uint32_t> skips{0};
      if (skips++ % 300 == 0) {
        nativeLogf("[source-bus] zoom frame for '%s' skipped: bus entry is kind '%s'\n",
                   f.participantId.c_str(), existing->descriptor().kind.c_str());
      }
    }
  }

  // Identify Zoom-owned sources by KIND, not id shape — the bus keys Zoom
  // sources by the raw participant id (no "zoom:" prefix, to match the
  // engine roster/continuity keying downstream in MediaCore), so a
  // prefix-based check here would never match and would leak every departed
  // participant's entry forever. Kind-based removal works regardless of id
  // shape and still never touches a non-zoom source (e.g. "test:pattern",
  // kind "test").
  if (engineRoster.empty()) {
    return;
  }
  for (const std::string& id : bus.sourceIds()) {
    const ISource* source = bus.sourceFor(id);
    if (source && source->descriptor().kind == "zoom" && present.find(id) == present.end() &&
        engineRoster.find(id) == engineRoster.end()) {
      if (source->descriptor().hasAudio) {
        static_cast<ZoomParticipantSource*>(bus.sourceFor(id))->clearVideo();
      } else {
        bus.remove(id);
      }
    }
  }
}

// One pre-polled audio batch, moved under coreMutex and drained by ingestAudio
// in the same gather. No engine locks, extra queue latency, or PCM copying here.
// Audio-only guests share the same identity as their later camera frames.
inline void stageZoomAudioSources(SourceBus& bus, std::vector<modules::AudioFrame> frames) {
  for (const auto& id : bus.sourceIds()) {
    auto* source = dynamic_cast<ZoomParticipantSource*>(bus.sourceFor(id));
    if (source) source->clearAudio();
  }
  for (auto& frame : frames) {
    if (!bus.contains(frame.participantId)) {
      bus.add(std::make_shared<ZoomParticipantSource>(frame.participantId, 0, 0));
    }
    auto* source = dynamic_cast<ZoomParticipantSource*>(bus.sourceFor(frame.participantId));
    if (source) {
      source->stageAudio(std::move(frame));
    } else {
      static std::atomic<uint32_t> skips{0};
      if (skips++ % 300 == 0) {
        nativeLogf("[source-bus] zoom audio for '%s' skipped: source identity belongs to another adapter\n",
                   frame.participantId.c_str());
      }
    }
  }
  for (const auto& id : bus.sourceIds()) {
    const auto* source = dynamic_cast<ZoomParticipantSource*>(bus.sourceFor(id));
    if (source && !source->descriptor().hasAudio && !source->descriptor().hasVideo) bus.remove(id);
  }
}

}  // namespace corevideo::core
