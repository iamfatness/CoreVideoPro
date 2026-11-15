#pragma once

// macOS virtual-camera backend (docs/virtual-camera-macos-spec.md §2, roadmap
// M9.2): hands finished 1920x1080 NV12 from the GPU tap to an IVirtualCameraSink
// (production: the CoreMediaIO sink stream of the CoreVideo Pro Camera extension).
//
// Portable C++ so it is unit-tested on every platform. It does NO pixel work:
// mirror is applied upstream on the GPU (the Metal tap), and the CPU-BGRA
// ProgramFrame fallback is refused loudly rather than converted on the CPU.
// One mutex guards every member; the only work done under it is the sink's
// bounded plane copy. Callers are the AsyncVirtualCameraPublisher worker (and
// the command thread through it), never the render thread.

#include "modules/VirtualCameraPublisher.h"
#include "modules/VirtualCameraSink.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace corevideo::modules {

class SinkVirtualCameraPublisher final : public IVirtualCameraPublisher {
 public:
  SinkVirtualCameraPublisher(std::unique_ptr<IVirtualCameraSink> sink, std::function<int64_t()> hostClockNs);
  ~SinkVirtualCameraPublisher() override;

  bool start(int width, int height, int fps) override;
  void stop() override;
  void publish(const ProgramFrame& frame) override;
  void publishNv12(const std::uint8_t* nv12, int width, int height) override;
  void publishNv12Shared(std::shared_ptr<const std::vector<std::uint8_t>> nv12, int width, int height) override;
  void publishNv12OnWorker(std::shared_ptr<const std::vector<std::uint8_t>> nv12, int width, int height) override;
  void publishNv12IdentifiedOnWorker(std::shared_ptr<const std::vector<std::uint8_t>> nv12,
      int width, int height, int64_t programSequence, int64_t deliveredAt100ns) override;
  VirtualCameraStatus status() const override;
  void setMirror(bool mirror) override;
  void setDeviceName(const std::string& name) override;
  bool mirrorAppliedUpstream() const override { return true; }  // the Metal tap mirrors (Task 5)

  std::uint64_t sequenceGapsForTest() const;

 private:
  void publishLocked(const std::uint8_t* nv12, std::size_t bytes, int width, int height, int64_t programSequence);

  mutable std::mutex mutex_;
  std::unique_ptr<IVirtualCameraSink> sink_;
  std::function<int64_t()> hostClockNs_;
  VirtualCameraStatus status_;
  bool connected_ = false;
  bool mirror_ = false;
  bool warnedCpuProgramFrame_ = false;
  int64_t lastProgramSequence_ = 0;
  std::uint64_t sequenceGaps_ = 0;
  std::uint64_t queueFullDrops_ = 0;
};

}  // namespace corevideo::modules
