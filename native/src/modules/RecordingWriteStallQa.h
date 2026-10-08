#pragma once
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include "core/BoundedAsyncLog.h"

namespace corevideo::modules {
// Explicit headless fault injection. Disabled unless all three knobs are valid.
// Sleeps only on the owning file worker, before one steady-state WriteSample.
class RecordingWriteStallQa {
 public:
  RecordingWriteStallQa() {
    const char* source = std::getenv("COREVIDEO_QA_RECORDING_STALL_SOURCE");
    const char* duration = std::getenv("COREVIDEO_QA_RECORDING_STALL_MS");
    const char* after = std::getenv("COREVIDEO_QA_RECORDING_STALL_AFTER_FRAMES");
    if (!source || !duration || !after) return;
    try {
      size_t d = 0, a = 0;
      const auto ms = std::stoi(duration, &d), frames = std::stoi(after, &a);
      if (duration[d] || after[a] || ms < 1 || ms > 2000 || frames < 30 || frames > 36000) return;
      source_ = source; milliseconds_ = ms; afterFrames_ = frames;
    } catch (...) {}
  }
  void beforeWrite(const std::string& source, int64_t written) {
    if (used_ || milliseconds_ == 0 || source != source_ || written < afterFrames_) return;
    used_ = true;
    core::nativeLogf("[recording-write-stall-qa] source=%s duration_ms=%d after_frames=%lld begin\n",
                    source.c_str(), milliseconds_, static_cast<long long>(written));
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds_));
    core::nativeLogf("[recording-write-stall-qa] source=%s end\n", source.c_str());
  }
 private:
  std::string source_;
  int milliseconds_ = 0, afterFrames_ = 0;
  bool used_ = false;
};
}
