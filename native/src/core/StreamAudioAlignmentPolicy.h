#pragma once

#include <cstddef>

namespace corevideo::core {

// GPU-direct video starts at the first encoded access unit. PCM queued before
// that point would start the muxer's audio timeline before its video timeline.
inline bool holdGpuStreamAudio(bool gpuDirect, bool firstVideoWritten) {
  return gpuDirect && !firstVideoWritten;
}

// The GPU encoder reads the live texture, while the Program audio tap has
// already been delayed to match the buffered recording path. Consume that
// content delay once at stream start; the raw-video fallback retains it.
inline std::size_t gpuStreamAudioStartupSkipSamples(
    bool gpuDirect, int programBufferFrames, int sampleRate, int channels) {
  if (!gpuDirect || (programBufferFrames != 2 && programBufferFrames != 3) ||
      sampleRate <= 0 || channels <= 0) return 0;
  return static_cast<std::size_t>(sampleRate * programBufferFrames / 60) *
         static_cast<std::size_t>(channels);
}

}  // namespace corevideo::core
