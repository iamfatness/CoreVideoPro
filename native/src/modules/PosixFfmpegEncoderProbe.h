#pragma once

// Is this ffmpeg encoder registered AND able to open on this machine? (POSIX,
// macOS parity Phase 4 Task 1 / O3.)
//
// The Windows probe (RtmpOutputSenderAdapter.cpp, ffmpegEncoderIsAvailable)
// encodes 30 synthetic frames and trusts only exit code 0, because
// `ffmpeg -h encoder=<name>` exits 0 even for an unknown name. Before this the
// POSIX branch answered `true` for every encoder, so a Mac asked for an encoder
// the bundled LGPL ffmpeg lacks (or whose hardware session cannot open) chose
// it anyway and failed at stream start instead of at selection.
//
// Same argv, same 5 s budget as Windows. The child runs under
// corevideo-child-guard (posix_child::spawn), so a core killed inside the
// window leaves nothing behind; a child that outlives the budget is stopped by
// process group. Blocking: call it from a writer thread, never the render tick
// or under a mutex a writer needs.

#if !defined(_WIN32)

#include "modules/PosixChildProcess.h"

#include <chrono>
#include <string>
#include <thread>

#include <cerrno>
#include <sys/wait.h>

namespace corevideo::modules {

inline bool posixFfmpegEncoderIsAvailable(const std::string& executable, const std::string& encoderName,
                                          std::chrono::milliseconds budget = std::chrono::milliseconds(5000)) {
  if (executable.empty() || encoderName.empty()) {
    return false;
  }
  posix_child::SpawnRequest request;
  request.executable = executable;
  request.arguments = {"-hide_banner", "-loglevel", "quiet", "-nostdin",
                       "-f", "lavfi", "-i", "color=c=black:s=1280x720:r=30",
                       "-frames:v", "30", "-an", "-c:v", encoderName,
                       "-f", "null", "-"};
  const auto spawned = posix_child::spawn(request);
  if (spawned.pid <= 0) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + budget;
  int status = 0;
  for (;;) {
    const pid_t reaped = ::waitpid(spawned.pid, &status, WNOHANG);
    if (reaped == spawned.pid) {
      // The guard mirrors its target's exit code (or terminating signal).
      return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    if (reaped < 0 && errno != EINTR) {
      return false;  // not ours any more; no evidence the encoder works
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      posix_child::stopGroup(spawned.pid, std::chrono::milliseconds(200), nullptr);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}  // namespace corevideo::modules

#endif  // !_WIN32
