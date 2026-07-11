#pragma once

// Temporary diagnostic logger (runs inside the consuming app, e.g. Zoom).
// Writes to %LOCALAPPDATA%\CoreVideoPro\vcam-serve.log so we can see whether the
// stream starts, RequestSample fires, the SHM opens, and real frames vs the
// slate are served. Remove once frame serving is confirmed.

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <string>

namespace corevideo::virtualcam {

inline void VcamServeLog(const char* msg) {
  // Fixed, world-writable path so it works no matter which process/user runs the
  // DLL (the consuming app in the user session, OR the Frame Server as LocalService).
  FILE* f = nullptr;
  if (_wfopen_s(&f, L"C:\\Windows\\Temp\\corevideo-vcam.log", L"a") == 0 && f != nullptr) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD pid = GetCurrentProcessId();
    std::fprintf(f, "[%02d:%02d:%02d.%03d pid=%lu] %s\n", st.wHour, st.wMinute, st.wSecond,
                 st.wMilliseconds, pid, msg);
    std::fclose(f);
  }
}

}  // namespace corevideo::virtualcam
