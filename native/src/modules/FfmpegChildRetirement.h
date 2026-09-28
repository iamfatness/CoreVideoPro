#pragma once

// Stop owns the child handle until it has exited. A timed-out graceful EOF
// cannot be treated as a completed stream stop: FFmpeg may still be publishing
// and a restart may otherwise overlap it with a second child.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace corevideo::modules {

inline bool retireFfmpegChild(HANDLE process, DWORD gracefulWaitMs = 500) {
  if (process == nullptr) return true;
  const DWORD graceful = ::WaitForSingleObject(process, gracefulWaitMs);
  if (graceful == WAIT_OBJECT_0) return true;
  if (graceful == WAIT_FAILED) return false;

  // TerminateProcess is asynchronous. Keep ownership and wait for the process
  // object to signal before the caller can create its replacement.
  if (!::TerminateProcess(process, 1)) {
    DWORD exitCode = STILL_ACTIVE;
    if (!::GetExitCodeProcess(process, &exitCode) || exitCode == STILL_ACTIVE) return false;
  }
  return ::WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0;
}

}  // namespace corevideo::modules
#endif
