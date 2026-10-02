#pragma once

// Is the MACHINE about to refuse memory? Said out loud, before it happens.
//
// #728 (live 2026-10-01). The core died three times in 2.5 minutes because Windows was out
// of commit; the core itself held about 1 GB. Dropping a frame that cannot be allocated
// (core/FrameAllocation.h) survives part of that, but not all of it: measured under a
// per-process commit cap, a core that cannot commit the stack of a NEW THREAD dies inside
// the Windows loader (STATUS_STACK_OVERFLOW in LdrpInitializeThread), where no handler of
// ours runs. So the remaining defence is to tell the operator while there is still time to
// close something. Windows logged its own low-virtual-memory event in the very second
// Program froze; the app said nothing.
//
// What is measured is AVAILABLE COMMIT (GlobalMemoryStatusEx ullAvailPageFile): the bytes
// this process could still commit, which is what operator new and thread creation consume.
// Physical RAM is not the limit that kills.
//
// Pure decision + a tiny sampler, in the MonitorShedPolicy / OutputLifecyclePolicy shape.

#include <cstdint>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace corevideo::core {

struct SystemMemorySample {
  bool measured = false;
  std::uint64_t commitAvailableBytes = 0;
  std::uint64_t commitLimitBytes = 0;
};

enum class SystemMemoryLevel { Unknown, Ok, Low, Critical };

inline const char* systemMemoryLevelName(SystemMemoryLevel level) {
  switch (level) {
    case SystemMemoryLevel::Ok: return "ok";
    case SystemMemoryLevel::Low: return "low";
    case SystemMemoryLevel::Critical: return "critical";
    default: return "unknown";
  }
}

// The core peaks between about 0.7 GB (a small show) and 3.3 GB (a full meeting with
// recording). LOW leaves room for one more core's worth; CRITICAL is less than a small
// show's worth, where the next source or stream start may not get its memory.
inline constexpr std::uint64_t kSystemMemoryLowBytes = 4ull * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kSystemMemoryCriticalBytes = 1ull * 1024 * 1024 * 1024;
// Leaving a level needs this much more than entering it, so a machine hovering at the line
// does not log a transition on every sample.
inline constexpr std::uint64_t kSystemMemoryRecoverMarginBytes = 512ull * 1024 * 1024;

// `previous` supplies the hysteresis: pass the level returned for the last sample.
inline SystemMemoryLevel classifySystemMemory(const SystemMemorySample& sample,
                                              SystemMemoryLevel previous = SystemMemoryLevel::Unknown) {
  if (!sample.measured) return SystemMemoryLevel::Unknown;
  const std::uint64_t available = sample.commitAvailableBytes;
  const std::uint64_t criticalLine = kSystemMemoryCriticalBytes +
      (previous == SystemMemoryLevel::Critical ? kSystemMemoryRecoverMarginBytes : 0);
  if (available < criticalLine) return SystemMemoryLevel::Critical;
  const bool wasLowOrWorse = previous == SystemMemoryLevel::Low || previous == SystemMemoryLevel::Critical;
  const std::uint64_t lowLine = kSystemMemoryLowBytes + (wasLowOrWorse ? kSystemMemoryRecoverMarginBytes : 0);
  return available < lowLine ? SystemMemoryLevel::Low : SystemMemoryLevel::Ok;
}

inline SystemMemorySample sampleSystemMemory() {
  SystemMemorySample sample;
#if defined(_WIN32)
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (GlobalMemoryStatusEx(&status)) {
    sample.measured = true;
    sample.commitAvailableBytes = status.ullAvailPageFile;
    sample.commitLimitBytes = status.ullTotalPageFile;
  }
#endif
  return sample;
}

}  // namespace corevideo::core
