#pragma once

// The process half of the macOS virtual-camera sink helper (POSIX; built only
// with COREVIDEO_WITH_MAC_VIRTUALCAM). See HelperVirtualCameraPublisher.h for
// the picture and VcamHelperRespawnPolicy.h for why the sink is out of process.
//
// The helper is the core's OWN executable started with kVcamSinkHelperArgument,
// so it is the same signed file in the same directory the camera extension's
// ProducerAuthorizer already attests: no second binary, no extension change.
// It makes ONE connect attempt at birth and never retries; a failure or a lost
// camera ends the process and the core starts a new one.
//
// File descriptors in the helper: kVcamHelperWakeFd (stdin) is a pipe from the
// core (a byte per frame hand-off; end-of-file means stop, and is also how the
// helper learns the core died), kVcamHelperRegionFd is the shared region
// (VcamHelperTransport.h), stderr is the core's (the helper's log lines join
// the core log), stdout is /dev/null (the core's stdout is the RPC channel).

#include "modules/HelperVirtualCameraPublisher.h"
#include "modules/VirtualCameraSink.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace corevideo::modules {

inline constexpr const char* kVcamSinkHelperArgument = "--vcam-sink-helper";
inline constexpr const char* kVcamDeviceProbeArgument = "--vcam-device-probe";
inline constexpr int kVcamHelperWakeFd = 0;
inline constexpr int kVcamHelperRegionFd = 3;

// Helper exit codes (the core logs them; the shared state carries the reason).
inline constexpr int kVcamHelperExitStopped = 0;
inline constexpr int kVcamHelperExitBadInvocation = 2;
inline constexpr int kVcamHelperExitUnreachable = 3;
inline constexpr int kVcamHelperExitLost = 4;

// The helper loop over an already mapped region. Returns the exit code.
int runVcamSinkHelper(VcamHelperShared* shared, std::size_t mappedBytes, int wakeFd,
                      std::unique_ptr<IVirtualCameraSink> sink, std::function<std::int64_t()> hostClockNs);

// Maps `regionFd` and runs the loop (the helper process's entry).
int runVcamSinkHelperFromDescriptors(int regionFd, int wakeFd, std::unique_ptr<IVirtualCameraSink> sink,
                                     std::function<std::int64_t()> hostClockNs);

// This process's executable, resolved; "" when it cannot be read.
std::string currentExecutablePath();
// The executable of `pid`, resolved; "" when it cannot be read.
std::string executablePathOfProcess(std::int64_t pid);

// The helper feeds whatever its parent hands it into a camera every app on the
// Mac trusts, so it runs only as a child of the same executable (the core).
// Pure: both paths resolved by the caller.
inline bool vcamHelperParentAllowed(const std::string& ownPath, const std::string& parentPath) {
  return !ownPath.empty() && ownPath == parentPath;
}

// Starts `executable` with `arguments` as a sink helper.
std::unique_ptr<IVcamHelperLauncher> createPosixVcamHelperLauncher(std::string executable,
                                                                   std::vector<std::string> arguments);

}  // namespace corevideo::modules
