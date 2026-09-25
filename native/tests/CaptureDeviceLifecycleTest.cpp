#include "modules/Interfaces.h"

#include <string>
#include <vector>

#ifdef COREVIDEO_USE_SYSTEM_GTEST
#include <gtest/gtest.h>
#else
#include "gtest/gtest.h"
#endif

namespace {

class RecordingLifecycle final : public corevideo::modules::ICaptureDevice {
 public:
  std::vector<corevideo::modules::CaptureDeviceInfo> enumerate() const override { return devices_; }
  std::vector<corevideo::modules::CaptureDeviceInfo> selectInput(const std::string& deviceId,
                                                                 const std::string& inputId) override {
    lastSelect_ = deviceId + ":" + inputId;
    return enumerate();
  }
  std::vector<corevideo::modules::CaptureDeviceInfo> setAudioSyncOffset(const std::string& deviceId,
                                                                        int offsetMs) override {
    lastOffset_ = deviceId + ":" + std::to_string(offsetMs);
    return enumerate();
  }
  std::vector<corevideo::modules::CaptureDeviceInfo> connect(const std::string& deviceId) override {
    lastConnect_ = deviceId;
    return enumerate();
  }
  void registerCaptureBuffer(const std::string& deviceId, const std::string& shmName, int width, int height) override {
    registered_ = deviceId + "|" + shmName + "|" + std::to_string(width) + "x" + std::to_string(height);
  }
  void unregisterCaptureBuffer(const std::string& deviceId) override { unregistered_ = deviceId; }

  void captureVideoTick(int64_t) override { replaceVideo(frames); }

  std::string lastSelect_, lastOffset_, lastConnect_, registered_, unregistered_;
  std::vector<corevideo::modules::CaptureDeviceInfo> devices_{{"cam-1", "Camera"}};
  std::vector<corevideo::modules::VideoFrame> frames;
};

}  // namespace

TEST(CaptureDeviceLifecycle, SessionCommandsDoNotRequireThePollInterface) {
  RecordingLifecycle device;
  corevideo::modules::ICaptureDeviceLifecycle& lifecycle = device;
  EXPECT_EQ(lifecycle.enumerate().size(), 1u);
  EXPECT_EQ(lifecycle.selectInput("cam-1", "hdmi-1").size(), 1u);
  EXPECT_EQ(device.lastSelect_, "cam-1:hdmi-1");
  EXPECT_EQ(lifecycle.setAudioSyncOffset("cam-1", -12).size(), 1u);
  EXPECT_EQ(device.lastOffset_, "cam-1:-12");
  EXPECT_EQ(lifecycle.connect("cam-1", "capture:shell").size(), 1u);
  EXPECT_EQ(device.lastConnect_, "cam-1");
  lifecycle.registerCaptureBuffer("cam-1", "Local\\cvp", 1280, 720);
  EXPECT_EQ(device.registered_, "cam-1|Local\\cvp|1280x720");
  lifecycle.unregisterCaptureBuffer("cam-1");
  EXPECT_EQ(device.unregistered_, "cam-1");
  EXPECT_TRUE(lifecycle.disconnect("cam-1").size() == 1u);
}

TEST(CaptureDeviceLifecycle, DeliverVideoPublishesAndEndsADroppedPicture) {
  RecordingLifecycle device;
  device.frames.push_back({});
  device.frames.back().participantId = "capture:cam-1";
  device.frames.back().frameId = 1;
  class Consumer final : public corevideo::modules::ICaptureVideoConsumer {
   public:
    void publish(corevideo::modules::VideoFrame frame) override { published.push_back(frame.participantId); }
    void end(const std::string& participantId) override { ended.push_back(participantId); }
    std::vector<std::string> published, ended;
  } consumer;
  device.deliverVideo(consumer, 10);
  EXPECT_EQ(consumer.published.size(), 1u);
  EXPECT_TRUE(consumer.ended.empty());
  device.frames.clear();
  device.deliverVideo(consumer, 20);
  EXPECT_EQ(consumer.ended.size(), 1u);
  EXPECT_EQ(consumer.ended.front(), "capture:cam-1");
}

TEST(CaptureDeviceLifecycle, DeliverAudioPublishesEmbeddedPackets) {
  class Transport final : public corevideo::modules::ICaptureDevice {
   public:
    std::vector<corevideo::modules::CaptureDeviceInfo> enumerate() const override { return {}; }
    std::vector<corevideo::modules::CaptureDeviceInfo> selectInput(const std::string&, const std::string&) override { return {}; }
    std::vector<corevideo::modules::CaptureDeviceInfo> setAudioSyncOffset(const std::string&, int) override { return {}; }
    std::vector<corevideo::modules::CaptureDeviceInfo> connect(const std::string&) override { return {}; }
    void captureAudioTick(int64_t stamp) override {
      corevideo::modules::AudioFrame frame;
      frame.participantId = "capture:srt-1";
      frame.timestampMs = stamp;
      frame.sampleCount = 1;
      postAudio(std::move(frame));
    }
  } transport;
  class Consumer final : public corevideo::modules::ICaptureAudioConsumer {
   public:
    void publish(corevideo::modules::AudioFrame frame) override {
      ids.push_back(frame.participantId);
      stamps.push_back(frame.timestampMs);
    }
    std::vector<std::string> ids;
    std::vector<int64_t> stamps;
  } consumer;
  transport.deliverAudio(consumer, 40);
  ASSERT_EQ(consumer.ids.size(), 1u);
  EXPECT_EQ(consumer.ids.front(), "capture:srt-1");
  EXPECT_EQ(consumer.stamps.front(), 40);
  transport.deliverAudio(consumer, 50);
  EXPECT_EQ(consumer.ids.size(), 2u);
}

TEST(CaptureDeviceLifecycle, ShellBufferRegistrationIsANoOpUnlessOverridden) {
  class Bare final : public corevideo::modules::ICaptureDevice {
   public:
    std::vector<corevideo::modules::CaptureDeviceInfo> enumerate() const override { return {}; }
    std::vector<corevideo::modules::CaptureDeviceInfo> selectInput(const std::string&, const std::string&) override {
      return {};
    }
    std::vector<corevideo::modules::CaptureDeviceInfo> setAudioSyncOffset(const std::string&, int) override {
      return {};
    }
    std::vector<corevideo::modules::CaptureDeviceInfo> connect(const std::string&) override { return {}; }
  } bare;
  corevideo::modules::ICaptureDeviceLifecycle& lifecycle = bare;
  lifecycle.registerCaptureBuffer("cam-1", "shm", 16, 16);
  lifecycle.unregisterCaptureBuffer("cam-1");
  EXPECT_TRUE(lifecycle.enumerate().empty());
}
