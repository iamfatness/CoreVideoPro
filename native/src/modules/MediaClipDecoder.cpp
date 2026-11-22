#include "modules/MediaClipDecoder.h"

#include "core/BoundedAsyncLog.h"
#include "modules/MediaPlaybackTimeline.h"
#include "modules/StillMediaFrameCache.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

// Each rule below names the Media Foundation line it ports ("MF:" is
// MediaFoundationMediaFrameSourceAdapter.cpp). Where this decoder deliberately
// differs, the comment says so and why.
namespace corevideo::modules {

namespace {

// A loop opens its NEXT reader this long before the end of the current one, so
// the seam costs a reader switch and not an open (an AVFoundation open is tens
// of milliseconds; a seam that waits for it repeats the last frame on air).
constexpr int64_t kLoopPreopenLead100ns = 3'000'000;

std::string frameSourceIdOf(const MediaDecodeRequest& request) {
  return request.sourceId.empty() ? "media:" + request.assetId : request.sourceId;  // MF:177-179
}

int64_t steadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

struct MediaClipDecoder::Impl {
  // Reader callbacks arrive on reader queues and can race teardown. They hold a
  // weak_ptr to this relay, never the decoder. The callback runs UNDER the relay
  // mutex, so clear() returns only after an in-flight wake has finished: once
  // the destructor has cleared the relay, no wake can reach the owner at all.
  // Lock order is relay -> owner; the owner never calls into the decoder while
  // holding the lock its callback takes (MediaTransports attaches the wake with
  // no entry lock held).
  struct WakeRelay {
    std::mutex mutex;
    std::function<void()> callback;
    void fire() {
      std::lock_guard<std::mutex> lock(mutex);
      if (callback) callback();
    }
    void set(std::function<void()> next) {
      std::lock_guard<std::mutex> lock(mutex);
      callback = std::move(next);
    }
  };

  // Everything that belongs to ONE playback identity (path + loop mode).
  struct State {
    std::string identity;
    std::string path;
    bool loop = false;
    MediaPlaybackTimeline clock;
    std::unique_ptr<IClipVideoReader> reader;
    std::unique_ptr<IClipVideoReader> nextReader;  // a loop's pre-opened successor
    bool preopenTried = false;
    bool readerWasPreopened = false;  // the current reader was adopted at a loop seam, untried
    int samplesFromReader = 0;
    // An open that failed is NOT retried per poll: an open is an operator-scale
    // event and is logged once. A new identity (or a new decoder) tries again.
    bool openFailed = false;
    bool openFailedAtLoopSeam = false;
    std::string openError;
    bool ended = false;
    bool wasPlaying = false;
    bool videoPending = false;  // direct-poll path: one decoded sample of lookahead
    bool stillLoaded = false;
    bool stillFailed = false;
    // The file opened and has no video track (an audio bed, N3). Its picture
    // is ONE transparent frame for the identity's whole life.
    bool audioOnly = false;
    VideoFrame lastFrame;       // the most recently decoded frame
    VideoFrame presentedFrame;  // the frame the direct-poll path last presented
    int64_t decodedPts = 0, decodedDuration = 0, loopOffset = 0;
    int64_t duration100ns = -1;
    // Audio: a SECOND reader on the same clock (MF:938-988, 1054-1103, 1223-1290).
    std::unique_ptr<IClipAudioReader> audioReader;
    std::unique_ptr<IClipAudioReader> nextAudioReader;  // a loop's pre-opened successor (start 0)
    bool audioPreopenTried = false;
    int audioSamplesFromReader = 0;
    bool audioAbsent = false;      // the asset has no audio track: silent, never warned
    bool audioOpenFailed = false;  // sticky, like the video open
    bool audioEnded = false;
    int64_t audioLoopOffset = 0;   // media time the finished loop passes covered
    int64_t audioEndPts = 0;       // end of the last sample appended, loop offset included
    MediaAudioWindows audioWindows;
  };

  std::shared_ptr<IClipReaderFactory> readers;
  std::function<int64_t()> nowMs;
  std::shared_ptr<WakeRelay> relay = std::make_shared<WakeRelay>();
  // Video and audio warnings are rebuilt by their OWN poll and reported
  // together. DELIBERATE DEVIATION FROM MF, which keeps one list and clears it
  // in both polls: the owner polls video, then audio, then reads warnings(), so
  // on MF a playing clip's video warning is erased before anything reads it.
  std::vector<std::string> warnings;       // video
  std::vector<std::string> audioWarnings;
  State state;
  bool reached = false;   // a request has reached the clip (position is -1 before)
  // DELIBERATE DEVIATION FROM MF: ids keep rising across an identity change and
  // across a loop seam (MF resets frameId with `state = {}`). Rising ids can
  // never collide with MediaVideoPresentation's repeat dedup.
  int64_t frameId = 0;
  int readersOpened = 0;

  std::function<void()> wakeForReader() const {
    return [weak = std::weak_ptr<WakeRelay>(relay)] {
      if (const auto strong = weak.lock()) strong->fire();
    };
  }

  // Identity is normalized path + loop flag, NEVER `playing` (MF:733-747): a
  // pause or resume is carried by the same clock, a new identity is a new
  // reader at 0.
  void configure(const MediaDecodeRequest& request, int64_t now100ns) {
    const auto path = normalizeMediaAssetPath(request.assetPath);
    const auto identity = path + (request.loop ? "|loop" : "|once");
    if (state.identity != identity) {
      state = {};  // destroys (cancels and drains) the old readers
      state.identity = identity;
      state.path = path;
      state.loop = request.loop;
    }
    state.clock.configure(identity, request.playing, now100ns);
    reached = true;
  }

  std::unique_ptr<IClipVideoReader> open(std::string& error) {
    error.clear();
    auto reader = readers ? readers->openVideo(state.path, wakeForReader(), error) : nullptr;
    if (reader) readersOpened += 1;
    else if (error.empty()) error = "no media reader is available";
    return reader;
  }

  // One line per failed open, never per poll (MF:1040-1046, docs/reference/media-no-decoder.md).
  void recordOpenFailure(const std::string& error, bool atLoopSeam) {
    state.reader.reset();
    state.openFailed = true;
    state.openFailedAtLoopSeam = atLoopSeam;
    state.openError = error;
    ::corevideo::core::nativeLogf("[media-decoder] %s: NO DECODER - %s\n", state.path.c_str(), error.c_str());
  }

  // Warnings are rebuilt per poll (MF:656, 674); the wording is MF's so the
  // shell and the support bundle read the same text on both platforms.
  void warnOpenFailure(const MediaDecodeRequest& request, bool cue) {
    const auto suffix = state.openError.empty() ? std::string(".") : ": " + state.openError;
    if (state.openFailedAtLoopSeam) {
      warnings.push_back("Media background " + request.assetId + " could not restart its loop" + suffix);  // MF:891-894
    } else if (cue) {
      warnings.push_back("Media asset " + request.assetId + " could not be opened for Preview cueing" + suffix);  // MF:806-809
    } else {
      warnings.push_back("Media asset " + request.assetId + " could not be opened for Program playback" + suffix);  // MF:872-875
    }
  }

  // Takes at most ONE new sample into lastFrame. True only when a new frame was
  // decoded. Never blocks: a reader that is still opening or behind says
  // Pending and this returns false with nothing changed.
  bool takeNewSample(const MediaDecodeRequest& request, const std::string& sourceId, int64_t timestampMs, bool cue) {
    // At most: the loop seam, one replacement of a pre-opened successor that
    // turned out to have failed, then the first sample of the successor.
    for (int hop = 0; hop < 3; ++hop) {
      if (state.audioOnly) return false;  // its one picture is already decoded
      if (state.openFailed) { warnOpenFailure(request, cue); return false; }
      if (!state.reader) {
        std::string error;
        state.reader = open(error);
        state.samplesFromReader = 0;
        state.preopenTried = false;
        if (!state.reader) { recordOpenFailure(error, state.loopOffset > 0); warnOpenFailure(request, cue); return false; }
      }
      ClipVideoSample sample;
      std::string error;
      const auto status = state.reader->take(sample, error);
      if (state.duration100ns <= 0) state.duration100ns = state.reader->duration100ns();
      if (status == ClipReadStatus::Pending) return false;  // a stall is never an end
      if (status == ClipReadStatus::NoVideoTrack) {
        // AN AUDIO BED (N3). Not an open failure: nothing warns and nothing is
        // logged. Its picture is fully transparent, so a layer carrying it
        // draws nothing over the scene (a frameless layer would paint a slate
        // over Program). One frame, one id, held for the identity's life.
        state.reader.reset();
        state.audioOnly = true;
        state.duration100ns = -1;  // a reader with no video track has no say; the audio track decides
        VideoFrame frame;
        frame.participantId = sourceId;
        frame.width = frame.pixelWidth = frame.naturalWidth = 2;
        frame.height = frame.pixelHeight = frame.naturalHeight = 2;
        frame.pixelStride = 8;
        frame.timestampMs = timestampMs;
        frame.frameId = ++frameId;
        frame.pixels = std::make_shared<const std::vector<uint8_t>>(16u, uint8_t{0});
        state.lastFrame = std::move(frame);
        state.decodedPts = 0;
        state.decodedDuration = 0;
        return true;
      }
      if (status == ClipReadStatus::Failed) {
        if (state.samplesFromReader == 0 && state.readerWasPreopened) {
          // The successor was opened ~300 ms before the seam and failed in the
          // meantime (a reader reports a failed open on its first take). One
          // transient failure must not freeze a looping background: open once
          // more, now, and let THAT attempt be the one that is reported.
          state.reader.reset();
          state.readerWasPreopened = false;
          continue;
        }
        if (state.samplesFromReader == 0) {
          // The reader never produced anything: this is the open failing
          // (asynchronously), not a decode error part-way through the file.
          recordOpenFailure(error, state.loopOffset > 0);
          warnOpenFailure(request, cue);
        } else {
          warnings.push_back("Media asset " + request.assetId + " failed while decoding a video frame.");  // MF:1155
        }
        return false;
      }
      if (status == ClipReadStatus::EndOfStream) {
        if (!request.loop) { state.ended = true; return false; }  // MF:902-910
        if (state.samplesFromReader == 0) {
          // A reader with no samples at all cannot loop; reopening it forever
          // would be an open per poll.
          recordOpenFailure("the file has no video frames", true);
          warnOpenFailure(request, cue);
          return false;
        }
        // LOOP SEAM (MF:885-901): the last frame stays on air, the offset moves
        // by exactly the media time the finished pass covered, and the
        // successor (pre-opened when the duration was known) takes over.
        state.loopOffset += state.decodedPts + state.decodedDuration;
        state.reader = std::move(state.nextReader);
        state.readerWasPreopened = state.reader != nullptr;
        state.samplesFromReader = 0;
        state.preopenTried = false;
        if (!state.reader) {
          std::string openError;
          state.reader = open(openError);
          if (!state.reader) { recordOpenFailure(openError, true); warnOpenFailure(request, cue); return false; }
        }
        continue;
      }
      const bool gpuSample = sample.gpu && sample.gpu->width > 0 && sample.gpu->height > 0;
      const bool cpuSample = sample.bgra && sample.width > 0 && sample.height > 0 &&
          sample.bgra->size() >= static_cast<size_t>(sample.width) * static_cast<size_t>(sample.height) * 4u;
      if (!gpuSample && !cpuSample) {
        return false;  // consumed and dropped; the clip holds its last frame
      }
      VideoFrame frame;
      frame.participantId = sourceId;
      frame.timestampMs = timestampMs;
      frame.frameId = ++frameId;
      if (cpuSample) {
        frame.width = frame.pixelWidth = frame.naturalWidth = sample.width;
        frame.height = frame.pixelHeight = frame.naturalHeight = sample.height;
        frame.pixelStride = sample.width * 4;
        frame.pixels = std::move(sample.bgra);
      } else {
        // ZERO-COPY (N4): the reader's native image, untouched. No CPU
        // dimensions are claimed (pixelWidth stays 0), so nothing mistakes it
        // for a CPU payload.
        frame.width = frame.naturalWidth = sample.gpu->width;
        frame.height = frame.naturalHeight = sample.gpu->height;
        frame.gpuPixels = std::move(sample.gpu);
      }
      state.lastFrame = std::move(frame);
      state.decodedPts = sample.pts100ns;
      state.decodedDuration = sample.duration100ns;
      state.samplesFromReader += 1;
      state.readerWasPreopened = false;
      if (request.loop && !state.nextReader && !state.preopenTried && state.duration100ns > 0 &&
          sample.pts100ns + sample.duration100ns >= state.duration100ns - kLoopPreopenLead100ns) {
        // Pre-open (new; not in MF). A failure here is not reported: the seam
        // opens again (see readerWasPreopened) and reports that attempt.
        state.preopenTried = true;
        std::string ignored;
        state.nextReader = open(ignored);
      }
      return true;
    }
    return false;
  }

  // False = no audio reader (no track, or the open failed; the flags say which).
  bool openAudioReader(int64_t start100ns, bool firstOpen) {
    std::string error;
    state.audioReader = readers ? readers->openAudio(state.path, start100ns, wakeForReader(), error) : nullptr;
    state.audioSamplesFromReader = 0;
    state.audioPreopenTried = false;
    if (state.audioReader) return true;
    // nullptr with an EMPTY error = no audio track: not a failure (MF:1080-1086).
    if (error.empty()) { if (firstOpen) state.audioAbsent = true; else state.audioEnded = true; }
    else state.audioOpenFailed = true;
    return false;
  }

  // One 960-frame stereo window cut at the CLOCK position, or nothing. Paused
  // emits nothing, not even silence (MF:673-681): the caller returned before
  // this. Chunks are appended by media PTS, so a window is exact whatever the
  // reader's chunk size, and the 200 ms history lets a resume replay samples
  // decoded ahead before the pause (MediaPlaybackTimeline.h).
  bool decodeAudio(const MediaDecodeRequest& request, int64_t timestampMs, AudioFrame& out) {
    constexpr int kWindowFrames = 960;
    constexpr int64_t kRate = 48000;
    configure(request, timestampMs * 10000);
    if (state.audioAbsent) return false;
    const auto warnOpen = [&] {
      audioWarnings.push_back("Media asset " + request.assetId + " could not be opened for Program audio.");
    };
    if (state.audioOpenFailed) { warnOpen(); return false; }
    const auto elapsed = state.clock.elapsed100ns(timestampMs * 10000);
    bool firstOpen = !state.audioReader && !state.audioEnded && state.audioLoopOffset == 0 && state.audioEndPts == 0;
    state.audioWindows.seek(elapsed * kRate / 10'000'000);  // MF:972-977
    for (int chunks = 0; chunks < 8 && !state.audioEnded &&
         state.audioWindows.bufferedThrough() < state.audioWindows.cursor() + kWindowFrames; ++chunks) {
      if (!state.audioReader) {
        // First playing poll, or a loop seam with nothing pre-opened: start at
        // the clock position inside the current loop pass (MF:1093-1101).
        if (!openAudioReader(mediaLoopSeek100ns(elapsed, state.audioLoopOffset), firstOpen)) {
          if (state.audioOpenFailed) warnOpen();
          break;
        }
        firstOpen = false;
      }
      ClipAudioSample sample;
      std::string error;
      const auto status = state.audioReader->take(sample, error);
      // An audio bed has no video reader to ask: its duration is the audio track's.
      if (state.audioOnly && state.duration100ns <= 0) state.duration100ns = state.audioReader->duration100ns();
      if (status == ClipReadStatus::Pending) break;
      if (status == ClipReadStatus::Failed) {
        if (state.audioSamplesFromReader == 0) {
          state.audioReader.reset();
          state.audioOpenFailed = true;
          warnOpen();
        } else {
          audioWarnings.push_back("Media asset " + request.assetId + " failed while decoding Program audio.");
        }
        break;
      }
      if (status == ClipReadStatus::EndOfStream) {
        if (state.audioSamplesFromReader == 0 && state.audioLoopOffset == 0 && state.audioEndPts == 0) {
          // The first reader ended without one sample: the asset has no audio
          // track (an asynchronous reader can only say so this way).
          state.audioReader.reset();
          state.audioAbsent = true;
          break;
        }
        if (!request.loop || state.audioSamplesFromReader == 0) { state.audioEnded = true; break; }
        // LOOP SEAM (MF:953-958): time keeps running forward by the media the
        // finished pass covered; the pre-opened successor takes over in the same
        // poll, so the window that straddles the seam is filled from both sides.
        state.audioLoopOffset = state.audioEndPts;
        state.audioReader = std::move(state.nextAudioReader);
        state.audioSamplesFromReader = 0;
        state.audioPreopenTried = false;
        continue;
      }
      const auto frames = static_cast<int64_t>(sample.pcm.size() / 2);
      if (frames <= 0) continue;
      const auto pts = sample.pts100ns + state.audioLoopOffset;
      const auto duration = frames * 10'000'000 / kRate;
      state.audioWindows.append(pts, std::move(sample.pcm));
      state.audioEndPts = pts + duration;
      state.audioSamplesFromReader += 1;
      if (request.loop && !state.nextAudioReader && !state.audioPreopenTried && state.duration100ns > 0 &&
          sample.pts100ns + duration >= state.duration100ns - kLoopPreopenLead100ns) {
        // Pre-open from the top (new; not in MF). The duration is the video
        // reader's; an asset with no video has none and reopens at the seam.
        state.audioPreopenTried = true;
        std::string ignored;
        state.nextAudioReader = readers ? readers->openAudio(state.path, 0, wakeForReader(), ignored) : nullptr;
      }
    }
    // Emit only a FULL window, or the tail when the reader is at its end (MF:978-986).
    if (state.audioWindows.bufferedThrough() <= state.audioWindows.cursor() ||
        (!state.audioEnded && state.audioWindows.bufferedThrough() < state.audioWindows.cursor() + kWindowFrames)) {
      return false;
    }
    out.participantId = frameSourceIdOf(request);
    out.sampleRate = static_cast<int>(kRate);
    out.channels = 2;
    out.sampleCount = kWindowFrames;
    out.timestampMs = timestampMs;
    out.voiceActive = true;
    out.pcm = state.audioWindows.take(kWindowFrames);
    return true;
  }

  bool decode(const MediaDecodeRequest& request, int64_t now100ns, bool prefetching, VideoFrame& out) {
    warnings.clear();
    if (request.assetId.empty() || request.assetPath.empty()) return false;
    // Route stills are served by MediaCore's StillMediaFrameCache; decoding them
    // here would repeat that work (MF:660-663). Nothing is opened for them.
    if (request.kind == "media-video" && isStillImageMediaAsset(request.assetKind, request.assetPath)) return false;
    const auto sourceId = frameSourceIdOf(request);
    const auto timestampMs = now100ns / 10000;
    configure(request, now100ns);
    const auto deliver = [&](const VideoFrame& frame) {
      if (!frame.hasContent()) return false;
      out = frame;
      out.participantId = sourceId;
      out.timestampMs = timestampMs;
      return true;
    };
    if (isStillImagePath(state.path)) {
      // A background still: decode once and hold it forever (MF:776-789).
      if (!state.stillLoaded && !state.stillFailed) {
        VideoFrame still;
        std::string error;
        if (readers && readers->decodeStill(state.path, still, error) && still.hasContent()) {
          still.frameId = ++frameId;
          state.lastFrame = std::move(still);
          state.stillLoaded = true;
        } else {
          state.stillFailed = true;
        }
      }
      if (!state.stillLoaded) {
        warnings.push_back("Media asset " + request.assetId + " could not be decoded as an image.");
        return false;
      }
      return deliver(state.lastFrame);
    }
    if (!request.playing) {
      // PAUSED AFTER IT ROLLED: hold the frame on air and read nothing; the
      // reader stays where it is, so Play continues with the next sample
      // (MF:790-799).
      const VideoFrame& held = state.presentedFrame.hasContent() ? state.presentedFrame : state.lastFrame;
      if (state.wasPlaying && held.hasContent()) return deliver(held);
      // NEVER PLAYED (a cue poster): exactly one sample, then held (MF:800-819).
      if (!state.lastFrame.hasContent()) (void)takeNewSample(request, sourceId, timestampMs, /*cue=*/true);
      return deliver(state.lastFrame);
    }
    // PLAYING. A cue that starts to roll continues from the reader's next
    // sample, no reopen (MF:820-824). On the direct path the poster is the
    // presented frame until the next sample comes due.
    if (!state.wasPlaying && state.lastFrame.hasContent() && !state.presentedFrame.hasContent()) {
      state.presentedFrame = state.lastFrame;
    }
    state.wasPlaying = true;
    if (state.ended) return prefetching ? false : deliver(state.lastFrame);
    if (prefetching) {
      // At most one NEW sample per call (MF:639-652).
      if (!takeNewSample(request, sourceId, timestampMs, /*cue=*/false)) return false;
      return deliver(state.lastFrame);
    }
    // Direct poll: present by media time, one sample of lookahead (MF:925-935).
    for (int decoded = 0; decoded < 4; ++decoded) {
      if (!state.videoPending) {
        if (!takeNewSample(request, sourceId, timestampMs, /*cue=*/false)) break;
        state.videoPending = true;
      }
      if (!state.clock.videoDue(state.loopOffset + state.decodedPts, now100ns)) break;
      state.presentedFrame = state.lastFrame;
      state.videoPending = false;
    }
    return deliver(state.presentedFrame);
  }
};

MediaClipDecoder::MediaClipDecoder(std::shared_ptr<IClipReaderFactory> readers, std::function<int64_t()> nowMs)
    : impl_(std::make_unique<Impl>()) {
  impl_->readers = std::move(readers);
  impl_->nowMs = nowMs ? std::move(nowMs) : std::function<int64_t()>(steadyNowMs);
}

MediaClipDecoder::~MediaClipDecoder() {
  // Clear the relay BEFORE the readers go: a reader callback racing teardown
  // then reaches an empty relay and calls nothing.
  impl_->relay->set({});
  impl_->state = {};
}

void MediaClipDecoder::setMediaWakeCallback(std::function<void()> callback) {
  impl_->relay->set(std::move(callback));
}

void MediaClipDecoder::syncMediaClock(const MediaDecodeRequest& request, int64_t nowMs) {
  if (request.assetId.empty() || request.assetPath.empty()) return;
  if (request.kind == "media-video" && isStillImageMediaAsset(request.assetKind, request.assetPath)) return;
  impl_->configure(request, nowMs * 10000);  // MF:632-637
}

std::vector<VideoFrame> MediaClipDecoder::pollMediaFrames(const MediaDecodeRequest& request, int64_t timestampMs) {
  return pollMediaFramesAt100ns(request, timestampMs * 10000);
}

std::vector<VideoFrame> MediaClipDecoder::pollMediaFramesAt100ns(const MediaDecodeRequest& request,
                                                                 int64_t timestamp100ns) {
  VideoFrame frame;
  if (impl_->decode(request, timestamp100ns, /*prefetching=*/false, frame)) return {std::move(frame)};
  return {};
}

std::vector<ScheduledMediaVideo> MediaClipDecoder::prefetchMediaVideo(const MediaDecodeRequest& request,
                                                                      int64_t nowMs) {
  VideoFrame frame;
  if (!impl_->decode(request, nowMs * 10000, /*prefetching=*/true, frame)) return {};
  const auto& state = impl_->state;
  // A rolling clip's sample is due at epoch + loop offset + its media time
  // (MF:639-652). A poster, a held frame and a still are due now.
  const bool scheduled = request.playing && !state.stillLoaded;
  const auto due = scheduled ? state.clock.epoch100ns() + state.loopOffset + state.decodedPts : nowMs * 10000;
  std::vector<ScheduledMediaVideo> result;
  result.push_back({std::move(frame), due});
  return result;
}

std::vector<AudioFrame> MediaClipDecoder::pollMediaAudioFrames(const MediaDecodeRequest& request, int64_t timestampMs) {
  impl_->audioWarnings.clear();
  if (request.kind != "media-video" || request.assetId.empty() || request.assetPath.empty()) return {};
  // A paused clip emits no PCM. Returning BEFORE the clock is touched also keeps
  // an audio-slot timestamp (which runs ahead of now) from ever becoming the
  // instant the clock froze; the video path / syncMediaClock owns that (MF:938-946).
  if (!request.playing || isStillImagePath(normalizeMediaAssetPath(request.assetPath))) return {};
  AudioFrame frame;
  if (impl_->decodeAudio(request, timestampMs, frame)) return {std::move(frame)};
  return {};
}

std::vector<std::string> MediaClipDecoder::warnings() const {
  auto all = impl_->warnings;
  all.insert(all.end(), impl_->audioWarnings.begin(), impl_->audioWarnings.end());
  return all;
}

int64_t MediaClipDecoder::playbackPositionMs() const {
  if (!impl_->reached) return -1;
  const auto& state = impl_->state;
  // The clock is wall time; the position is a place in the media. Wrap it for a
  // loop and clamp it for a clip when the duration is known (MF:599-616).
  auto elapsed = (std::max)(int64_t{0}, state.clock.elapsed100ns(impl_->nowMs() * 10000));
  const auto duration = state.reader && state.reader->duration100ns() > 0 ? state.reader->duration100ns()
                                                                         : state.duration100ns;
  if (duration > 0) elapsed = state.loop ? elapsed % duration : (std::min)(elapsed, duration);
  return elapsed / 10000;
}

int64_t MediaClipDecoder::mediaDurationMs() const {
  const auto& state = impl_->state;
  const auto duration = state.reader && state.reader->duration100ns() > 0 ? state.reader->duration100ns()
                                                                         : state.duration100ns;
  return duration > 0 ? duration / 10000 : -1;  // MF:617-621
}

// A loop never ends (MF:626-630). An audio bed ends when its audio reader has
// run out: its one picture never ends. (decodeAudio sees the reader's end only
// while it is short of a full window, and emits that tail in the same poll, so
// nothing is left buffered once this turns true; the OWNER's #1214 drain rule
// then waits for the delivered windows to play.)
bool MediaClipDecoder::mediaEnded() const {
  const auto& state = impl_->state;
  if (state.loop) return false;
  if (state.audioOnly) return state.audioEnded || state.audioAbsent;
  return state.ended;
}

bool MediaClipDecoder::mediaHasVideo() const { return !impl_->state.audioOnly; }

int MediaClipDecoder::videoReadersOpenedForTest() const { return impl_->readersOpened; }

}  // namespace corevideo::modules
