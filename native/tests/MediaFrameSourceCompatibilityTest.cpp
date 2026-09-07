#include "modules/Interfaces.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <filesystem>
#include <fstream>
#include <cstdio>

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
    frames = source->pollMediaFrames({layer}, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
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
    auto next = source->pollMediaFrames({layer}, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
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
      auto next = source->pollMediaFrames({layer}, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
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

TEST(MediaFrameSourceCompatibility, DecodedAudioUsesCanonicalLayerRoutingIdentity) {
#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_MF_ENCODER
  // A tiny real PCM WAV exercises the MF decoder without an external fixture,
  // ffmpeg process, or a mock that could duplicate the identity bug.
  const auto path = std::filesystem::temp_directory_path() / ("corevideo-audio-identity-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); } } cleanup{path};
  {
    std::ofstream file(path, std::ios::binary);
    const auto word = [&](uint32_t value, int bytes) {
      for (int n = 0; n < bytes; ++n) file.put(static_cast<char>((value >> (n * 8)) & 255));
    };
    constexpr uint32_t samples = 4800, dataBytes = samples * 2;
    file.write("RIFF", 4); word(36 + dataBytes, 4); file.write("WAVEfmt ", 8);
    word(16, 4); word(1, 2); word(1, 2); word(48000, 4); word(96000, 4); word(2, 2); word(16, 2);
    file.write("data", 4); word(dataBytes, 4);
    for (uint32_t n = 0; n < samples; ++n) word(n % 48 < 24 ? 8192 : static_cast<uint16_t>(-8192), 2);
    ASSERT_TRUE(file.good());
  }
  auto source = corevideo::modules::createMediaFoundationMediaFrameSource();
  ASSERT_TRUE(source != nullptr);
  corevideo::modules::CompositorRenderPlanLayer explicitLayer;
  explicitLayer.kind = "media-video"; explicitLayer.mediaAssetId = "asset-one";
  explicitLayer.sourceId = "media:custom-route"; explicitLayer.mediaAssetPath = path.string();
  explicitLayer.mediaAssetPlaying = true;
  auto fallbackLayer = explicitLayer;
  fallbackLayer.sourceId.clear(); fallbackLayer.mediaAssetId = "asset-two";
  std::vector<corevideo::modules::AudioFrame> frames;
  bool sawExplicit = false, sawFallback = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while ((!sawExplicit || !sawFallback) && std::chrono::steady_clock::now() < deadline) {
    for (auto& frame : source->pollMediaAudioFrames({explicitLayer, fallbackLayer}, 100)) {
      if (frame.participantId == "media:custom-route" && !sawExplicit) { frames.push_back(frame); sawExplicit = true; }
      if (frame.participantId == "media:asset-two" && !sawFallback) { frames.push_back(frame); sawFallback = true; }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(sawExplicit && sawFallback);
  if (frames.size() != 2u) return;
  if (frames[0].participantId != "media:custom-route") std::swap(frames[0], frames[1]);
  EXPECT_EQ(frames[0].participantId, "media:custom-route");
  EXPECT_EQ(frames[1].participantId, "media:asset-two");
  for (const auto& frame : frames) {
    EXPECT_TRUE(frame.sampleCount > 0);
    EXPECT_TRUE(!frame.pcm.empty());
    bool nonSilent = false;
    for (const auto sample : frame.pcm) nonSilent = nonSilent || sample > 0.01f || sample < -0.01f;
    EXPECT_TRUE(nonSilent);
  }
#endif
}

TEST(MediaFrameSourceCompatibility, ConfiguredFlashFixtureKeepsAllVideoPulseEdgesAtSourceCadence) {
#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_MF_ENCODER
  const auto* fixture = std::getenv("COREVIDEO_TEST_FLASH_BEEP");
  if (!fixture || !*fixture) return; // Explicit real1080p60 H264 fixture from the recorded A/V harness.
  auto source = corevideo::modules::createMediaFoundationMediaFrameSource();
  ASSERT_TRUE(source != nullptr);
  corevideo::modules::CompositorRenderPlanLayer layer;
  layer.kind = "media-video"; layer.sourceId = "media:flash-probe"; layer.mediaAssetId = "flash-probe";
  layer.mediaAssetPath = fixture; layer.mediaAssetPlaying = true; layer.mediaPlaybackKey = "source-edge-proof";
  const auto anchor = std::chrono::steady_clock::now();
  std::vector<double> starts, widths;
  bool white = false;
  for (int tick = 0; tick < 540; ++tick) {
    const auto scheduled = anchor + std::chrono::nanoseconds(tick * 1000000000LL / 60);
    std::this_thread::sleep_until(scheduled);
    const auto pts = std::chrono::duration_cast<std::chrono::nanoseconds>(scheduled.time_since_epoch()).count() / 100;
    const auto frames = source->pollMediaFramesAt100ns({layer}, pts);
    if (frames.empty()) continue;
    const auto& frame = frames.front();
    ASSERT_TRUE(frame.hasPixels());
    const auto center = static_cast<size_t>(frame.pixelHeight / 2) * frame.pixelStride + static_cast<size_t>(frame.pixelWidth / 2) * 4;
    const bool currentWhite = (*frame.pixels)[center] >= 180;
    const double elapsed = tick / 60.0;
    if (currentWhite && !white) starts.push_back(elapsed);
    if (!currentWhite && white) widths.push_back(elapsed - starts.back());
    white = currentWhite;
  }
  const double relativeStarts[] = {0, 0.5, 1.2, 2.0, 2.9, 3.9, 5.0, 6.2};
  for (size_t i = 0; i < starts.size(); ++i)
    std::fprintf(stderr, "[media-source-edge] pulse=%zu onset=%.6f width=%.6f\n", i, starts[i], i < widths.size() ? widths[i] : -1.0);
  std::fprintf(stderr, "[media-source-edge] onset_count=%zu width_count=%zu\n", starts.size(), widths.size());
  EXPECT_EQ(starts.size(), 8u); EXPECT_EQ(widths.size(), 8u);
  for (size_t i = 0; i < starts.size() && i < 8; ++i) {
    EXPECT_TRUE(std::abs((starts[i] - starts[0]) - relativeStarts[i]) <= 1.0 / 60 + 0.000001)
        << "pulse=" << i << " relative_onset=" << starts[i] - starts[0];
  }
  for (size_t i = 0; i < widths.size() && i < 8; ++i) {
    EXPECT_TRUE(std::abs(widths[i] - (i + 1) * 0.1) <= 1.0 / 60 + 0.000001)
        << "pulse=" << i << " width=" << widths[i];
  }
  EXPECT_TRUE(source->warnings().empty());
#endif
}
