#include "core/SourceVideoIngress.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "core/CaptureBusRoster.h"
#include "core/MediaBusRoster.h"
#include "core/MediaTransports.h"

namespace corevideo::core {

CaptureVideoToSourceBus::CaptureVideoToSourceBus(SourceBus& bus) : bus_(bus) {}
void CaptureVideoToSourceBus::publish(modules::VideoFrame frame) {
  publishHeldCaptureFrame(bus_, std::move(frame));
}
void CaptureVideoToSourceBus::end(const std::string& participantId) {
  endHeldCaptureFrame(bus_, participantId);
}

// The native adapters publish onto SourceBus before this function runs. Keep
// the legacy draw order and the Zoom subscription roster's authority in one
// place: capture and media survive the roster rebuild, while an absent Zoom
// frame can still be represented by its metadata-only roster entry.
std::vector<modules::VideoFrame> gatherSourceVideo(
    SourceBus& bus, MediaTransports* mediaTransports,
    const std::vector<modules::VideoFrame>& engineFrames,
    const std::vector<modules::VideoFrame>& browserFrames,
    bool engineLive, int64_t mediaPresentationTime100ns, int64_t nowNs) {
  syncCaptureSources(bus, browserFrames);
  if (mediaTransports) {
    for (const auto& change : mediaTransports->collectExpiredReleases(nowNs)) {
      if (!change.added) bus.remove(change.sourceId);
    }
  }

  std::vector<modules::VideoFrame> captureFrames, slateFrames, zoomFrames, mediaFrames;
  if (!bus.empty()) {
    auto result = bus.ingest(mediaPresentationTime100ns, nowNs,
        [engineLive](const SourceDescriptor& descriptor) {
          if (descriptor.kind == "still") return false;
          if (engineLive && descriptor.kind == "zoom-slate") return false;
          return true;
        });
    for (size_t index = 0; index < result.video.size(); ++index) {
      const auto& kind = index < result.videoKinds.size() ? result.videoKinds[index] : std::string{};
      auto& bucket = kind == "zoom-slate" ? slateFrames
          : kind == "capture" ? captureFrames
          : kind == "media" ? mediaFrames : zoomFrames;
      bucket.push_back(std::move(result.video[index]));
    }
  }

  std::vector<modules::VideoFrame> frames;
  frames.insert(frames.end(), slateFrames.begin(), slateFrames.end());
  frames.insert(frames.end(), captureFrames.begin(), captureFrames.end());
  frames.insert(frames.end(), std::make_move_iterator(zoomFrames.begin()),
                std::make_move_iterator(zoomFrames.end()));
  frames.insert(frames.end(), mediaFrames.begin(), mediaFrames.end());
  if (engineLive && !engineFrames.empty()) {
    std::vector<modules::VideoFrame> merged;
    merged.reserve(engineFrames.size() + captureFrames.size() + mediaFrames.size());
    for (auto engineFrame : engineFrames) {
      const auto withContent = std::find_if(frames.begin(), frames.end(),
          [&](const modules::VideoFrame& candidate) {
            return candidate.participantId == engineFrame.participantId &&
                   (candidate.hasPixels() || candidate.hasI420());
          });
      merged.push_back(withContent != frames.end() ? *withContent : std::move(engineFrame));
    }
    for (auto& capture : captureFrames) merged.push_back(std::move(capture));
    for (auto& media : mediaFrames) merged.push_back(std::move(media));
    return merged;
  }
  return frames;
}

void appendStillSourceVideo(SourceBus& bus,
    const std::vector<modules::VideoFrame>& stillFrames,
    int64_t mediaPresentationTime100ns, int64_t nowNs,
    std::vector<modules::VideoFrame>& frames) {
  syncMediaSources(bus, stillFrames, "still");
  auto stills = bus.ingest(mediaPresentationTime100ns, nowNs,
      [](const SourceDescriptor& descriptor) { return descriptor.kind == "still"; });
  frames.insert(frames.end(), std::make_move_iterator(stills.video.begin()),
                std::make_move_iterator(stills.video.end()));
}

// SourceBus owns counters and health; this is their one wire projection. The
// policy callback is shell/core presentation state and defaults to hold.
rpc::Json sourceHealthState(const SourceBus* bus, int64_t nowNs,
    const std::function<SourcePresentationPolicy(const std::string&)>& policyFor) {
  rpc::Json::Array sources;
  if (bus) {
    for (const auto& source : bus->snapshot(nowNs)) {
      const auto policy = policyFor(source.descriptor.sourceId);
      sources.push_back(rpc::Json::Object{
          {"sourceId", source.descriptor.sourceId},
          {"kind", source.descriptor.kind},
          {"width", source.descriptor.width},
          {"height", source.descriptor.height},
          {"framesIngested", static_cast<double>(source.counters.framesIngested)},
          {"droppedFrames", static_cast<double>(source.counters.droppedFrames)},
          {"hasVideo", source.descriptor.hasVideo},
          {"hasAudio", source.descriptor.hasAudio},
          {"audioPacketsIngested", static_cast<double>(source.counters.audioPacketsIngested)},
          {"audioSamplesIngested", static_cast<double>(source.counters.audioSamplesIngested)},
          {"health", sourceHealthName(source.health)},
          {"dropoutPolicy", policy.dropoutPolicy},
          {"displayName", policy.displayName},
      });
    }
  }
  return rpc::Json{sources};
}

} // namespace corevideo::core
