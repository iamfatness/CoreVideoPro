#pragma once
#include "modules/VirtualCameraPublisher.h"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace corevideo::modules {
// All backend calls, including status(), run on one lifecycle worker. Command
// and render callers only touch desired state and a bounded latest-frame slot.
class AsyncVirtualCameraPublisher final : public IVirtualCameraPublisher {
 public:
  explicit AsyncVirtualCameraPublisher(std::unique_ptr<IVirtualCameraPublisher> backend);
  ~AsyncVirtualCameraPublisher() override;
  bool start(int width, int height, int fps) override;
  void stop() override;
  void setMirror(bool mirror) override;
  void setFramerEnabled(bool enabled) override;
  void setDeviceName(const std::string& name) override;
  VirtualCameraStatus status() const override;
  void publish(const ProgramFrame& frame) override;
  void publishNv12(const uint8_t* bytes, int width, int height) override;
  void publishNv12Shared(std::shared_ptr<const std::vector<uint8_t>> bytes, int width, int height) override;
  void publishNv12Identified(std::shared_ptr<const std::vector<uint8_t>> bytes,
      int width, int height, int64_t programSequence, int64_t deliveredAt100ns) override;
 private:
  void run();
  std::unique_ptr<IVirtualCameraPublisher> backend_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool shutdown_ = false, desiredOn_ = false, mirror_ = false;
  bool framerEnabled_ = false;
  uint64_t revision_ = 0;
  uint64_t framesAccepted_ = 0, pendingFramesReplaced_ = 0, publicationExceptions_ = 0;
  VirtualCameraStatus cached_;
  std::string name_ = "CoreVideo Pro Camera";
  int width_ = 1920, height_ = 1080, fps_ = 60;
  std::optional<ProgramFrame> frame_;
  std::shared_ptr<const std::vector<uint8_t>> nv12_;
  int frameWidth_ = 0, frameHeight_ = 0;
  int64_t pendingProgramSequence_ = 0, pendingDeliveredAt100ns_ = 0;
  std::thread worker_;
};
}
