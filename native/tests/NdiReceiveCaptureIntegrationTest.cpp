#include <gtest/gtest.h>

#include "modules/Interfaces.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace corevideo::modules;

namespace {

class VideoSink final : public ICaptureVideoConsumer {
 public:
  void publish(VideoFrame frame) override {
    if (frame.hasPixels()) {
      sourceId = frame.participantId;
      width = frame.pixelWidth;
      firstPixel = {(*frame.pixels)[0], (*frame.pixels)[1], (*frame.pixels)[2]};
      ++frames;
    }
  }
  void end(const std::string&) override {}
  std::string sourceId;
  int width = 0;
  int frames = 0;
  std::vector<uint8_t> firstPixel;
};

class AudioSink final : public ICaptureAudioConsumer {
 public:
  void publish(AudioFrame frame) override {
    if (!frame.pcm.empty()) {
      sourceId = frame.participantId;
      channels = frame.channels;
      samples += frame.sampleCount;
    }
  }
  std::string sourceId;
  int channels = 0;
  int samples = 0;
};

}  // namespace

TEST(NdiReceiveCapture, DiscoversProductSenderAndDeliversVideoAndAudioToCaptureBus) {
#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && \
    COREVIDEO_WITH_NDI_INGEST && COREVIDEO_WITH_NDI_OUTPUT
  auto sender = createNdiOutputSender();
  auto receiver = createNdiReceiveCaptureDevice();
  const bool requireHardware = std::getenv("COREVIDEO_REQUIRE_NDI_TEST") != nullptr;
  if (!sender || !sender->runtimeAvailableAtConstruction() || !receiver) {
    if (requireHardware) {
      EXPECT_TRUE(false) << "NDI runtime or receive adapter unavailable; this is missing evidence.";
    }
    return;
  }

  OutputDestinationSettings settings;
  settings.id = "ndi";
  settings.protocol = "ndi";
  settings.ndiName = "CVPReceiveTest";
  settings.fps = 30;

  ProgramFrame frame;
  frame.width = frame.preview.width = 320;
  frame.height = frame.preview.height = 180;
  frame.preview.bgra.resize(320u * 180u * 4u);
  for (size_t i = 0; i < frame.preview.bgra.size(); i += 4) {
    frame.preview.bgra[i] = 25;
    frame.preview.bgra[i + 1] = 90;
    frame.preview.bgra[i + 2] = 210;
    frame.preview.bgra[i + 3] = 255;
  }
  std::vector<float> pcm(480u * 2u, 0.125f);
  VideoSink video;
  AudioSink audio;
  std::string id;
  std::string seenNames;
  std::string lastSenderResult;

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < deadline &&
         (video.frames == 0 || audio.samples == 0)) {
    ++frame.frameNumber;
    const auto session = sender->sync({"ndi"}, &frame, frame.frameNumber * 33.3, {settings}, &pcm, 2, 48000);
    if (!session.senders.empty()) lastSenderResult = session.senders.front().lastResultCode;
    for (const auto& device : receiver->enumerate()) {
      if (seenNames.find(device.name) == std::string::npos) seenNames += device.name + "; ";
      if (device.name.find("CVPReceiveTest") != std::string::npos) {
        id = device.id;
        receiver->connect(id);
        break;
      }
    }
    receiver->deliverVideo(video, frame.frameNumber * 33);
    receiver->deliverAudio(audio, frame.frameNumber * 33);
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }

  ASSERT_FALSE(id.empty()) << "Product NDI sender was never discovered. Seen: " << seenNames
                           << " Sender: " << lastSenderResult;
  EXPECT_GT(video.frames, 0);
  EXPECT_EQ(video.sourceId, "capture:" + id);
  EXPECT_EQ(video.width, 320);
  EXPECT_EQ(video.firstPixel.size(), 3u);
  if (video.firstPixel.size() == 3u) EXPECT_GT(video.firstPixel[2], video.firstPixel[0]);
  EXPECT_GT(audio.samples, 0);
  EXPECT_EQ(audio.sourceId, "capture:" + id);
  EXPECT_EQ(audio.channels, 2);

  sender->sync({}, nullptr, frame.frameNumber * 33.3 + 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(1700));
  const auto statuses = receiver->enumerate();
  const auto stopped = std::find_if(statuses.begin(), statuses.end(), [&](const auto& item) { return item.id == id; });
  ASSERT_NE(stopped, statuses.end());
  EXPECT_FALSE(stopped->signalPresent);
  EXPECT_EQ(stopped->connectionState, "stalled");
  receiver->disconnect(id);
#endif
}
