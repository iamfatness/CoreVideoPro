#include "modules/Interfaces.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

TEST(MediaFrameSourceCompatibility, DecodesConfiguredProductionMov) {
#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_MF_ENCODER
  const char* configuredPath = std::getenv("COREVIDEO_TEST_PRODUCTION_MOV");
  if (configuredPath == nullptr || *configuredPath == '\0') {
    return;  // Opt-in integration evidence; unit/CI machines need no media fixture.
  }

  auto source = corevideo::modules::createMediaFoundationMediaFrameSource();
  ASSERT_TRUE(source != nullptr);
  corevideo::modules::CompositorRenderPlanLayer layer;
  layer.layerId = "production-mov";
  layer.kind = "media-video";
  layer.sourceId = "media:production-mov";
  layer.mediaAssetId = "production-mov";
  layer.mediaAssetName = "Production MOV";
  layer.mediaAssetKind = "stinger";
  layer.mediaAssetPath = configuredPath;
  layer.mediaPlaybackKey = "compatibility-test:1";
  layer.mediaAssetPlaying = true;
  layer.mediaAssetLoop = true;

  std::vector<corevideo::modules::VideoFrame> frames;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (frames.empty() && std::chrono::steady_clock::now() < deadline) {
    frames = source->pollMediaFrames({layer}, 0);
    if (frames.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  ASSERT_TRUE(!frames.empty());
  EXPECT_TRUE(frames.front().hasPixels());
  EXPECT_EQ(frames.front().pixelWidth, 1920);
  EXPECT_EQ(frames.front().pixelHeight, 1080);
  EXPECT_EQ(frames.front().pixelStride, 1920 * 4);
  EXPECT_TRUE(source->warnings().empty());

  const auto firstFrameId = frames.front().frameId;
  const auto advanceDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (frames.front().frameId <= firstFrameId &&
         std::chrono::steady_clock::now() < advanceDeadline) {
    auto next = source->pollMediaFrames({layer}, 33);
    if (!next.empty()) {
      frames = std::move(next);
    }
    if (frames.front().frameId <= firstFrameId) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  EXPECT_GT(frames.front().frameId, firstFrameId);

  // A caller can supply the fixture's frame count to turn this smoke test
  // into a real loop-boundary proof. The check remains opt-in because large
  // production backdrops can take several seconds of wall time at -re speed.
  const char* configuredLoopFrameCount =
      std::getenv("COREVIDEO_TEST_PRODUCTION_MOV_FIRST_LOOP_FRAMES");
  if (configuredLoopFrameCount != nullptr && *configuredLoopFrameCount != '\0') {
    const auto firstLoopFrameCount = std::strtoull(configuredLoopFrameCount, nullptr, 10);
    ASSERT_TRUE(firstLoopFrameCount > 0);
    const auto loopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (frames.front().frameId <= firstLoopFrameCount &&
           std::chrono::steady_clock::now() < loopDeadline) {
      auto next = source->pollMediaFrames({layer}, 33);
      if (!next.empty()) {
        frames = std::move(next);
      }
      if (frames.front().frameId <= firstLoopFrameCount) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    EXPECT_GT(frames.front().frameId, firstLoopFrameCount);
    EXPECT_TRUE(source->warnings().empty());
  }

  // This fixture is a video-only production backdrop. Lack of an audio stream
  // is valid media, not an operator warning.
  const auto audio = source->pollMediaAudioFrames({layer}, 33);
  EXPECT_TRUE(audio.empty());
  EXPECT_TRUE(source->warnings().empty());
#endif
}
