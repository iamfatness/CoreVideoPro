#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include <utility>
#include <optional>

namespace corevideo::modules {
// A playback generation has one clock shared by its audio and video readers.
// Callers supply a monotonic clock; decoder packet counts never advance time.
//
// PAUSE IS A CLOCK STATE, NOT AN IDENTITY (T1.2). Only a new identity (a new
// go-live generation, path or loop mode) resets the clock and bumps the
// generation. Playing -> paused freezes elapsed time where it is; paused ->
// playing resumes from that value by shifting the epoch forward by exactly the
// paused duration, so video due-times (epoch + pts) and the audio sample
// position (elapsed) both continue from the frame that was on air — no jump
// to 0 and no skip of the time spent paused. A new identity that starts
// paused (a cue poster) sits at 0 until it is played.
class MediaPlaybackTimeline {
 public:
  bool configure(const std::string& identity, bool playing, int64_t now100ns) {
    if (generation_ == 0 || identity != identity_) {
      identity_ = identity; paused_ = !playing; epoch_ = now100ns; pausedAt_ = now100ns; ++generation_;
      return true;
    }
    if (playing && paused_) { epoch_ += now100ns - pausedAt_; paused_ = false; }
    else if (!playing && !paused_) { pausedAt_ = now100ns; paused_ = true; }
    return false;
  }
  int64_t elapsed100ns(int64_t now100ns) const {
    return (std::max)(int64_t{0}, (paused_ ? pausedAt_ : now100ns) - epoch_);
  }
  bool videoDue(int64_t pts100ns, int64_t now100ns) const { return pts100ns <= elapsed100ns(now100ns); }
  int64_t epoch100ns() const { return epoch_; }
  uint64_t generation() const { return generation_; }
  bool paused() const { return paused_; }
 private:
  std::string identity_;
  bool paused_ = true;
  int64_t epoch_ = 0, pausedAt_ = 0;
  uint64_t generation_ = 0;
};

// A window can straddle the end of a loop: its target precedes the next
// loop's origin, so the new reader must start at zero, never seek negative.
inline int64_t mediaLoopSeek100ns(int64_t target100ns, int64_t loopOrigin100ns) {
  return (std::max)(int64_t{0}, target100ns - loopOrigin100ns);
}

// Fixed50Hz demand slots. A delayed poll skips expired source windows rather
// than making media audio permanently late relative to video wall time.
class MediaAudioDemandClock {
 public:
  explicit MediaAudioDemandClock(int64_t anchorMs = 0) : anchorMs_(anchorMs) {}
  std::optional<int64_t> takeDue(int64_t nowMs) {
    if (nowMs < nextTimeMs()) return std::nullopt;
    const auto slot = (nowMs - anchorMs_) / 20;
    skipped_ += static_cast<uint64_t>(slot - nextSlot_); nextSlot_ = slot + 1;
    return anchorMs_ + slot * 20;
  }
  int64_t nextTimeMs() const { return anchorMs_ + nextSlot_ * 20; }
  uint64_t skipped() const { return skipped_; }
 private:
  int64_t anchorMs_, nextSlot_ = 0;
  uint64_t skipped_ = 0;
};

// Decoded interleaved PCM uses media PTS, not decoder chunk boundaries. This
// helper emits exact sample windows; absent ranges remain explicitly silent.
//
// Consumed chunks are kept for a short HISTORY behind the cursor. Audio is
// decoded a few windows ahead of the clock; when a clip pauses, those windows
// are dropped unheard, and on resume the clock seeks back to the paused
// position. Retaining ~200 ms lets that backward seek replay the real samples
// instead of turning them into a silent hole (and skipping that media).
class MediaAudioWindows {
 public:
  struct Chunk { int64_t firstSample; std::vector<float> pcm; };
  MediaAudioWindows(int rate = 48000, int channels = 2) : rate_(rate), channels_(channels) {}
  void reset(int rate, int channels) { rate_ = rate; channels_ = channels; cursor_ = 0; chunks_.clear(); }
  void seek(int64_t sample) {
    cursor_ = (std::max)(int64_t{0}, sample);
    trimHistory();
  }
  int64_t cursor() const { return cursor_; }
  int64_t bufferedThrough() const {
    return chunks_.empty() ? cursor_ : (std::max)(cursor_, chunkEnd(chunks_.back()));
  }
  void append(int64_t pts100ns, std::vector<float> pcm) {
    const auto sample = (pts100ns / 10000000) * rate_ + ((pts100ns % 10000000) * rate_ + (pts100ns >= 0 ? 5000000 : -5000000)) / 10000000;
    if (!pcm.empty()) chunks_.push_back({sample, std::move(pcm)});
  }
  std::vector<float> take(int frames) {
    std::vector<float> result(static_cast<size_t>(frames) * channels_, 0.f);
    const auto end = cursor_ + frames;
    for (const auto& chunk : chunks_) {
      const auto chunkEnd = chunk.firstSample + static_cast<int64_t>(chunk.pcm.size() / channels_);
      const auto begin = (std::max)(cursor_, chunk.firstSample);
      const auto stop = (std::min)(end, chunkEnd);
      if (begin < stop) std::copy_n(chunk.pcm.begin() + (begin - chunk.firstSample) * channels_,
          (stop - begin) * channels_, result.begin() + (begin - cursor_) * channels_);
    }
    cursor_ = end;
    trimHistory();
    return result;
  }
 private:
  int64_t chunkEnd(const Chunk& chunk) const { return chunk.firstSample + static_cast<int64_t>(chunk.pcm.size() / channels_); }
  void trimHistory() {
    const int64_t keepFrom = cursor_ - static_cast<int64_t>(rate_) / 5; // 200 ms behind the cursor.
    while (!chunks_.empty() && chunkEnd(chunks_.front()) <= keepFrom) chunks_.pop_front();
  }
  int rate_, channels_;
  int64_t cursor_ = 0;
  std::deque<Chunk> chunks_;
};
} // namespace corevideo::modules
