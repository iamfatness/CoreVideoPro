#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

namespace corevideo::modules {

enum class SrtDecoderFault { None, Codec, Packet };

// FFmpeg owns the SRT socket and decoder process. Classify its error-level
// stderr without ever copying a raw line (which may contain a secret URL) into
// an operator warning or snapshot. Each matching line is an observed error
// event, not a claim about the number of damaged video frames.
inline SrtDecoderFault classifySrtDecoderError(std::string line) {
  std::transform(line.begin(), line.end(), line.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  const auto has = [&](const char* phrase) { return line.find(phrase) != std::string::npos; };
  if (has("corrupt input packet") || has("packet corrupt") ||
      has("pes packet size mismatch") || has("invalid data found when processing input"))
    return SrtDecoderFault::Packet;
  if (has("error while decoding") || has("invalid nal unit") ||
      has("non-existing pps") || has("missing picture in access unit") ||
      (has("concealing ") && has("error")))
    return SrtDecoderFault::Codec;
  return SrtDecoderFault::None;
}

struct SrtIngestHealth {
  std::string connectionState;
  bool signalPresent = false;
  int64_t lastFrameAgeMs = -1;
  std::string warning;
};

// A held last frame remains available for dropout handling, but is not live signal.
inline SrtIngestHealth projectSrtIngestHealth(const std::string& transportState,
                                             const std::string& transportWarning,
                                             int64_t framesReceived,
                                             int64_t lastFrameAtMs,
                                             int64_t nowMs,
                                             const std::string& transport = "SRT") {
  SrtIngestHealth health;
  health.connectionState = transportState;
  health.warning = transportWarning;
  if (framesReceived > 0 && lastFrameAtMs > 0) {
    health.lastFrameAgeMs = nowMs > lastFrameAtMs ? nowMs - lastFrameAtMs : 0;
  }
  if (transportState == "receiving" && health.lastFrameAgeMs >= 0) {
    if (health.lastFrameAgeMs <= 1500) {
      health.signalPresent = true;
    } else {
      health.connectionState = "stalled";
      health.warning = transport + " decoder has not produced a frame for over 1500 ms.";
    }
  }
  return health;
}

}  // namespace corevideo::modules
