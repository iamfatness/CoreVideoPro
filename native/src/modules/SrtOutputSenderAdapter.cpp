#include "modules/Interfaces.h"

#include <memory>

namespace corevideo::modules {

std::unique_ptr<IOutputSender> createSrtOutputSender() {
  // SRT delivery rides the SHARED FFmpeg sender rather than a second libsrt
  // integration: the staged FFmpeg is built with libsrt, and the RTMP sender
  // already owns the hardened process/pipe pipeline (pacing, NV12 feeding,
  // reconnect/backoff, health). This file used to return nullptr on BOTH sides
  // of its #if, so SRT output did not exist at all.
  //
  // COREVIDEO_WITH_SRT_OUTPUT remains the gate for a future in-core libsrt
  // implementation; it is deliberately NOT required here, because the FFmpeg
  // path needs no libsrt at build time.
  return createFfmpegSrtOutputSender();
}

}  // namespace corevideo::modules
