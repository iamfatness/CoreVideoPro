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
#include <share.h>
#include "modules/VirtualCameraShm.h"
#include "VcamLogQueue.h"

namespace corevideo::virtualcam {

inline bool WriteVcamServeLog(std::string_view message, void* context) {
  FILE* f = nullptr;
  // Honor the same isolated directory as the pixel transport in native tests.
  const auto& path = *static_cast<const std::string*>(context);
  f = _fsopen(path.c_str(), "ab", _SH_DENYNO);
  if (f != nullptr) {
    const bool written = std::fwrite(message.data(), 1, message.size(), f) == message.size();
    return std::fclose(f) == 0 && written;
  }
  return false;
}

inline VcamLogQueue& VcamServeLogQueue() {
  struct Logger {
    std::string path = corevideo::modules::virtualCameraShmDir() + "\\vcam-serve.log";
    VcamLogQueue queue{&WriteVcamServeLog, &path};
  };
  static Logger logger;
  return logger.queue;
}

inline void VcamServeLog(const char* msg) {
  SYSTEMTIME st; GetLocalTime(&st);
  char line[VcamLogQueue::MessageBytes];
  const int length = std::snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d pid=%lu] %s\n",
      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(), msg);
  if (length > 0) VcamServeLogQueue().write(std::string_view(line,
      static_cast<size_t>(length) < sizeof(line) ? static_cast<size_t>(length) : sizeof(line) - 1));
}

}  // namespace corevideo::virtualcam
