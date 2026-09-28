#pragma once

#include <cstdint>
#include <string>

namespace corevideo::modules {

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
