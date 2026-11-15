#pragma once
// Test-only probe into the AVFoundation recording writer (AvfProgramTailTest).
// Defined only when COREVIDEO_WITH_AVF_ENCODER is built; nothing in the product
// calls it, and no command, env var or wire field reaches it.
#include <string>

namespace corevideo::modules {

struct AvfStoppedWriterProbe {
  bool setUp = false;
  std::string setUpError;
  bool silenceRefused = false;
  std::string silenceError;
  double silenceMs = 0;
  bool tailRefused = false;
  std::string tailError;
  double tailMs = 0;
  bool finalizeRefused = false;
  std::string finalizeError;
};

// Opens a real writer at `path`, writes one picture and 0.1 s of audio, then
// cancels it (status Cancelled: its inputs are never ready again), and times
// the paths that used to spend a 200 ms readiness wait on such a writer: a
// must-accept silence pad and the tail reconcile (video short of audio).
AvfStoppedWriterProbe probeStoppedWriterForTest(const std::string& path);

}  // namespace corevideo::modules
