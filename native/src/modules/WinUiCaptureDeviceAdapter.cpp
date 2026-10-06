#include "WinUiCaptureDeviceAdapter.h"
#include <algorithm>
#include <utility>

namespace corevideo::modules {

WinUiCaptureDeviceAdapter::WinUiCaptureDeviceAdapter(std::unique_ptr<ICaptureDevice> inner,
    std::function<void()> beforeShmCopy)
    : inner_(std::move(inner)), preparation_(std::move(beforeShmCopy)) {}

WinUiCaptureDeviceAdapter::~WinUiCaptureDeviceAdapter() = default;

void WinUiCaptureDeviceAdapter::registerCaptureBuffer(const std::string& deviceId,
    const std::string& shmName, int width, int height) {
  preparation_.registerBuffer(deviceId, shmName, width, height);
}
void WinUiCaptureDeviceAdapter::unregisterCaptureBuffer(const std::string& deviceId) {
  preparation_.unregisterBuffer(deviceId);
}
CapturePreparationDiagnostics WinUiCaptureDeviceAdapter::shmCapturePreparationDiagnostics() const {
#ifndef _WIN32
  return {};
#else
  const auto stats = preparation_.stats();
  return {true, stats.state, stats.reason, stats.accepted, stats.refused, stats.prepared,
      stats.torn, stats.poolBusy, stats.failed, stats.residentBytes,
      ShmCapturePreparation::kBudgetBytes, stats.active, stats.retiring,
      stats.copyTotalNs, stats.copyMaximumNs, stats.lastRefusalReason};
#endif
}

void WinUiCaptureDeviceAdapter::captureVideoTick(int64_t timestampMs) {
  struct Collect final : ICaptureVideoConsumer {
    std::vector<VideoFrame> frames;
    void publish(VideoFrame frame) override { frames.push_back(std::move(frame)); }
    void end(const std::string&) override {}
  } collect;
  inner_->deliverVideo(collect, timestampMs);
  std::vector<VideoFrame> frames = std::move(collect.frames);

  for (auto frame : preparation_.latest()) {
    // Preserve the legacy delivery clock separately from the immutable
    // preparation observation time and source identity.
    frame.timestampMs = timestampMs;
    frames.erase(std::remove_if(frames.begin(), frames.end(),
        [&](const VideoFrame& inner) { return inner.participantId == frame.participantId; }), frames.end());
    frames.push_back(std::move(frame));
  }

  replaceVideo(std::move(frames));
}

}  // namespace corevideo::modules
