#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>
namespace corevideo::core {
// Sample-accurate output-content delay. No PTS changes: audio remains a
// continuous sample clock, but its content follows the buffered Program image.
// One instance per output bus; never share this with cue or ISO stems.
class ProgramAudioDelay {
 public:
  const std::vector<float>& process(const std::vector<float>& input, int channels,
      int sampleRate, int videoBufferFrames, int silentFrameCount = 0) {
    const int delayFrames = (videoBufferFrames == 2 || videoBufferFrames == 3) && sampleRate > 0
        ? sampleRate * videoBufferFrames / 60 : 0;
    if (channels <= 0 || delayFrames == 0) {
      ring_.clear(); cursor_ = 0; channels_ = 0; rate_ = 0;
      return input;
    }
    const auto size = static_cast<size_t>(delayFrames) * static_cast<size_t>(channels);
    if (ring_.size() != size || channels_ != channels || rate_ != sampleRate) {
      ring_.assign(size, 0.f); cursor_ = 0; channels_ = channels; rate_ = sampleRate;
    }
    const auto samples = input.empty()
        ? static_cast<size_t>((std::max)(0, silentFrameCount)) * static_cast<size_t>(channels)
        : input.size();
    output_.resize(samples);
    for (size_t i = 0; i < samples; ++i) {
      output_[i] = ring_[cursor_];
      ring_[cursor_] = input.empty() ? 0.f : input[i];
      if (++cursor_ == ring_.size()) cursor_ = 0;
    }
    return output_;
  }
 private:
  std::vector<float> ring_, output_;
  size_t cursor_ = 0;
  int channels_ = 0, rate_ = 0;
};
}  // namespace corevideo::core
