#include "modules/SinkVirtualCameraPublisher.h"

#include "core/BoundedAsyncLog.h"
#include "modules/VirtualCameraFrame.h"

namespace corevideo::modules {
namespace {
constexpr const char* kLostWarning = "CAMERA_EXTENSION_LOST: CoreVideo Pro Camera restarted; reconnecting.";
constexpr std::uint64_t kGapLogEveryFrames = 600;
constexpr std::uint64_t kDropLogEvery = 300;
// Review Focus 1 (Task 7): the queue full continuously this long, with no device
// change, means the extension stopped draining it. That is a loss, not back-pressure.
constexpr int64_t kStalledQueueLostNs = 2'000'000'000;
constexpr const char* kStalledWarning =
    "CAMERA_EXTENSION_LOST: CoreVideo Pro Camera stopped taking frames; reconnecting.";
}  // namespace

SinkVirtualCameraPublisher::SinkVirtualCameraPublisher(std::unique_ptr<IVirtualCameraSink> sink,
                                                       std::function<int64_t()> hostClockNs)
    : sink_(std::move(sink)), hostClockNs_(std::move(hostClockNs)) {}

SinkVirtualCameraPublisher::~SinkVirtualCameraPublisher() { stop(); }

bool SinkVirtualCameraPublisher::start(int width, int height, int fps) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connected_) return true;
  status_.width = width;
  status_.height = height;
  status_.fps = fps;
  status_.state = "starting";
  lastProgramSequence_ = 0;
  queueFullStreak_ = false;
  const std::string failure = sink_ ? sink_->connect() : std::string("CAMERA_SINK_FAILED: no sink.");
  if (!failure.empty()) {
    status_.state = "failed";
    status_.enabled = false;
    // Logged only when it changes: the owning worker retries every second while
    // the extension is absent (not installed, awaiting approval, restarting).
    if (status_.warning != failure) ::corevideo::core::nativeLogf("[virtualcam] mac sink: %s\n", failure.c_str());
    status_.warning = failure;
    return false;
  }
  connected_ = true;
  status_.state = "live";
  status_.enabled = true;
  status_.warning.clear();
  ::corevideo::core::nativeLogf("[virtualcam] mac sink connected %dx%d@%d\n", width, height, fps);
  return true;
}

void SinkVirtualCameraPublisher::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connected_) {
    sink_->disconnect();
    connected_ = false;
  }
  status_.enabled = false;
  status_.state = "off";
}

void SinkVirtualCameraPublisher::publish(const ProgramFrame&) {
  // The CPU-BGRA fallback. Converting it here would be per-pixel CPU work on the
  // camera path (north star). A Mac build that reaches this is missing the GPU
  // tap, and that must be visible in the log, never quietly converted.
  std::lock_guard<std::mutex> lock(mutex_);
  if (warnedCpuProgramFrame_) return;
  warnedCpuProgramFrame_ = true;
  ::corevideo::core::nativeLogf("[virtualcam] mac backend ignores CPU program frames; waiting for the GPU tap\n");
}

void SinkVirtualCameraPublisher::publishNv12(const std::uint8_t* nv12, int width, int height) {
  if (nv12 == nullptr) return;
  std::lock_guard<std::mutex> lock(mutex_);
  publishLocked(nv12, nv12FrameSize(width, height), width, height, 0);
}

void SinkVirtualCameraPublisher::publishNv12Shared(std::shared_ptr<const std::vector<std::uint8_t>> nv12,
                                                   int width, int height) {
  publishNv12OnWorker(std::move(nv12), width, height);
}

void SinkVirtualCameraPublisher::publishNv12OnWorker(std::shared_ptr<const std::vector<std::uint8_t>> nv12,
                                                     int width, int height) {
  publishNv12IdentifiedOnWorker(std::move(nv12), width, height, 0, 0);
}

void SinkVirtualCameraPublisher::publishNv12IdentifiedOnWorker(std::shared_ptr<const std::vector<std::uint8_t>> nv12,
    int width, int height, int64_t programSequence, int64_t /*deliveredAt100ns*/) {
  if (!nv12) return;
  std::lock_guard<std::mutex> lock(mutex_);
  publishLocked(nv12->data(), nv12->size(), width, height, programSequence);
}

void SinkVirtualCameraPublisher::publishLocked(const std::uint8_t* nv12, std::size_t bytes, int width, int height,
                                               int64_t programSequence) {
  if (!connected_ || nv12 == nullptr) return;
  // The sink's format is fixed; another size is a no-op (Windows parity).
  if (width != (status_.width & ~1) || height != (status_.height & ~1)) return;
  const std::size_t expected = nv12FrameSize(width, height);
  if (expected == 0 || bytes < expected) return;

  if (programSequence > 0) {
    if (lastProgramSequence_ > 0 && programSequence > lastProgramSequence_ + 1)
      sequenceGaps_ += static_cast<std::uint64_t>(programSequence - lastProgramSequence_ - 1);
    lastProgramSequence_ = programSequence;
  }

  // The bytes go to the sink exactly as received: mirror happened on the GPU.
  const int64_t hostTimeNs = hostClockNs_ ? hostClockNs_() : 0;
  switch (sink_->enqueueNv12(nv12, width, height, hostTimeNs)) {
    case SinkEnqueueResult::Ok:
      queueFullStreak_ = false;
      ++status_.framesPublished;
      if (status_.framesPublished % kGapLogEveryFrames == 0) {
        ::corevideo::core::nativeLogf("[virtualcam] mac publication gaps %llu published %llu\n",
                                      static_cast<unsigned long long>(sequenceGaps_),
                                      static_cast<unsigned long long>(status_.framesPublished));
      }
      break;
    case SinkEnqueueResult::QueueFull:
      // Drop the NEW frame; never block, never evict what the consumer holds.
      if (queueFullDrops_++ % kDropLogEvery == 0) {
        ::corevideo::core::nativeLogf("[virtualcam] mac sink queue full; dropped %llu frame(s)\n",
                                      static_cast<unsigned long long>(queueFullDrops_));
      }
      if (!queueFullStreak_) {
        queueFullStreak_ = true;
        queueFullSinceNs_ = hostTimeNs;
      } else if (hostTimeNs - queueFullSinceNs_ >= kStalledQueueLostNs) {
        ::corevideo::core::nativeLogf("[virtualcam] mac sink queue full for %lld ms; extension stopped draining; reconnecting\n",
                                      static_cast<long long>((hostTimeNs - queueFullSinceNs_) / 1'000'000));
        markLostLocked(kStalledWarning);
      }
      return;
    case SinkEnqueueResult::Lost:
      ::corevideo::core::nativeLogf("[virtualcam] mac sink lost; reconnecting\n");
      markLostLocked(kLostWarning);
      return;
  }
}

void SinkVirtualCameraPublisher::markLostLocked(const char* warning) {
  sink_->disconnect();
  connected_ = false;
  queueFullStreak_ = false;
  status_.state = "failed";
  status_.enabled = false;
  status_.warning = warning;
}

VirtualCameraStatus SinkVirtualCameraPublisher::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

void SinkVirtualCameraPublisher::setMirror(bool mirror) {
  // Recorded only: the compositor mirrors on the GPU before the tap (Task 5).
  std::lock_guard<std::mutex> lock(mutex_);
  mirror_ = mirror;
}

void SinkVirtualCameraPublisher::setDeviceName(const std::string& name) {
  // The extension owns the device name (spec §6); the custom-property write to
  // the sink lands with Task 8. Until then the name is reported, as on Windows.
  if (name.empty()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  status_.deviceName = name;
}

std::uint64_t SinkVirtualCameraPublisher::sequenceGapsForTest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sequenceGaps_;
}

}  // namespace corevideo::modules
