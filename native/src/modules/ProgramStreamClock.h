#pragma once

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "modules/ProgramAacEncoder.h"

namespace corevideo::modules {

// The one Program stream clock (#538 Slice 8).
//
// Video time is the compositor frame number: the hardware encoder stamps frame
// N at N * 10'000'000 / fps (MediaFoundationGpuVideoEncoder::processInput).
// AAC time is the shared encoder's accepted-sample count at 48 kHz. Both
// counters are exact; what they lacked was a common origin. Before this slice
// each FFmpeg process started its two inputs at zero and aligned them by which
// packet happened to reach the pipe first (plus a three-packet startup trim).
//
// This clock fixes the relation once per stream session, in the core, the same
// way the recording aligns its tracks: the Program frame carries its steady-
// clock timeline time (ProgramFrame::timelineTimestamp100ns) and the audio
// block carries the audio worker's (AudioOutputWorkItem::outputTimestamp100ns).
// At the session's first PCM block the latest frame N (timeline T_v) and the
// block (first sample S, timeline T_a) give the anchor
//   programTime(S) = N / fps + (T_a - T_v)
// and every later AAC packet is stamped
//   N / fps + (T_a - T_v) + (sampleIndex - S) / 48000
// so audio advances by samples only. (A frame-number-only anchor quantized
// this to a whole frame per session; measured 3-19 ms RTMP-record spread.)
// Mute and dropout already feed exact silent samples, so they continue the
// same count. A destination reconnect or a fresh IDR does not touch this clock;
// only the end of the whole stream session (no RTMP/SRT/HLS destination
// requested) does, because the AAC sample counter then stops while compositor
// frames keep advancing.
//
// Threading: observeProgramFrame runs on the video tick; beginAudioBlock and
// stamp run on the audio worker. The latest-frame pair is read under a mutex
// that is only ever held for a copy.
class ProgramStreamClock {
 public:
  // Video tick. `frameNumber` is the Program frame handed to the encoder this
  // tick and `timeline100ns` its steady-clock timeline time (0 if unknown);
  // `sessionActive` is whether any shared-stream destination is requested.
  void observeProgramFrame(int64_t frameNumber, int64_t timeline100ns, bool sessionActive) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sessionActive) {
      if (active_) ++generation_;
      active_ = false;
      return;
    }
    active_ = true;
    if (frameNumber > 0) {
      latestFrame_ = frameNumber;
      latestTimeline100ns_ = timeline100ns;
    }
  }

  // Audio worker, before encoding a PCM block whose first sample is
  // `firstSample` in the AAC encoder's accepted-sample count and whose
  // steady-clock timeline time is `timeline100ns` (0 if unknown). The first
  // block of a session (after any video frame is known) fixes the anchor.
  void beginAudioBlock(int64_t firstSample, int64_t timeline100ns) {
    int64_t frame = 0, frameTimeline = 0;
    uint64_t generation = 0;
    bool active = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      frame = latestFrame_;
      frameTimeline = latestTimeline100ns_;
      generation = generation_;
      active = active_;
    }
    if (anchored_ && anchorGeneration_ == generation) return;
    if (frame <= 0 || !active) {
      anchored_ = false;
      return;
    }
    anchored_ = true;
    anchorGeneration_ = generation;
    anchorFrame_ = frame;
    anchorSample_ = firstSample;
    anchorOffset100ns_ = timeline100ns > 0 && frameTimeline > 0 ? timeline100ns - frameTimeline : 0;
    anchorLogPending_ = true;
    anchorFrameTimeline100ns_ = frameTimeline;
    anchorAudioTimeline100ns_ = timeline100ns;
  }

  // Audio worker: the anchor taken by the last beginAudioBlock, once, for the log.
  struct AnchorEvidence { int64_t frame, sample, offset100ns, frameTimeline100ns, audioTimeline100ns; };
  bool takeAnchorEvidence(AnchorEvidence& evidence) {
    if (!anchorLogPending_) return false;
    anchorLogPending_ = false;
    evidence = {anchorFrame_, anchorSample_, anchorOffset100ns_, anchorFrameTimeline100ns_, anchorAudioTimeline100ns_};
    return true;
  }

  // Audio worker. Leaves the packet unanchored (anchorFrameNumber < 0) until a
  // session anchor exists; muxers drop such packets rather than invent time.
  void stamp(ProgramAacPacket& packet) const {
    packet.anchorFrameNumber = anchored_ ? anchorFrame_ : -1;
    packet.anchorSampleIndex = anchorSample_;
    packet.anchorOffset100ns = anchorOffset100ns_;
    packet.anchorFrameTimeline100ns = anchorFrameTimeline100ns_;
  }

  [[nodiscard]] bool anchored() const { return anchored_; }

 private:
  std::mutex mutex_;
  int64_t latestFrame_ = 0;
  int64_t latestTimeline100ns_ = 0;
  bool active_ = false;
  uint64_t generation_ = 0;
  // Audio worker only.
  bool anchored_ = false;
  uint64_t anchorGeneration_ = 0;
  int64_t anchorFrame_ = 0;
  int64_t anchorSample_ = 0;
  int64_t anchorOffset100ns_ = 0;
  bool anchorLogPending_ = false;
  int64_t anchorFrameTimeline100ns_ = 0;
  int64_t anchorAudioTimeline100ns_ = 0;
};

// Frame numbers count RENDERED frames; the program buffer's delivery grid
// counts SLOTS. A render stall (a take opening media, a GPU hiccup) leaves slots
// with no new frame, so every later frame is delivered N slots later than its
// number says: K = timeline - frame/fps steps up by N slot durations. Measured
// 2026-09-30: a 6-slot startup stall shifted a stream's video 100 ms against its
// sample-counted audio for the rest of the session; the recording (stamped by
// delivery time) was unaffected. Muxers add the slots K moved since the audio
// anchor to the encoder's frame-number PTS, so a stall becomes a PTS gap in the
// video instead of a permanent A/V offset.
//
// Observe only BUFFERED frames: their timeline is the exact delivery deadline.
// An unbuffered frame's timeline is tick arrival and carries jitter, not slots.
class ProgramSlotClock {
 public:
  void observe(int64_t frame, int64_t timeline100ns, int fps) {
    if (frame <= 0 || timeline100ns <= 0 || fps <= 0) return;
    const int64_t k = timeline100ns - frame * 10'000'000LL / fps;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!steps_.empty()) {
      if (frame <= steps_.back().first) return;
      if (std::llabs(k - steps_.back().second) * 2 * fps < 10'000'000LL) return;  // same slot
    }
    steps_.push_back({frame, k});
    while (steps_.size() > 64) steps_.pop_front();
  }

  // K in effect for `frame`: the latest step at or before it (the newest known
  // step when `frame` has not been delivered yet). nullopt before any frame.
  [[nodiscard]] std::optional<int64_t> kFor(int64_t frame) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (steps_.empty()) return std::nullopt;
    for (auto it = steps_.rbegin(); it != steps_.rend(); ++it)
      if (it->first <= frame) return it->second;
    return steps_.front().second;
  }

  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    steps_.clear();
  }

 private:
  mutable std::mutex mutex_;
  std::deque<std::pair<int64_t, int64_t>> steps_;  // (first frame, K)
};

// Whole slots the delivery grid has moved for a frame whose K is `k100ns`,
// relative to the K the audio anchored on (`anchorK100ns`).
[[nodiscard]] inline int64_t slotsSinceAnchor(int64_t k100ns, int64_t anchorK100ns, int fps) {
  if (fps <= 0) return 0;
  const int64_t delta = (k100ns - anchorK100ns) * fps;
  return (delta >= 0 ? delta + 5'000'000LL : delta - 5'000'000LL) / 10'000'000LL;
}

// The anchor's own K, from a stamped AAC packet (nullopt if unanchored).
[[nodiscard]] inline std::optional<int64_t> anchorK100ns(const ProgramAacPacket& packet, int fps) {
  if (packet.anchorFrameNumber < 0 || packet.anchorFrameTimeline100ns <= 0 || fps <= 0) return std::nullopt;
  return packet.anchorFrameTimeline100ns - packet.anchorFrameNumber * 10'000'000LL / fps;
}

// Program-clock presentation time of an anchored AAC packet, in the same
// 100 ns units and frame-number origin as the hardware video encoder's PTS.
// Returns false for an unanchored packet.
[[nodiscard]] inline bool programAudioPts100ns(const ProgramAacPacket& packet, int fps, int64_t& pts100ns) {
  if (packet.anchorFrameNumber < 0 || fps <= 0) return false;
  pts100ns = packet.anchorFrameNumber * 10'000'000LL / fps + packet.anchorOffset100ns +
             (packet.sampleIndex - packet.anchorSampleIndex) * 10'000'000LL / 48'000;
  return true;
}

}  // namespace corevideo::modules
