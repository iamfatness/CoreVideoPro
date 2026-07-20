#pragma once

#include <cstdint>
#include <optional>

namespace corevideo::modules {

// Shared-epoch PTS clock for a recording session (audio overhaul spec R4/4.3).
//
// The prior model gave each track its own feed-driven counter: video PTS
// advanced by 1/fps per SUBMITTED frame — but the audio worker submits the
// latest program frame every ~20ms tick, duplicates included, so ~50 muxed
// frames/s were tagged 1/60s apart and the video timeline advanced at ~0.83x
// real time — while audio PTS advanced by real samples fed (~1.0x real time).
// The two never shared a clock, so recordings drifted A/V apart monotonically
// for their whole length.
//
// This clock anchors BOTH tracks to ONE epoch — the first media submitted
// after the writers open:
//  - video PTS = elapsed wall time at submit; duplicate program frames are
//    rejected by frameNumber so each composed frame is muxed exactly once, at
//    the moment it is fresh (variable-frame-rate MP4, correct wall timing);
//  - audio PTS = the epoch offset of the FIRST audio buffer plus the
//    accumulated sample count (the sample counter stays the intra-track clock
//    so the AAC stream is gapless; the base offset aligns its start with the
//    video timeline instead of both tracks pretending to start at 0).
//
// Pure logic — the caller supplies `now` in 100ns ticks from any steady
// origin — so the drift behavior is unit-testable without Media Foundation.
class RecordingPtsClock {
 public:
  void reset() { *this = RecordingPtsClock{}; }

  // PTS for a program frame, or nullopt when this frameNumber was already
  // muxed (the audio worker re-submits the latest frame every tick).
  std::optional<std::int64_t> videoPts(std::int64_t now100ns, std::int64_t frameNumber) {
    if (hasLastFrameNumber_ && frameNumber == lastFrameNumber_) {
      return std::nullopt;
    }
    ensureEpoch(now100ns);
    hasLastFrameNumber_ = true;
    lastFrameNumber_ = frameNumber;
    std::int64_t pts = now100ns - epoch100ns_;
    // Media Foundation requires strictly monotonic sample times per stream.
    if (hasLastVideoPts_ && pts <= lastVideoPts_) {
      pts = lastVideoPts_ + 1;
    }
    hasLastVideoPts_ = true;
    lastVideoPts_ = pts;
    return pts;
  }

  // A3 (VST latency compensation): audio that passed through a
  // latency-reporting plugin is CONTENT-late — the samples in the buffer
  // stamped "now" actually happened `latencySamples` earlier. The offset is
  // LATCHED at the first audio buffer: changing it mid-session would jump the
  // audio timeline against the gapless AAC sample clock (an audible A/V
  // glitch), so a mid-recording plugin swap keeps the latched offset and the
  // next recording picks up the new value. Clamped so the first buffer's PTS
  // never goes below the shared epoch (MF rejects negative sample times).
  void setAudioContentLatency(int latencySamples) {
    if (!audioStarted_) {
      audioLatencySamples_ = latencySamples > 0 ? latencySamples : 0;
    }
  }

  [[nodiscard]] int audioContentLatencySamples() const { return audioLatencySamples_; }

  // PTS for an audio buffer of `frameCount` sample-frames; advances the
  // sample clock. Multiplying the TOTAL sample count (instead of accumulating
  // per-packet durations) keeps the track free of cumulative rounding drift.
  std::int64_t audioPts(std::int64_t now100ns, int frameCount, int sampleRate) {
    ensureEpoch(now100ns);
    const std::int64_t rate = sampleRate > 0 ? sampleRate : 48000;
    if (!audioStarted_) {
      audioStarted_ = true;
      audioBase100ns_ = now100ns - epoch100ns_;
      // Latch the content-latency offset (see setAudioContentLatency): the
      // whole audio track shifts EARLIER by the plugin latency, clamped to
      // the epoch so the first PTS stays >= 0.
      const std::int64_t latency100ns = audioLatencySamples_ * 10'000'000LL / rate;
      audioLatencyOffset100ns_ = latency100ns < audioBase100ns_ ? latency100ns : audioBase100ns_;
    }
    const std::int64_t pts =
        audioBase100ns_ - audioLatencyOffset100ns_ + audioSamples_ * 10'000'000LL / rate;
    audioSamples_ += frameCount > 0 ? frameCount : 0;
    return pts;
  }

  [[nodiscard]] std::int64_t lastVideoPts100ns() const { return hasLastVideoPts_ ? lastVideoPts_ : 0; }

 private:
  void ensureEpoch(std::int64_t now100ns) {
    if (!hasEpoch_) {
      hasEpoch_ = true;
      epoch100ns_ = now100ns;
    }
  }

  bool hasEpoch_ = false;
  std::int64_t epoch100ns_ = 0;
  bool hasLastFrameNumber_ = false;
  std::int64_t lastFrameNumber_ = 0;
  bool hasLastVideoPts_ = false;
  std::int64_t lastVideoPts_ = 0;
  bool audioStarted_ = false;
  std::int64_t audioBase100ns_ = 0;
  std::int64_t audioSamples_ = 0;
  int audioLatencySamples_ = 0;               // set until the first buffer latches it
  std::int64_t audioLatencyOffset100ns_ = 0;  // latched, clamped to the epoch
};

}  // namespace corevideo::modules
