#pragma once
#include <cstdint>
#include <optional>

namespace corevideo::modules {
// Source sample positions include packets refused by any recording queue.
// Queue latency cannot change them. Only explicit position gaps insert silence;
// normal callback clock jitter must not disturb sample-counted Program audio.
class RecordingAudioContinuity {
 public:
  uint64_t gapBefore(uint64_t position, uint64_t frames) {
    const auto gap = next_ && position > *next_ ? position - *next_ : 0;
    next_ = position + frames;
    return gap;
  }
 private:
  std::optional<uint64_t> next_;
};
}
