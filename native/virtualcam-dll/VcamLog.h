#pragma once

// Serve-diagnostics logger. Runs in WHATEVER process hosts the media source:
// the consuming app, the creating app, or a locked-down Frame Server service
// worker. Writes to %ProgramData%\CoreVideoPro\vcam-serve.log - the publisher
// pre-creates that file with a permissive DACL (ensureVirtualCameraServeLogFile)
// precisely so restricted service processes can append; C:\Windows\Temp is NOT
// writable from those processes and left us blind on the serving side.

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include "modules/VirtualCameraShm.h"

namespace corevideo::virtualcam {

inline void VcamServeLog(const char* msg) {
  FILE* f = nullptr;
  // Honor the same isolated directory as the pixel transport in native tests.
  const auto path = corevideo::modules::virtualCameraShmDir() + "\\vcam-serve.log";
  if (fopen_s(&f, path.c_str(), "a") == 0 && f != nullptr) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD pid = GetCurrentProcessId();
    std::fprintf(f, "[%02d:%02d:%02d.%03d pid=%lu] %s\n", st.wHour, st.wMinute, st.wSecond,
                 st.wMilliseconds, pid, msg);
    std::fclose(f);
  }
}

}  // namespace corevideo::virtualcam
