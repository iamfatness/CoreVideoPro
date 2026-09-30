#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace corevideo::modules {

struct ProgramAacPacket {
  std::vector<uint8_t> adts;
  int64_t pts100ns = 0;
  int64_t sampleIndex = 0;
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

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace corevideo::modules
