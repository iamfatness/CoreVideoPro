#pragma once

#include <algorithm>
#include <cstdint>

namespace corevideo::modules {

// Counts compressed video that a destination actually wrote into its muxer.
// This is deliberately not a claim that the remote ingest received the bytes.
struct MuxInputRateWindow {
  std::int64_t startedMs = 0;
  std::int64_t bytes = 0;
  std::int64_t frames = 0;
  double videoMbps = 0;
  double videoFps = 0;
};

inline MuxInputRateWindow observeMuxInputWrite(MuxInputRateWindow state,
                                               std::int64_t nowMs,
                                               std::int64_t payloadBytes) {
  if (payloadBytes <= 0) return state;
  if (state.startedMs <= 0 || nowMs < state.startedMs) state.startedMs = nowMs;
  state.bytes += payloadBytes;
  ++state.frames;
  const auto elapsedMs = nowMs - state.startedMs;
  if (elapsedMs >= 1000) {
    state.videoMbps = static_cast<double>(state.bytes) * 8.0 / (elapsedMs * 1000.0);
    state.videoFps = static_cast<double>(state.frames) * 1000.0 / elapsedMs;
    state.startedMs = nowMs;
    state.bytes = 0;
    state.frames = 0;
  }
  return state;
}

inline double currentMuxInputRate(double measuredRate, std::int64_t nowMs,
                                  std::int64_t lastWriteMs) {
  return lastWriteMs > 0 && nowMs >= lastWriteMs && nowMs - lastWriteMs <= 2000
             ? (std::max)(0.0, measuredRate)
             : 0.0;
}

}  // namespace corevideo::modules
