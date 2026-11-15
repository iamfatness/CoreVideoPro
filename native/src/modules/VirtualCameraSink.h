#pragma once

// The seam between the portable macOS virtual-camera publisher and the OS camera
// transport (docs/virtual-camera-macos-spec.md §2, roadmap M9.2). Production is
// CmioVirtualCameraSink (CoreMediaIO sink stream of the CoreVideo Pro Camera
// extension); tests drive a fake. Kept in its own header so the publisher and its
// tests compile on every platform.

#include <cstdint>
#include <string>

namespace corevideo::modules {

enum class SinkEnqueueResult { Ok, QueueFull, Lost };

class IVirtualCameraSink {
 public:
  virtual ~IVirtualCameraSink() = default;
  // "" on success; otherwise a warning beginning "CAMERA_EXTENSION_NOT_FOUND:" or "CAMERA_SINK_FAILED:".
  virtual std::string connect() = 0;
  // Copies the bytes; never retains `nv12`. Must not block.
  virtual SinkEnqueueResult enqueueNv12(const uint8_t* nv12, int width, int height, int64_t hostTimeNs) = 0;
  // Task 8. Returns true when the extension accepted a name that differs from the current one.
  virtual bool setDeviceName(const std::string& name) { (void)name; return false; }
  virtual void disconnect() = 0;
};

}  // namespace corevideo::modules
