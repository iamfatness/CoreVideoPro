#pragma once
#include "core/CaptureDeviceSource.h"
#include "core/MediaAssetSource.h"
#include "core/ZoomBusRoster.h"
#include <unordered_set>

namespace corevideo::core {

// Called and drained in one audio-worker gather, under the bus ownership lock.
// These mailboxes introduce no extra cadence, delay, or timestamp conversion.
inline void stageCaptureAudioSources(SourceBus& bus, std::vector<modules::AudioFrame> frames,
                                    const std::unordered_set<std::string>& configured) {
  for (const auto& id : configured) {
    if (!id.empty() && !bus.contains(id)) bus.add(std::make_shared<CaptureDeviceSource>(id, 0, 0));
  }
  for (const auto& id : bus.sourceIds()) {
    if (auto* source = dynamic_cast<CaptureDeviceSource*>(bus.sourceFor(id)))
      source->prepareAudio(configured.count(id) != 0);
  }
  for (auto& frame : frames) {
    if (frame.participantId.empty()) continue;
    if (!bus.contains(frame.participantId))
      bus.add(std::make_shared<CaptureDeviceSource>(frame.participantId, 0, 0));
    if (auto* source = dynamic_cast<CaptureDeviceSource*>(bus.sourceFor(frame.participantId))) {
      source->stageAudio(std::move(frame));
    } else {
      static std::atomic<uint32_t> collisions{0};
      if (collisions++ % 300 == 0)
        nativeLogf("[source-bus] capture PCM identity collision: %s\n", frame.participantId.c_str());
    }
  }
  for (const auto& id : bus.sourceIds()) {
    const auto* source = dynamic_cast<CaptureDeviceSource*>(bus.sourceFor(id));
    if (source && !source->descriptor().hasVideo && !source->descriptor().hasAudio) bus.remove(id);
  }
}

inline void stageMediaAudioSources(SourceBus& bus, std::vector<modules::AudioFrame> frames) {
  for (const auto& id : bus.sourceIds()) {
    if (auto* source = dynamic_cast<MediaAssetSource*>(bus.sourceFor(id))) source->clearAudio();
  }
  for (auto& frame : frames) {
    auto* source = dynamic_cast<MediaAssetSource*>(bus.sourceFor(frame.participantId));
    // Transport membership is command-owned. Never resurrect a retired decoder.
    if (source && source->descriptor().kind == "media") source->stageAudio(std::move(frame));
    else {
      static std::atomic<uint32_t> rejected{0};
      if (rejected++ % 300 == 0)
        nativeLogf("[source-bus] media PCM without owned transport: %s\n", frame.participantId.c_str());
    }
  }
}

inline std::vector<modules::AudioFrame> ingestSourceAudio(
    SourceBus& bus, std::vector<modules::AudioFrame> zoom,
    modules::IAudioCaptureSource* capture, modules::ICaptureDevice* transport,
    MediaTransports* media, const std::vector<modules::CaptureAudioSourceConfig>& configs,
    int64_t timestampMs, int64_t programTime100ns, int64_t nowNs) {
  stageZoomAudioSources(bus, std::move(zoom));
  std::unordered_set<std::string> configured;
  if (capture) for (const auto& c : configs) {
    if (!c.captureDeviceId.empty() && c.audioSourceKind != "none")
      configured.insert(c.captureDeviceId == "local-machine-audio" ? c.captureDeviceId : "capture:" + c.captureDeviceId);
  }
  auto frames = capture ? capture->pollAudioFrames(timestampMs) : std::vector<modules::AudioFrame>{};
  if (transport) {
    for (auto& id : transport->audioSourceIds()) configured.insert(std::move(id));
    for (auto& frame : transport->pollAudioFrames(timestampMs)) frames.push_back(std::move(frame));
  }
  stageCaptureAudioSources(bus, std::move(frames), configured);
  stageMediaAudioSources(bus, media ? media->popAudio(timestampMs) : std::vector<modules::AudioFrame>{});
  return bus.ingestAudio(programTime100ns, nowNs);
}
} // namespace corevideo::core
