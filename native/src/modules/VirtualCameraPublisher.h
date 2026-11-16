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
  std::string state = "off";  // off | starting | live | stopping | failed
  std::string deviceName = "CoreVideo Pro Camera";
  int width = 1280;
  int height = 720;
  int fps = 30;
  std::uint64_t framesPublished = 0;
  std::string warning;
  std::uint64_t framesAccepted = 0, pendingFramesReplaced = 0, publicationExceptions = 0;
  bool framerEnabled = false;
  std::string framerState = "off"; // off | waiting | active | unavailable
  std::string framerWarning;
  std::uint64_t framerFrames = 0, framerFailures = 0;
  // macOS sink helper (HelperVirtualCameraPublisher). Every other backend
  // leaves these at "not observed"; -1 means not observed, never false.
  std::uint64_t sinkAttempts = 0;            // helper processes started
  std::int64_t lastConnectedAtUnixMs = 0;    // 0 = never connected
  int deviceVisibleToSystem = -1;            // what the newest helper (a newborn process) saw
  int deviceVisibleInProcess = -1;           // this process's own CoreMediaIO view, same moment
  std::int64_t sinkHelperPid = 0;            // 0 = no helper running
  std::uint64_t sinkHelperGeneration = 0;
  bool sinkHelperGaveUp = false;
  std::int64_t sinkNextAttemptInMs = 0;
  std::string sinkHelperLastExit;            // "" = none yet
  std::uint64_t transportDrops = 0;          // frames the core dropped because the hand-off ring was full
  std::uint64_t sinkQueueFullDrops = 0;      // frames the helper dropped because the camera queue was full
  std::uint64_t sinkSequenceGaps = 0;
  double transportLastMs = -1, transportMaxMs = -1, transportMeanMs = -1;  // hand-off to helper pickup
  double sinkPublishLastMs = -1, sinkPublishMaxMs = -1, sinkPublishMeanMs = -1;  // helper copy + enqueue
};

class IVirtualCameraPublisher {
 public:
  virtual ~IVirtualCameraPublisher() = default;
  // Create the SHM slot and (V2) register the OS virtual camera. Idempotent.
  // Async implementations return acceptance; status() reports completion/failure.
  virtual bool start(int width, int height, int fps) = 0;
  // Convert + publish one program frame (no-op when not started).
  virtual void publish(const ProgramFrame& frame) = 0;
  // Publish an ALREADY-converted NV12 frame (the compositor's tap thread did the
  // BGRA->NV12 conversion off the hot path). Applies mirror + writes the SHM.
  // No-op when not started or the size doesn't match the declared media type.
  virtual void publishNv12(const std::uint8_t* nv12, int width, int height) {
    (void)nv12; (void)width; (void)height;
  }
  // Ownership-preserving fast path used by the D3D tap. Implementations may
  // queue the immutable frame and return immediately, preventing camera SHM I/O
  // from pacing the compositor or the streaming/recording video tick.
  virtual void publishNv12Shared(std::shared_ptr<const std::vector<std::uint8_t>> nv12,
                                 int width, int height) {
    if (nv12) publishNv12(nv12->data(), width, height);
  }
  // Already on the isolated publisher worker. Windows writes directly here
  // instead of adding a second replaceable queue and another scheduling hop.
  virtual void publishNv12OnWorker(std::shared_ptr<const std::vector<std::uint8_t>> nv12,
                                   int width, int height) {
    publishNv12Shared(std::move(nv12), width, height);
  }
  virtual void publishNv12Identified(std::shared_ptr<const std::vector<std::uint8_t>> nv12,
      int width, int height, int64_t /*programSequence*/, int64_t /*deliveredAt100ns*/) {
    publishNv12Shared(std::move(nv12), width, height);
  }
  virtual void publishNv12IdentifiedOnWorker(std::shared_ptr<const std::vector<std::uint8_t>> nv12,
      int width, int height, int64_t /*programSequence*/, int64_t /*deliveredAt100ns*/) {
    publishNv12OnWorker(std::move(nv12), width, height);
  }
  // Remove the OS virtual camera and release the slot. Idempotent.
  virtual void stop() = 0;
  virtual VirtualCameraStatus status() const = 0;
  // Mirror-me: flip the published frame horizontally (optional; default no-op).
  virtual void setMirror(bool mirror) { (void)mirror; }
  virtual void setFramerEnabled(bool enabled) { (void)enabled; }
  virtual void setProgramLoudness(double lufs, bool completeWindow) { (void)lufs; (void)completeWindow; }
  // Operator-set display name; applies on the next start() (optional; no-op default).
  virtual void setDeviceName(const std::string& name) { (void)name; }
  // True when the producer already mirrored the frame before it reached this
  // backend (macOS: the Metal tap mirrors on the GPU). The webcam framer then
  // must not mirror again. Default false keeps Windows exactly as it was.
  virtual bool mirrorAppliedUpstream() const { return false; }
  // The operator turned the camera on: a backend with a bounded retry budget
  // (the macOS sink helper) starts it afresh. Default: nothing to reset.
  virtual void rearmRetryBudget() {}
};

// Dev-gated (COREVIDEO_WITH_VIRTUALCAM). Returns a no-op stub otherwise, so the
// control plane and tests work with no OS camera stack.
std::unique_ptr<IVirtualCameraPublisher> createVirtualCameraPublisher();

// macOS virtual camera only; inert everywhere else.
//
// The CoreMediaIO sink runs in a helper process that is this same executable
// started with a flag (modules/VcamSinkHelper.h). Only the real core calls
// enableVirtualCameraSinkHelper(), from main: a test or tool binary that built
// the camera through createVirtualCameraPublisher() must never re-execute
// ITSELF as a "helper". Without the call the camera reports a helper failure.
void enableVirtualCameraSinkHelper();
// The helper and probe modes of the core executable. Returns true when
// `argv` asked for one of them and it ran; `exitCode` is then the process's.
bool runVirtualCameraProcessMode(int argc, char** argv, int* exitCode);

}  // namespace corevideo::modules
