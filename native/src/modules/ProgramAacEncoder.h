#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace corevideo::modules {

struct ProgramAacPacket {
  std::vector<uint8_t> adts;
  int64_t pts100ns = 0;
  int64_t sampleIndex = 0;
  // Stream-session anchor (ProgramStreamClock): the Program frame number that
  // sample `anchorSampleIndex` coincides with. Negative until anchored.
  int64_t anchorFrameNumber = -1;
  int64_t anchorSampleIndex = 0;
  // Sub-frame steady-clock offset of that sample from that frame's timeline.
  int64_t anchorOffset100ns = 0;
};

// One 48 kHz stereo AAC-LC program encoder. Packet time is the exact count of
// accepted PCM frames; output transports may subtract their session epoch.
class ProgramAacEncoder {
 public:
  ProgramAacEncoder();
  ~ProgramAacEncoder();
  ProgramAacEncoder(const ProgramAacEncoder&) = delete;
  ProgramAacEncoder& operator=(const ProgramAacEncoder&) = delete;

  bool start();
  void stop();
  bool encode(const std::vector<float>& pcm, int channels, int sampleRate,
              std::vector<ProgramAacPacket>& packets);
  [[nodiscard]] bool running() const;
  // Samples accepted so far, including any not yet a full 1024-sample packet:
  // the index the next submitted PCM sample will carry.
  [[nodiscard]] int64_t acceptedSamples() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace corevideo::modules
