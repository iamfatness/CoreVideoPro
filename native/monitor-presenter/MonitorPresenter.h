#pragma once
#include <cstdint>

#if defined(COREVIDEO_MONITOR_EXPORTS)
#define CV_MONITOR_API extern "C" __declspec(dllexport)
#else
#define CV_MONITOR_API extern "C" __declspec(dllimport)
#endif

// Versioned fixed-width ABI. No managed callback, frame payload or UI object
// crosses this boundary. COM resources are leased until the native owner retires.
struct CvMonitorStats {
  std::uint64_t handle = 0, attempts = 0, retryAttempts = 0, presents = 0;
  std::uint64_t ingested = 0, acquireAttempts = 0, acquireBusy = 0;
  std::uint64_t notReady = 0, presentBusy = 0;
  std::int64_t lastCommitQpc = 0, maximumGapQpc = 0, observedQpc = 0;
  double ingestMs = 0, copyMs = 0, copyMaximumMs = 0, presentMs = 0, presentMaximumMs = 0, totalMs = 0;
  std::int32_t hresult = 0;
  std::uint32_t active = 0, pixel = 0, pixelValid = 0;
  // Cumulative accepted-commit intervals. Take endpoint deltas; a lifetime
  // maximum or average cannot qualify an interval containing occasional stalls.
  std::uint64_t gaps = 0, gapsOver25Ms = 0, gapsOver33Ms = 0, gapsOver50Ms = 0;
  // Observation only: original slot target, actual native wake, and GPU-call
  // boundaries for the latest >25 ms commit interval. Never re-time a slot.
  std::uint64_t periodicWakes = 0, wakesOver1Ms = 0, wakesOver4Ms = 0, wakesOver8Ms = 0;
  std::int64_t maximumWakeLatenessQpc = 0;
  std::uint64_t lastGapSequence = 0, lastGapSourceGeneration = 0, lastGapAcquireBusy = 0, lastGapNotReady = 0;
  std::int64_t lastGapPreviousCommitQpc = 0, lastGapStartQpc = 0, lastGapDueQpc = 0, lastGapWakeQpc = 0;
  std::int64_t lastGapAcquiredQpc = 0, lastGapCreditQpc = 0, lastGapPresentQpc = 0, lastGapCommitQpc = 0;
  std::uint64_t lastGapCpuCycles = 0;
  std::uint32_t workerThreadId = 0, lastGapWaitResult = 0;
};
CV_MONITOR_API std::uint32_t cv_monitor_stats_size();
CV_MONITOR_API void* cv_monitor_create(void* device, void* context, void* chain,
    void* backBuffer, std::uint64_t sharedHandle, void* frameReady, std::uint32_t flags);
CV_MONITOR_API void cv_monitor_release(void* lease);
CV_MONITOR_API void cv_monitor_active(void* lease, std::int32_t active);
CV_MONITOR_API void cv_monitor_probe_reset(void* lease);
CV_MONITOR_API void cv_monitor_probe_pixel(void* lease);
CV_MONITOR_API std::int32_t cv_monitor_snapshot(void* lease, CvMonitorStats* result);
