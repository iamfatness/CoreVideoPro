#pragma once

#include "core/BoundedAsyncLog.h"
#ifdef _WIN32
#include <Windows.h>
#include <avrt.h>
#endif

namespace corevideo::core {
// Register only deadline-bearing video workers. MMCSS retains its CPU quota;
// this does not raise the process priority or change the system task settings.
// Construction and destruction must both occur on the worker being registered.
class ScopedVideoScheduling {
 public:
  explicit ScopedVideoScheduling(const char* role) {
#ifdef _WIN32
    DWORD index = 0;
    task_ = ::AvSetMmThreadCharacteristicsW(L"Playback", &index);
    const DWORD registrationError = task_ ? ERROR_SUCCESS : ::GetLastError();
    const bool prioritySet = task_ && ::AvSetMmThreadPriority(task_, AVRT_PRIORITY_HIGH);
    nativeLogf("[video-scheduling] worker=%s mmcss=%d priority_set=%d registration_error=%lu thread=%lu\n",
        role, task_ != nullptr, prioritySet, static_cast<unsigned long>(registrationError),
        static_cast<unsigned long>(::GetCurrentThreadId()));
#else
    (void)role;
#endif
  }
  ~ScopedVideoScheduling() {
#ifdef _WIN32
    if (task_) ::AvRevertMmThreadCharacteristics(task_);
#endif
  }
  ScopedVideoScheduling(const ScopedVideoScheduling&) = delete;
  ScopedVideoScheduling& operator=(const ScopedVideoScheduling&) = delete;
 private:
#ifdef _WIN32
  HANDLE task_ = nullptr;
#endif
};
}
