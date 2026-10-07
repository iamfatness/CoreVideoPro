#pragma once

#include "modules/Interfaces.h"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace corevideo::core {

// Operator trim only. Positive delays one Zoom guest's PCM; negative delays
// that guest's picture. This never changes timestamps or adds a global bus
// delay. Source ids here are the Zoom engine's bare participant ids.
inline int clampGuestAvOffsetMs(int value) {
  return std::clamp(value, -200, 200);
}

class GuestAvSyncAudio {
 public:
  void observeEpoch(uint64_t epoch) {
    if (epoch != epoch_) {
      states_.clear();
      epoch_ = epoch;
    }
  }

  // Call once per 20 ms audio tick, after the steady feed and sample-rate
  // conversion, before any bus or ISO consumer sees the isolated source.
  void apply(std::vector<modules::AudioFrame>& frames,
             const std::map<std::string, int>& offsets, int tickMs = 20) {
    if (offsets.empty()) {
      states_.clear();
      return;
    }
    std::set<std::string> seen;
    for (auto& frame : frames) {
      auto wanted = offsets.find(frame.participantId);
      if (wanted == offsets.end() || wanted->second <= 0) continue;
      seen.insert(frame.participantId);
      auto& state = states_[frame.participantId];
      state.lastFrame = frame;
      process(frame, state, clampGuestAvOffsetMs(wanted->second), tickMs);
    }
    for (auto it = states_.begin(); it != states_.end();) {
      auto wanted = offsets.find(it->first);
      if (wanted == offsets.end() || wanted->second <= 0) {
        it = states_.erase(it);
        continue;
      }
      if (!seen.count(it->first)) {
        auto frame = it->second.lastFrame;
        frame.pcm.clear();
        frame.sampleCount = 0;
        process(frame, it->second, clampGuestAvOffsetMs(wanted->second), tickMs);
        // Emit a whole silent/input-delayed tick while packets are gated. The
        // mixer and ISO sink then keep their normal sample clocks.
        frames.push_back(std::move(frame));
      }
      ++it;
    }
  }

 private:
  struct State {
    std::vector<float> ring;
    size_t cursor = 0;
    int offsetMs = 0;
    int rate = 0;
    int channels = 0;
    modules::AudioFrame lastFrame;
  };
  std::map<std::string, State> states_;
  uint64_t epoch_ = 0;

  static void process(modules::AudioFrame& frame, State& state, int offsetMs,
                      int tickMs) {
    if (frame.sampleRate <= 0 || frame.channels <= 0) return;
    const size_t channels = static_cast<size_t>(frame.channels);
    const size_t delayFrames = static_cast<size_t>(frame.sampleRate) *
                               static_cast<size_t>(offsetMs) / 1000;
    const size_t delaySamples = delayFrames * channels;
    if (delaySamples == 0) return;
    if (state.offsetMs != offsetMs || state.rate != frame.sampleRate ||
        state.channels != frame.channels || state.ring.size() != delaySamples) {
      state.ring.assign(delaySamples, 0.f);
      state.cursor = 0;
      state.offsetMs = offsetMs;
      state.rate = frame.sampleRate;
      state.channels = frame.channels;
    }
    const size_t tickFrames = static_cast<size_t>(frame.sampleRate) *
                              static_cast<size_t>(tickMs) / 1000;
    const size_t samples = frame.pcm.empty() ? tickFrames * channels : frame.pcm.size();
    std::vector<float> output(samples, 0.f);
    for (size_t i = 0; i < samples; ++i) {
      output[i] = state.ring[state.cursor];
      state.ring[state.cursor] = frame.pcm.empty() ? 0.f : frame.pcm[i];
      if (++state.cursor == state.ring.size()) state.cursor = 0;
    }
    frame.pcm = std::move(output);
    frame.sampleCount = static_cast<int>(samples / channels);
  }
};

class GuestAvSyncVideo {
 public:
  void observeEpoch(uint64_t epoch) {
    if (epoch != epoch_) {
      states_.clear();
      epoch_ = epoch;
    }
  }

  // Frame payloads are shared_ptrs, so this queue retains at most 200 ms of
  // references to decoded frames; there is no CPU pixel copy or extra decode.
  void apply(std::vector<modules::VideoFrame>& frames,
             const std::map<std::string, int>& offsets, int64_t nowMs) {
    if (offsets.empty()) {
      states_.clear();
      return;
    }
    for (auto it = states_.begin(); it != states_.end();) {
      auto wanted = offsets.find(it->first);
      if (wanted == offsets.end() || wanted->second >= 0) it = states_.erase(it);
      else ++it;
    }
    for (auto& frame : frames) {
      auto wanted = offsets.find(frame.participantId);
      if (wanted == offsets.end() || wanted->second >= 0 ||
          (!frame.hasI420() && !frame.hasPixels())) continue;
      const int delayMs = -clampGuestAvOffsetMs(wanted->second);
      auto& state = states_[frame.participantId];
      if (state.delayMs != delayMs || state.sourceEpoch != frame.sourceEpoch) {
        state = {};
        state.delayMs = delayMs;
        state.sourceEpoch = frame.sourceEpoch;
      }
      if (!state.hasSeen || state.lastSeenFrameId != frame.frameId) {
        state.queue.push_back({nowMs + delayMs, frame});
        state.lastSeenFrameId = frame.frameId;
        state.hasSeen = true;
      }
      // Hold the current picture during the initial warmup instead of
      // publishing a blank; subsequent pictures mature at the chosen delay.
      if (!state.hasReleased) {
        state.released = frame;
        state.hasReleased = true;
      }
      while (!state.queue.empty() && state.queue.front().readyAtMs <= nowMs) {
        state.released = std::move(state.queue.front().frame);
        state.queue.pop_front();
      }
      // A stalled producer must not grow this queue without bound.
      while (state.queue.size() > 16) state.queue.pop_front();
      frame = state.released;
    }
  }

 private:
  struct Pending {
    int64_t readyAtMs = 0;
    modules::VideoFrame frame;
  };
  struct State {
    std::deque<Pending> queue;
    uint64_t sourceEpoch = 0;
    modules::VideoFrame released;
    int64_t lastSeenFrameId = 0;
    int delayMs = 0;
    bool hasSeen = false;
    bool hasReleased = false;
  };
  std::map<std::string, State> states_;
  uint64_t epoch_ = 0;
};

}  // namespace corevideo::core
