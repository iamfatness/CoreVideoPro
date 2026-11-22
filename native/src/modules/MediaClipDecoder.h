#pragma once
#include "modules/Interfaces.h"
#include "modules/MediaVideoPresentation.h"
#include <functional>
#include <memory>
#include <string>

// A PORTABLE media clip decoder: the Media Foundation adapter's IMediaDecoder +
// IMediaVideoPrefetch state machine (cue poster, pause hold, resume without a
// reopen, loop offset, end, frame ids, position) with every platform call moved
// behind a small NON-BLOCKING reader seam. The decisions are about samples, not
// about a decode API, so they are tested with scripted readers on every
// platform; a platform supplies only "open, decode, copy, hand over, cancel".
// The Apple side is AVFoundationMediaDecoderAdapter.mm. Windows is NOT wired to
// this: it keeps MediaFoundationMediaFrameSourceAdapter.cpp.
namespace corevideo::modules {

enum class ClipReadStatus { Ready, Pending, EndOfStream, Failed, NoVideoTrack };

struct ClipVideoSample {
  std::shared_ptr<const std::vector<uint8_t>> bgra;  // tightly packed, stride width*4, STRAIGHT alpha
  // OR a native GPU image and no CPU bytes (the Mac zero-copy reader, N4). The
  // decoder moves it into VideoFrame::gpuPixels untouched; nothing copies it.
  std::shared_ptr<const GpuVideoFrame> gpu;
  int width = 0, height = 0;
  int64_t pts100ns = 0;        // media time within THIS reader, 0 at the start of the file
  int64_t duration100ns = 0;
};

struct ClipAudioSample {       // used from the audio slice (N2)
  std::vector<float> pcm;      // interleaved float32, 48000 Hz, 2 channels
  int64_t pts100ns = 0;        // media time within THIS reader, offset by the reader's start
};

// A reader NEVER blocks. take() hands over at most one decoded sample.
//  Ready       -> `out` filled, sample consumed.
//  Pending     -> nothing yet (opening, or the reader queue is behind).
//  EndOfStream -> sticky.
//  Failed      -> sticky; `error` is an operator-facing cause.
//  NoVideoTrack-> sticky, video readers only: the file opened and has no video
//                 track (an audio bed). Not a failure; nothing warns.
// Destroying a reader cancels its work and returns only after its queue is
// drained: no callback may run after the destructor returns.
class IClipVideoReader {
 public:
  virtual ~IClipVideoReader() = default;
  virtual ClipReadStatus take(ClipVideoSample& out, std::string& error) = 0;
  [[nodiscard]] virtual int64_t duration100ns() const = 0;  // -1 until the container says
};
class IClipAudioReader {
 public:
  virtual ~IClipAudioReader() = default;
  virtual ClipReadStatus take(ClipAudioSample& out, std::string& error) = 0;
  // The asset's duration as the audio reader knows it; -1 until known. Read
  // only for an audio-only asset, which has no video reader to ask.
  [[nodiscard]] virtual int64_t duration100ns() const { return -1; }
};

class IClipReaderFactory {
 public:
  virtual ~IClipReaderFactory() = default;
  // Returns at once; opening runs off the caller's thread. `wake` is called with
  // NO reader lock held whenever take() may return something new. nullptr with a
  // non-empty `error` = the file cannot be opened at all (missing, unsupported
  // container); the error is worded for the operator.
  virtual std::unique_ptr<IClipVideoReader> openVideo(const std::string& path,
      std::function<void()> wake, std::string& error) = 0;
  // nullptr with EMPTY error = the asset has no audio track (not a failure).
  virtual std::unique_ptr<IClipAudioReader> openAudio(const std::string& path, int64_t start100ns,
      std::function<void()> wake, std::string& error) {
    (void)path; (void)start100ns; (void)wake; (void)error;
    return nullptr;
  }
  // Background stills (kind "media-background" on a still path): decode once,
  // straight-alpha BGRA into frame.pixels.
  virtual bool decodeStill(const std::string& path, VideoFrame& frame, std::string& error) = 0;
};

class MediaClipDecoder final : public IMediaDecoder, public IMediaVideoPrefetch {
 public:
  // `nowMs` is the steady clock in milliseconds; tests inject one. It is used
  // ONLY by playbackPositionMs(); every poll takes its time as an argument.
  explicit MediaClipDecoder(std::shared_ptr<IClipReaderFactory> readers,
                            std::function<int64_t()> nowMs = {});
  ~MediaClipDecoder() override;
  std::vector<VideoFrame> pollMediaFrames(const MediaDecodeRequest& request, int64_t timestampMs) override;
  std::vector<VideoFrame> pollMediaFramesAt100ns(const MediaDecodeRequest& request, int64_t timestamp100ns) override;
  std::vector<AudioFrame> pollMediaAudioFrames(const MediaDecodeRequest& request, int64_t timestampMs) override;
  [[nodiscard]] std::vector<std::string> warnings() const override;
  void setMediaWakeCallback(std::function<void()> callback) override;
  std::vector<ScheduledMediaVideo> prefetchMediaVideo(const MediaDecodeRequest& request, int64_t nowMs) override;
  void syncMediaClock(const MediaDecodeRequest& request, int64_t nowMs) override;
  [[nodiscard]] int64_t playbackPositionMs() const override;
  [[nodiscard]] int64_t mediaDurationMs() const override;
  [[nodiscard]] bool mediaEnded() const override;
  [[nodiscard]] bool mediaHasVideo() const override;
  // Test-only observation, never read by production code.
  [[nodiscard]] int videoReadersOpenedForTest() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace corevideo::modules
