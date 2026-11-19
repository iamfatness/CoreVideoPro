#pragma once

// Core side of the macOS virtual camera when the CoreMediaIO sink runs in a
// helper process (plan docs/superpowers/plans/2026-11-16-mac-vcam-provider-
// arrival-fix.md, Task 3 design B; the "why" is in VcamHelperRespawnPolicy.h).
//
//   core:   AsyncVirtualCameraPublisher -> HelperVirtualCameraPublisher --ring--\
//   helper: runVcamSinkHelper -> SinkVirtualCameraPublisher -> CmioVirtualCameraSink
//
// This class does no CoreMediaIO work and no pixel work beyond ONE bulk copy of
// the finished NV12 into the hand-off ring. Every connect attempt is a new
// helper process (IVcamHelperLauncher); when the camera is lost the helper
// exits and the next attempt is again a new process. The size gate, sequence
// gaps, queue-full and stalled-queue rules stay in SinkVirtualCameraPublisher,
// which now runs inside the helper, so they are decided next to the real sink.
//
// `framesPublished` is what the helper reports the camera's queue ACCEPTED,
// never what the core handed off: a hand-off is a submission, not a delivery.
//
// Portable C++ (the launcher is the platform seam), unit-tested everywhere.
// One mutex; callers are the AsyncVirtualCameraPublisher worker only, so the
// bounded wait for a new helper's answer never holds up a command or render
// thread.

#include "modules/VcamHelperRespawnPolicy.h"
#include "modules/VcamHelperTransport.h"
#include "modules/VirtualCameraPublisher.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace corevideo::modules {

// One started helper process and the region it shares with the core.
class IVcamHelperChannel {
 public:
  virtual ~IVcamHelperChannel() = default;  // must leave no process behind
  virtual VcamHelperShared* shared() = 0;
  // A frame is on the ring. Must not block.
  virtual void wake() = 0;
  // True once the process is gone (reaped); `exitCode` is its exit status, or
  // -signal when a signal ended it.
  virtual bool exited(int* exitCode) = 0;
  // Ask the helper to stop its stream and exit (the camera then shows its slate).
  virtual void requestStop() = 0;
  virtual void kill() = 0;
  virtual std::int64_t pid() const = 0;
};

class IVcamHelperLauncher {
 public:
  virtual ~IVcamHelperLauncher() = default;
  // nullptr with `error` set when no process could be started.
  virtual std::unique_ptr<IVcamHelperChannel> launch(int width, int height, int fps, std::string* error) = 0;
};

// How long start() waits for a new helper to say live or failed. The helper
// makes one connect attempt at birth (measured 2026-11-16, Apple M-series,
// macOS 26.3: tens of ms), so a helper silent this long is treated as hung.
inline constexpr std::int64_t kVcamHelperAnswerWaitMs = 1500;
inline constexpr std::int64_t kVcamHelperStopWaitMs = 300;
// The ring full this long means the helper stopped taking frames.
inline constexpr std::int64_t kVcamHelperStalledMs = 2000;

struct HelperVirtualCameraClocks {
  std::function<std::int64_t()> steadyMs;   // monotonic
  std::function<std::int64_t()> hostNs;     // the host clock the helper also reads
  std::function<std::int64_t()> unixMs;     // wall clock, for lastConnectedAt
  std::function<void(std::int64_t)> sleepMs;
};
HelperVirtualCameraClocks defaultHelperVirtualCameraClocks(std::function<std::int64_t()> hostNs);

class HelperVirtualCameraPublisher final : public IVirtualCameraPublisher {
 public:
  // `inProcessDeviceVisible`: this process's own CoreMediaIO view of the camera
  // (1 yes, 0 no, -1 unknown), read once per attempt for diagnostics only.
  HelperVirtualCameraPublisher(std::unique_ptr<IVcamHelperLauncher> launcher, HelperVirtualCameraClocks clocks,
                               std::function<int()> inProcessDeviceVisible = {});
  ~HelperVirtualCameraPublisher() override;

  bool start(int width, int height, int fps) override;
  void stop() override;
  void publish(const ProgramFrame& frame) override;
  void publishNv12(const std::uint8_t* nv12, int width, int height) override;
  void publishNv12Shared(std::shared_ptr<const std::vector<std::uint8_t>> nv12, int width, int height) override;
  void publishNv12OnWorker(std::shared_ptr<const std::vector<std::uint8_t>> nv12, int width, int height) override;
  void publishNv12IdentifiedOnWorker(std::shared_ptr<const std::vector<std::uint8_t>> nv12, int width, int height,
                                     int64_t programSequence, int64_t deliveredAt100ns) override;
  VirtualCameraStatus status() const override;
  void setMirror(bool mirror) override;
  void setDeviceName(const std::string& name) override;
  bool mirrorAppliedUpstream() const override { return true; }  // the Metal tap mirrors
  void rearmRetryBudget() override;

 private:
  void publishLocked(const std::uint8_t* nv12, std::size_t bytes, int width, int height, int64_t programSequence);
  void failAttemptLocked(const std::string& warning, VcamHelperOutcome outcome, const std::string& exitText);
  void retireChannelLocked(bool graceful);
  void foldCountersLocked();
  void noteVisibilityLocked(int systemVisible);

  mutable std::mutex mutex_;
  std::unique_ptr<IVcamHelperLauncher> launcher_;
  HelperVirtualCameraClocks clocks_;
  std::function<int()> inProcessDeviceVisible_;
  VcamHelperRespawnPolicy policy_;
  std::unique_ptr<IVcamHelperChannel> channel_;
  VirtualCameraStatus status_;
  bool connected_ = false;
  bool warnedCpuProgramFrame_ = false;
  bool loggedInProcessBlind_ = false;
  std::uint64_t generation_ = 0;
  std::uint64_t consecutiveFailures_ = 0;
  // Totals of the helpers that already ended; the live one's are added on read.
  std::uint64_t basePublished_ = 0, baseGaps_ = 0, baseQueueFull_ = 0;
  std::uint64_t transportDrops_ = 0;
  bool ringFullStreak_ = false;
  std::int64_t ringFullSinceMs_ = 0;
};

}  // namespace corevideo::modules
