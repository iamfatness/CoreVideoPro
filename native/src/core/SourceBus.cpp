#include "core/SourceBus.h"

#include "core/CaptureDeviceSource.h"

namespace corevideo::core {

void SourceBus::recordCaptureObservation(ISource& source, int64_t nowNs) {
  if (auto* capture = dynamic_cast<CaptureDeviceSource*>(&source)) {
    capture->recordOnAirObservation(nowNs);
  }
}

std::optional<CaptureOnAirObservation> SourceBus::captureObservationFor(const ISource& source) {
  if (source.descriptor().kind == "capture") {
    if (const auto* capture = dynamic_cast<const CaptureDeviceSource*>(&source)) {
      return capture->onAirObservation();
    }
  }
  return std::nullopt;
}

}  // namespace corevideo::core
