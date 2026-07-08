#pragma once

// Core-side virtual camera publisher (docs/virtual-camera-spec.md V2). Owns the
// shared-memory slot and writes the program frame into it as NV12 with a
// seqlock, once per output tick while the operator has the virtual camera
// enabled. The virtual-camera COM DLL (frameserver.exe) reads this slot.
//
// No pixel work runs under a shared lock: publish() is called from the output
// worker with the program frame it already holds, and the NV12 convert is the
// same class of per-tick work the network senders already do.

#include "modules/Interfaces.h"

#include <cstdint>
#include <memory>
#include <string>

namespace corevideo::modules {

struct VirtualCameraStatus {
  bool enabled = false;
  std::string state = "off";  // off | starting | live | failed
  std::string deviceName = "CoreVideo Pro Camera";
  int width = 1280;
  int height = 720;
  int fps = 30;
  std::uint64_t framesPublished = 0;
  std::string warning;
};

class IVirtualCameraPublisher {
 public:
  virtual ~IVirtualCameraPublisher() = default;
  // Create the SHM slot and (V2) register the OS virtual camera. Idempotent.
  virtual bool start(int width, int height, int fps) = 0;
  // Convert + publish one program frame (no-op when not started).
  virtual void publish(const ProgramFrame& frame) = 0;
  // Remove the OS virtual camera and release the slot. Idempotent.
  virtual void stop() = 0;
  virtual VirtualCameraStatus status() const = 0;
  // Mirror-me: flip the published frame horizontally (optional; default no-op).
  virtual void setMirror(bool mirror) { (void)mirror; }
};

// Dev-gated (COREVIDEO_WITH_VIRTUALCAM). Returns a no-op stub otherwise, so the
// control plane and tests work with no OS camera stack.
std::unique_ptr<IVirtualCameraPublisher> createVirtualCameraPublisher();

}  // namespace corevideo::modules
