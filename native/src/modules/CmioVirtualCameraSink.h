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
#include <string>

namespace corevideo::modules {

// Construction is free: no CMIO call happens until connect().
std::unique_ptr<IVirtualCameraSink> createCmioVirtualCameraSink();
// THIS process's CoreMediaIO view of the CoreVideo Pro Camera device. A process
// only knows the camera-extension providers that existed at its first
// CoreMediaIO touch, so "not registered" here is the truth about the Mac only
// in a newborn process (the sink helper, `corevideo-native --vcam-device-probe`).
struct OwnCameraDeviceView {
  bool registered = false;
  // kCMIODevicePropertyDeviceIsRunningSomewhere; an unreadable flag counts as running.
  bool runningSomewhere = false;
  std::string deviceUid;
  std::string modelUid;  // carries the extension version ("CoreVideo Pro Camera/3003")
};
OwnCameraDeviceView readOwnCameraDeviceView();
// mach_absolute_time in nanoseconds: the host clock CoreMediaIO timestamps use.
int64_t machHostClockNs();

}  // namespace corevideo::modules
