#pragma once

// CoreMediaIO transport for the macOS virtual camera (Apple +
// COREVIDEO_WITH_MAC_VIRTUALCAM only; docs/virtual-camera-macos-spec.md §2,
// roadmap M9.2). Feeds the "CoreVideo Pro Camera Sink" stream of the CoreVideo
// Pro Camera system extension through the CMIO C HAL (the Apple sample / OBS
// mac-virtualcam pattern). Thin by design: all policy (size gate, loss/retry,
// gap and drop accounting) lives in the portable SinkVirtualCameraPublisher.

#include "modules/VirtualCameraSink.h"

#include <cstdint>
#include <memory>

namespace corevideo::modules {

// Construction is free: no CMIO call happens until connect().
std::unique_ptr<IVirtualCameraSink> createCmioVirtualCameraSink();
// mach_absolute_time in nanoseconds: the host clock CoreMediaIO timestamps use.
int64_t machHostClockNs();

}  // namespace corevideo::modules
