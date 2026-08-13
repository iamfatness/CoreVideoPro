// CoreAudio adapter tests. Compiled only under COREVIDEO_WITH_COREAUDIO.
// Failure-path tests are headless-safe (no device needed — they assert the
// truthful-metrics-row contract); the live-device tests skip with a log when
// no default output/input device exists (CI runners are usually deviceless),
// using the repo's skip idiom (the vendored gtest has no GTEST_SKIP).

#if COREVIDEO_WITH_COREAUDIO

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "modules/Interfaces.h"

namespace {

using namespace corevideo::modules;

CaptureAudioSourceConfig sourceConfig(const std::string& captureId, const std::string& audioId,
                                      const std::string& kind) {
  CaptureAudioSourceConfig config;
  config.captureDeviceId = captureId;
  config.audioDeviceId = audioId;
  config.audioDeviceName = audioId;
  config.audioSourceKind = kind;
  config.nativeAudioDeviceId = audioId;
  return config;
}

TEST(CoreAudioAdapters, FactoriesAreLiveUnderTheGate) {
  EXPECT_NE(createCoreAudioMonitorOutput(), nullptr);
  EXPECT_NE(createCoreAudioCaptureSource(), nullptr);
}

TEST(CoreAudioAdapters, UnsupportedKindGetsTruthfulMetricsRow) {
  auto capture = createCoreAudioCaptureSource();
  ASSERT_NE(capture, nullptr);
  capture->configure({sourceConfig("cam1", "asio-dev", "asio-input")});
  const auto metrics = capture->metrics();
  ASSERT_EQ(metrics.size(), 1u);
  EXPECT_EQ(metrics[0].captureDeviceId, "cam1");
  EXPECT_EQ(metrics[0].sourceId, "capture:cam1");  // the ISO-3 pairing key
  EXPECT_FALSE(metrics[0].streaming);
  EXPECT_NE(metrics[0].lastError.find("not supported"), std::string::npos);
  EXPECT_FALSE(capture->warnings().empty());
}

TEST(CoreAudioAdapters, LoopbackDefaultSentinelFailsWithVirtualDeviceExplanation) {
  auto capture = createCoreAudioCaptureSource();
  ASSERT_NE(capture, nullptr);
  capture->configure({sourceConfig("local-machine-audio", "default-render", "wasapi-loopback")});
  const auto metrics = capture->metrics();
  ASSERT_EQ(metrics.size(), 1u);
  // local-machine-audio passes through VERBATIM (never capture:-prefixed).
  EXPECT_EQ(metrics[0].sourceId, "local-machine-audio");
  EXPECT_FALSE(metrics[0].streaming);
  EXPECT_NE(metrics[0].lastError.find("BlackHole"), std::string::npos);
}

TEST(CoreAudioAdapters, UnknownDeviceUidFailsLoudlyButKeepsTheRow) {
  auto capture = createCoreAudioCaptureSource();
  ASSERT_NE(capture, nullptr);
  capture->configure(
      {sourceConfig("cam2", "no-such-device-uid-12345", "coreaudio-input")});
  const auto metrics = capture->metrics();
  ASSERT_EQ(metrics.size(), 1u);
  EXPECT_FALSE(metrics[0].streaming);
  EXPECT_NE(metrics[0].lastError.find("was not found"), std::string::npos);
  EXPECT_TRUE(capture->pollAudioFrames(0).empty());
}

TEST(CoreAudioAdapters, MonitorRendersIntoDefaultOutputWhenPresent) {
  auto monitor = createCoreAudioMonitorOutput();
  ASSERT_NE(monitor, nullptr);
  EXPECT_FALSE(monitor->render(nullptr, 0, 2, 1.0));  // render before start refuses
  if (!monitor->start("", 48000, 2)) {
    std::fprintf(stderr, "[coreaudio-test] skipping: no default output device\n");
    return;
  }
  EXPECT_TRUE(monitor->active());
  EXPECT_TRUE(monitor->hardwareOutput());
  EXPECT_FALSE(monitor->resolvedEndpointId().empty());
  // Idempotent re-start on the identical device.
  EXPECT_TRUE(monitor->start("", 48000, 2));
  std::vector<float> tone(960 * 2, 0.f);
  for (size_t i = 0; i < tone.size(); i += 2) {
    tone[i] = tone[i + 1] = 0.05f * static_cast<float>((i % 96) - 48) / 48.f;
  }
  EXPECT_TRUE(monitor->render(tone.data(), 960, 2, 0.5));
  monitor->stop();
  EXPECT_FALSE(monitor->active());
  monitor->stop();  // idempotent
}

TEST(CoreAudioAdapters, MonitorUnknownDeviceFallsBackToDefaultWithWarning) {
  auto monitor = createCoreAudioMonitorOutput();
  ASSERT_NE(monitor, nullptr);
  if (!monitor->start("no-such-monitor-uid", 48000, 2)) {
    std::fprintf(stderr, "[coreaudio-test] skipping: no default output device\n");
    return;
  }
  EXPECT_FALSE(monitor->warnings().empty());
  monitor->stop();
}

}  // namespace

#endif  // COREVIDEO_WITH_COREAUDIO
