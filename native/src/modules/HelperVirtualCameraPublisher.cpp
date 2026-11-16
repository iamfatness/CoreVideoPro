#include "modules/HelperVirtualCameraPublisher.h"

#include "core/BoundedAsyncLog.h"
#include "modules/SinkVirtualCameraPublisher.h"
#include "modules/VirtualCameraFrame.h"

#include <chrono>
#include <thread>

namespace corevideo::modules {
namespace {

constexpr std::uint64_t kTransportDropLogEvery = 300;
constexpr const char* kGaveUpWarning =
    "The virtual camera helper keeps failing. Turn the virtual camera off and on to try again.";

double nsToMs(std::uint64_t ns) { return static_cast<double>(ns) / 1'000'000.0; }

std::string exitDescription(int exitCode) {
  if (exitCode < 0) return "signal " + std::to_string(-exitCode);
  return "exit " + std::to_string(exitCode);
}

}  // namespace

HelperVirtualCameraClocks defaultHelperVirtualCameraClocks(std::function<std::int64_t()> hostNs) {
  HelperVirtualCameraClocks clocks;
  clocks.steadyMs = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  clocks.hostNs = std::move(hostNs);
  clocks.unixMs = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  clocks.sleepMs = [](std::int64_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
  return clocks;
}

HelperVirtualCameraPublisher::HelperVirtualCameraPublisher(std::unique_ptr<IVcamHelperLauncher> launcher,
                                                           HelperVirtualCameraClocks clocks,
                                                           std::function<int()> inProcessDeviceVisible)
    : launcher_(std::move(launcher)),
      clocks_(std::move(clocks)),
      inProcessDeviceVisible_(std::move(inProcessDeviceVisible)) {}

HelperVirtualCameraPublisher::~HelperVirtualCameraPublisher() { stop(); }

void HelperVirtualCameraPublisher::rearmRetryBudget() {
  std::lock_guard<std::mutex> lock(mutex_);
  policy_.rearm();
  consecutiveFailures_ = 0;
  status_.sinkHelperGaveUp = false;
}

bool HelperVirtualCameraPublisher::start(int width, int height, int fps) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connected_ && channel_) return true;
  status_.width = width;
  status_.height = height;
  status_.fps = fps;
  retireChannelLocked(false);

  const std::int64_t nowMs = clocks_.steadyMs();
  const auto decision = policy_.next(nowMs);
  status_.sinkHelperGaveUp = decision.gaveUp;
  status_.sinkNextAttemptInMs = decision.waitMs;
  if (decision.gaveUp) {
    // No CAMERA_ prefix: the owning worker stops retrying until the operator
    // turns the camera off and on (rearmRetryBudget).
    status_.state = "failed";
    status_.enabled = false;
    status_.warning = kGaveUpWarning;
    return false;
  }
  if (!decision.spawnNow) {
    // Between attempts: the last failure stands, and its CAMERA_ prefix keeps
    // the worker calling back every second.
    status_.state = "failed";
    status_.enabled = false;
    return false;
  }

  status_.state = "starting";
  std::string launchError;
  policy_.onSpawned();
  status_.sinkAttempts = policy_.attempts();
  channel_ = launcher_ ? launcher_->launch(width & ~1, height & ~1, fps, &launchError) : nullptr;
  if (!channel_) {
    failAttemptLocked("CAMERA_HELPER_FAILED: could not start the virtual camera helper (" +
                          (launchError.empty() ? std::string("no launcher") : launchError) + ").",
                      VcamHelperOutcome::HelperFailed, "not started");
    return false;
  }
  ++generation_;
  status_.sinkHelperGeneration = generation_;
  status_.sinkHelperPid = channel_->pid();

  // One bounded wait for the newborn helper's single connect attempt.
  const std::int64_t deadline = nowMs + kVcamHelperAnswerWaitMs;
  VcamHelperState state = VcamHelperState::Starting;
  int exitCode = 0;
  bool gone = false;
  for (;;) {
    state = vcamHelperReadState(channel_->shared());
    if (state != VcamHelperState::Starting) break;
    gone = channel_->exited(&exitCode);
    if (gone) {
      state = vcamHelperReadState(channel_->shared());  // it may have answered as it left
      break;
    }
    if (clocks_.steadyMs() >= deadline) break;
    clocks_.sleepMs(2);
  }

  if (state == VcamHelperState::Live) {
    connected_ = true;
    ringFullStreak_ = false;
    policy_.onOutcome(VcamHelperOutcome::Connected, clocks_.steadyMs());
    status_.state = "live";
    status_.enabled = true;
    status_.warning.clear();
    status_.lastConnectedAtUnixMs = clocks_.unixMs();
    status_.sinkNextAttemptInMs = 0;
    noteVisibilityLocked(1);
    ::corevideo::core::nativeLogf(
        "[virtualcam] mac sink connected %dx%d@%d through helper pid %lld (generation %llu) after %llu failed attempt(s)\n",
        width, height, fps, static_cast<long long>(channel_->pid()),
        static_cast<unsigned long long>(generation_), static_cast<unsigned long long>(consecutiveFailures_));
    consecutiveFailures_ = 0;
    return true;
  }
  if (state == VcamHelperState::Failed) {
    const std::string warning = vcamHelperReadWarning(channel_->shared());
    noteVisibilityLocked(vcamConnectResultSawDevice(warning) ? 1 : 0);
    failAttemptLocked(warning.empty() ? std::string("the virtual camera helper failed without a reason") : warning,
                      classifyVcamHelperWarning(warning), "reported: " + warning.substr(0, warning.find(':')));
    return false;
  }
  if (gone) {
    failAttemptLocked("CAMERA_HELPER_FAILED: the virtual camera helper ended before it answered (" +
                          exitDescription(exitCode) + ").",
                      VcamHelperOutcome::HelperFailed, exitDescription(exitCode));
    return false;
  }
  failAttemptLocked("CAMERA_HELPER_FAILED: the virtual camera helper did not answer in " +
                        std::to_string(kVcamHelperAnswerWaitMs) + " ms.",
                    VcamHelperOutcome::HelperFailed, "no answer");
  return false;
}

void HelperVirtualCameraPublisher::failAttemptLocked(const std::string& warning, VcamHelperOutcome outcome,
                                                     const std::string& exitText) {
  retireChannelLocked(false);
  connected_ = false;
  const std::int64_t nowMs = clocks_.steadyMs();
  policy_.onOutcome(outcome, nowMs);
  const auto next = policy_.next(nowMs);
  ++consecutiveFailures_;
  // A new failure is logged at once; the same one again only as a heartbeat
  // (SinkVirtualCameraPublisher's rule), so the log shows the retry is alive.
  if (status_.warning != warning) {
    ::corevideo::core::nativeLogf("[virtualcam] mac sink: %s (attempt %llu, helper generation %llu)\n",
                                  warning.c_str(), static_cast<unsigned long long>(consecutiveFailures_),
                                  static_cast<unsigned long long>(generation_));
  } else if (shouldLogRepeatedSinkConnectFailure(consecutiveFailures_)) {
    ::corevideo::core::nativeLogf("[virtualcam] mac sink: still failing after %llu attempts: %s\n",
                                  static_cast<unsigned long long>(consecutiveFailures_), warning.c_str());
  }
  status_.state = "failed";
  status_.enabled = false;
  status_.warning = warning;
  status_.sinkHelperLastExit = exitText;
  status_.sinkHelperGaveUp = next.gaveUp;
  status_.sinkNextAttemptInMs = next.waitMs;
  if (next.gaveUp) {
    ::corevideo::core::nativeLogf(
        "[virtualcam] mac sink helper gave up after %d consecutive helper failures; last: %s\n",
        policy_.consecutiveHelperFailures(), warning.c_str());
    status_.warning = kGaveUpWarning;
  }
}

void HelperVirtualCameraPublisher::noteVisibilityLocked(int systemVisible) {
  status_.deviceVisibleToSystem = systemVisible;
  status_.deviceVisibleInProcess = inProcessDeviceVisible_ ? inProcessDeviceVisible_() : -1;
  // The 2026-11-16 defect, named once when it is seen: the camera exists for a
  // newborn process but not for this one. The helper is what makes it harmless.
  if (systemVisible == 1 && status_.deviceVisibleInProcess == 0 && !loggedInProcessBlind_) {
    loggedInProcessBlind_ = true;
    ::corevideo::core::nativeLogf(
        "[virtualcam] CoreVideo Pro Camera is visible to a new process but not to this process's CoreMediaIO view "
        "(its provider registered after this process first used CoreMediaIO); the sink helper is unaffected\n");
  } else if (status_.deviceVisibleInProcess == 1) {
    loggedInProcessBlind_ = false;
  }
}

void HelperVirtualCameraPublisher::foldCountersLocked() {
  if (!channel_) return;
  auto* shared = channel_->shared();
  basePublished_ += shared->framesPublished.load(std::memory_order_relaxed);
  baseGaps_ += shared->sequenceGaps.load(std::memory_order_relaxed);
  baseQueueFull_ += shared->queueFullDrops.load(std::memory_order_relaxed);
}

void HelperVirtualCameraPublisher::retireChannelLocked(bool graceful) {
  if (!channel_) return;
  int exitCode = 0;
  bool gone = channel_->exited(&exitCode);
  if (!gone) {
    channel_->requestStop();
    const std::int64_t deadline = clocks_.steadyMs() + (graceful ? kVcamHelperStopWaitMs : 50);
    while (!(gone = channel_->exited(&exitCode)) && clocks_.steadyMs() < deadline) clocks_.sleepMs(2);
    if (!gone) channel_->kill();
  }
  foldCountersLocked();
  channel_.reset();
  status_.sinkHelperPid = 0;
}

void HelperVirtualCameraPublisher::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  retireChannelLocked(true);
  connected_ = false;
  ringFullStreak_ = false;
  status_.enabled = false;
  status_.state = "off";
}

void HelperVirtualCameraPublisher::publish(const ProgramFrame&) {
  // Same refusal as SinkVirtualCameraPublisher: no CPU BGRA conversion on the camera path.
  std::lock_guard<std::mutex> lock(mutex_);
  if (warnedCpuProgramFrame_) return;
  warnedCpuProgramFrame_ = true;
  ::corevideo::core::nativeLogf("[virtualcam] mac backend ignores CPU program frames; waiting for the GPU tap\n");
}

void HelperVirtualCameraPublisher::publishNv12(const std::uint8_t* nv12, int width, int height) {
  if (nv12 == nullptr) return;
  std::lock_guard<std::mutex> lock(mutex_);
  publishLocked(nv12, nv12FrameSize(width, height), width, height, 0);
}

void HelperVirtualCameraPublisher::publishNv12Shared(std::shared_ptr<const std::vector<std::uint8_t>> nv12, int width,
                                                     int height) {
  publishNv12OnWorker(std::move(nv12), width, height);
}

void HelperVirtualCameraPublisher::publishNv12OnWorker(std::shared_ptr<const std::vector<std::uint8_t>> nv12,
                                                       int width, int height) {
  publishNv12IdentifiedOnWorker(std::move(nv12), width, height, 0, 0);
}

void HelperVirtualCameraPublisher::publishNv12IdentifiedOnWorker(
    std::shared_ptr<const std::vector<std::uint8_t>> nv12, int width, int height, int64_t programSequence,
    int64_t /*deliveredAt100ns*/) {
  if (!nv12) return;
  std::lock_guard<std::mutex> lock(mutex_);
  publishLocked(nv12->data(), nv12->size(), width, height, programSequence);
}

void HelperVirtualCameraPublisher::publishLocked(const std::uint8_t* nv12, std::size_t bytes, int width, int height,
                                                 int64_t programSequence) {
  if (!connected_ || !channel_ || nv12 == nullptr) return;
  if (width != (status_.width & ~1) || height != (status_.height & ~1)) return;
  const std::size_t expected = nv12FrameSize(width, height);
  if (expected == 0 || bytes < expected) return;

  auto* shared = channel_->shared();
  // The helper ends itself when the camera is lost (it says why first) and can
  // also simply die. Either way the next attempt is a new process.
  int exitCode = 0;
  const bool gone = channel_->exited(&exitCode);
  const auto state = vcamHelperReadState(shared);
  if (state == VcamHelperState::Failed) {
    const std::string warning = vcamHelperReadWarning(shared);
    ::corevideo::core::nativeLogf("[virtualcam] mac sink lost; starting a new helper\n");
    failAttemptLocked(warning.empty() ? std::string("CAMERA_EXTENSION_LOST: CoreVideo Pro Camera restarted; reconnecting.")
                                      : warning,
                      VcamHelperOutcome::CameraUnreachable, "reported: " + warning.substr(0, warning.find(':')));
    return;
  }
  if (gone) {
    failAttemptLocked("CAMERA_HELPER_FAILED: the virtual camera helper ended (" + exitDescription(exitCode) +
                          "); restarting it.",
                      VcamHelperOutcome::HelperFailed, exitDescription(exitCode));
    return;
  }

  if (vcamRingTryWrite(shared, nv12, expected, programSequence, clocks_.hostNs ? clocks_.hostNs() : 0)) {
    ringFullStreak_ = false;
    channel_->wake();
    return;
  }
  // Ring full: drop the NEW frame, never block, never overwrite what the helper holds.
  if (transportDrops_++ % kTransportDropLogEvery == 0) {
    ::corevideo::core::nativeLogf("[virtualcam] mac sink helper hand-off full; dropped %llu frame(s)\n",
                                  static_cast<unsigned long long>(transportDrops_));
  }
  const std::int64_t nowMs = clocks_.steadyMs();
  if (!ringFullStreak_) {
    ringFullStreak_ = true;
    ringFullSinceMs_ = nowMs;
  } else if (nowMs - ringFullSinceMs_ >= kVcamHelperStalledMs) {
    ::corevideo::core::nativeLogf("[virtualcam] mac sink helper took no frame for %lld ms; replacing it\n",
                                  static_cast<long long>(nowMs - ringFullSinceMs_));
    channel_->kill();
    ringFullStreak_ = false;
    failAttemptLocked("CAMERA_HELPER_FAILED: the virtual camera helper stopped taking frames; restarting it.",
                      VcamHelperOutcome::HelperFailed, "stalled");
  }
}

VirtualCameraStatus HelperVirtualCameraPublisher::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = status_;
  result.framesPublished = basePublished_;
  result.sinkSequenceGaps = baseGaps_;
  result.sinkQueueFullDrops = baseQueueFull_;
  result.transportDrops = transportDrops_;
  if (channel_) {
    const auto* shared = channel_->shared();
    result.framesPublished += shared->framesPublished.load(std::memory_order_relaxed);
    result.sinkSequenceGaps += shared->sequenceGaps.load(std::memory_order_relaxed);
    result.sinkQueueFullDrops += shared->queueFullDrops.load(std::memory_order_relaxed);
    const std::uint64_t received = shared->framesReceived.load(std::memory_order_relaxed);
    if (received > 0) {
      result.transportLastMs = nsToMs(shared->transportLastNs.load(std::memory_order_relaxed));
      result.transportMaxMs = nsToMs(shared->transportMaxNs.load(std::memory_order_relaxed));
      result.transportMeanMs = nsToMs(shared->transportSumNs.load(std::memory_order_relaxed)) / received;
      result.sinkPublishLastMs = nsToMs(shared->publishLastNs.load(std::memory_order_relaxed));
      result.sinkPublishMaxMs = nsToMs(shared->publishMaxNs.load(std::memory_order_relaxed));
      result.sinkPublishMeanMs = nsToMs(shared->publishSumNs.load(std::memory_order_relaxed)) / received;
    }
  }
  if (!connected_) {
    const auto next = policy_.next(clocks_.steadyMs());
    result.sinkNextAttemptInMs = next.waitMs;
  }
  return result;
}

void HelperVirtualCameraPublisher::setMirror(bool) {
  // The compositor mirrors on the GPU before the tap; nothing to record here.
}

void HelperVirtualCameraPublisher::setDeviceName(const std::string& name) {
  // The extension owns the device name; reported only (as SinkVirtualCameraPublisher does).
  if (name.empty()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  status_.deviceName = name;
}

}  // namespace corevideo::modules
