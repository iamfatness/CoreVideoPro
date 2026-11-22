#pragma once

// The CoreVideo Pro Camera extension's own device (mac-shell CameraConstants:
// deviceUUID, legacyDeviceID, defaultDeviceName). CMIO reports the device UID
// as either the UUID string or the legacy id, depending on how the extension
// publishes it (this Mac: the legacy id).
//
// The capture-source list must never offer it: choosing our own Program output
// as a source is a video feedback loop. Since the core now services the main
// run loop (core/MainRunLoopHost.h), the camera appears in AVFoundation
// discovery mid-session as soon as the extension starts.

#include <cctype>
#include <string>

namespace corevideo::modules {

inline constexpr const char* kOwnVirtualCameraUuid = "791E98ED-72E7-4716-9A26-1B4FBB9A1520";
inline constexpr const char* kOwnVirtualCameraLegacyId = "us.iamfatness.corevideopro.camera.device";
inline constexpr const char* kOwnVirtualCameraDefaultName = "CoreVideo Pro Camera";

// Portable (Windows compiles the tests too): ASCII case-insensitive equality.
inline bool equalsIgnoringAsciiCase(const std::string& a, const char* b) {
  std::size_t i = 0;
  for (; i < a.size() && b[i] != '\0'; ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return i == a.size() && b[i] == '\0';
}

inline bool isOwnVirtualCameraUid(const std::string& deviceUid) {
  return equalsIgnoringAsciiCase(deviceUid, kOwnVirtualCameraUuid) || deviceUid == kOwnVirtualCameraLegacyId;
}

// The device UID alone identifies it (an operator-renamed camera keeps its UID);
// the default name also matches, for a device reported under a different id.
inline bool isOwnVirtualCamera(const std::string& deviceUid, const std::string& name) {
  return isOwnVirtualCameraUid(deviceUid) || name == kOwnVirtualCameraDefaultName;
}

}  // namespace corevideo::modules
