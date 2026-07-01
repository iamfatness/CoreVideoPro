#pragma once

#include "modules/Interfaces.h"
#include "rpc/Json.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace corevideo::modules {

// Real Zoom capture source backed by decoded participant frames.
//
// The Zoom engine decodes participant video into BGRA frames inside
// ZoomEngineRuntime. Those frames are ingested here, keyed by participantId,
// and surfaced through pollVideoFrames() carrying their real pixels (the
// SyntheticZoomCaptureSource only ever returned metadata). When no participant
// has a fresh frame, pollVideoFrames() falls back to the wrapped synthetic
// source so there is no regression when there is no meeting or no video.
class RealZoomCaptureSource final : public IZoomCaptureSource {
 public:
  explicit RealZoomCaptureSource(std::unique_ptr<IZoomCaptureSource> fallback = nullptr);

  // Ingest a single decoded BGRA frame for a participant. The latest frame per
  // participant is retained; older frames are overwritten. `bgra` is copied so
  // the caller retains ownership of its buffer.
  void ingestFrame(const std::string& participantId,
                   const uint8_t* bgra,
                   int width,
                   int height,
                   int64_t frameId,
                   int64_t timestampMs);

  // Ingest a single full-resolution I420 (YUV 4:2:0 planar) frame for a
  // participant. `i420` is Y (width*height) + U + V ((width/2)*(height/2) each),
  // tightly packed, and is copied so the caller retains ownership. The latest
  // frame per participant wins (replacing any prior BGRA or I420 frame). The
  // GPU compositor converts these planes to RGB in-shader, so the CPU never pays
  // the per-pixel I420->BGRA convert at full resolution.
  void ingestI420Frame(const std::string& participantId,
                       const uint8_t* i420,
                       int width,
                       int height,
                       int64_t frameId,
                       int64_t timestampMs);

  // Zero-copy overload: take shared ownership of an already-decoded I420 buffer
  // (e.g. the shared_ptr handed back by ZoomEngineRuntime::latestDecodedVideoFrames)
  // instead of copying its bytes. On the render thread this removes a full ~3MB/1080p
  // per-participant memcpy every tick (the dominant renderDisplayTick cost); the buffer
  // is const + immutable (a new one is allocated per decoded frame), so sharing is safe.
  void ingestI420Frame(const std::string& participantId,
                       std::shared_ptr<const std::vector<uint8_t>> i420,
                       int width,
                       int height,
                       int64_t frameId,
                       int64_t timestampMs);

  // Ingest a batch of zoom-video-frame events as produced by
  // ZoomEngineRuntime::drainFrameEvents(). Each event carries a base64-encoded
  // BGRA buffer plus participant/width/height/frameId metadata. Events that are
  // not video frames or that are malformed are ignored.
  void ingestFrameEvents(const std::vector<rpc::Json>& events);

  // Returns one VideoFrame per participant that has a stored frame, each
  // carrying its real BGRA pixels. Falls back to the wrapped synthetic source
  // when no real frames are available.
  std::vector<VideoFrame> pollVideoFrames() override;

  std::vector<AudioFrame> pollAudioFrames() override;

  // Number of participants with a stored frame (test/diagnostic helper).
  [[nodiscard]] size_t participantCount() const;

 private:
  struct StoredFrame {
    // BGRA payload (capture devices / tests). Mutually exclusive with `i420`:
    // whichever representation was ingested last is the one that is set.
    std::shared_ptr<const std::vector<uint8_t>> pixels;
    // I420 payload (Zoom participants — converted to RGB on the GPU).
    std::shared_ptr<const std::vector<uint8_t>> i420;
    int width = 0;
    int height = 0;
    int stride = 0;
    int64_t frameId = 0;
    int64_t timestampMs = 0;
  };

  mutable std::mutex mutex_;
  std::map<std::string, StoredFrame> frames_;
  std::unique_ptr<IZoomCaptureSource> fallback_;
};

}  // namespace corevideo::modules
