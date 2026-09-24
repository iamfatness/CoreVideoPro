#include "compositor/CompositorLayout.h"
#include "core/AudioControlSourcePolicy.h"
#include "core/BoundedAsyncLog.h"
#include "core/StreamBackpressurePolicy.h"
#include "core/MediaCore.h"

#include "EncoderCapacityProbeTestSupport.h"
#include "MediaTestSupport.h"
#include "modules/AudioDsp.h"
#include "modules/Interfaces.h"
#include "modules/ProgramFramePreview.h"
#include "modules/RealZoomCaptureSource.h"
#include "modules/ZoomMeetingSdkAdapter.h"
#include "core/TestPatternSource.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <chrono>
#include <set>
#include <sstream>
#include <stdexcept>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {

bool jsonArrayContains(const corevideo::rpc::Json& array, const std::string& value) {
  return std::any_of(array.asArray().begin(), array.asArray().end(), [&](const corevideo::rpc::Json& entry) {
    return entry.isString() && entry.asString() == value;
  });
}

const corevideo::rpc::Json* findParticipantMix(const corevideo::rpc::Json& mix, const std::string& participantId) {
  const auto* participants = mix.get("participants");
  if (!participants || !participants->isArray()) {
    return nullptr;
  }
  const auto& rows = participants->asArray();
  const auto found = std::find_if(rows.begin(), rows.end(), [&](const corevideo::rpc::Json& row) {
    return row.getString("participantId") == participantId;
  });
  return found == rows.end() ? nullptr : &(*found);
}

corevideo::modules::ProgramFrame makeTestProgramFrame(int64_t frameNumber) {
  corevideo::modules::ProgramFrame frame{1920, 1080, 2, frameNumber, "mp4-plan", "d3d11"};
  frame.preview.width = frame.width;
  frame.preview.height = frame.height;
  frame.preview.bgra.assign(static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 4u, 0);
  for (int y = 0; y < frame.height; ++y) {
    for (int x = 0; x < frame.width; ++x) {
      const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(frame.width) + static_cast<size_t>(x)) * 4u;
      frame.preview.bgra[offset + 0] = static_cast<uint8_t>(x % 256);
      frame.preview.bgra[offset + 1] = static_cast<uint8_t>(y % 256);
      frame.preview.bgra[offset + 2] = 0x40;
      frame.preview.bgra[offset + 3] = 0xff;
    }
  }
  return frame;
}

// Emits one stereo PCM audio frame per poll so the monitor path has a real
// signal to render through an injected monitor output (the default synthetic
// source is metadata-only).
class PcmTestZoomSource final : public corevideo::modules::IZoomCaptureSource {
 public:
  std::vector<corevideo::modules::VideoFrame> pollVideoFrames() override {
    corevideo::modules::VideoFrame frame;
    frame.participantId = "pcm-speaker";
    frame.width = 1280;
    frame.height = 720;
    frame.naturalWidth = 1280;
    frame.naturalHeight = 720;
    frame.timestampMs = ++tick_ * 16;
    return {frame};
  }
  std::vector<corevideo::modules::AudioFrame> pollAudioFrames() override {
    corevideo::modules::AudioFrame frame;
    frame.participantId = "pcm-speaker";
    frame.sampleRate = 48000;
    frame.channels = 2;
    frame.sampleCount = 480;
    frame.pcm.assign(static_cast<size_t>(frame.sampleCount) * frame.channels, 0.5f);
    return {frame};
  }

 private:
  int64_t tick_ = 0;
};

class QuietPcmTestZoomSource final : public corevideo::modules::IZoomCaptureSource {
 public:
  std::vector<corevideo::modules::VideoFrame> pollVideoFrames() override {
    corevideo::modules::VideoFrame frame;
    frame.participantId = "quiet-speaker";
    frame.width = 1280;
    frame.height = 720;
    frame.naturalWidth = 1280;
    frame.naturalHeight = 720;
    frame.timestampMs = ++tick_ * 16;
    return {frame};
  }

  std::vector<corevideo::modules::AudioFrame> pollAudioFrames() override {
    corevideo::modules::AudioFrame frame;
    frame.participantId = "quiet-speaker";
    frame.sampleRate = 48000;
    frame.channels = 2;
    frame.sampleCount = 480;
    frame.pcm.assign(static_cast<size_t>(frame.sampleCount) * frame.channels, 0.04f);
    return {frame};
  }

 private:
  int64_t tick_ = 0;
};

class SinePcmTestZoomSource final : public corevideo::modules::IZoomCaptureSource {
 public:
  std::vector<corevideo::modules::VideoFrame> pollVideoFrames() override {
    corevideo::modules::VideoFrame frame;
    frame.participantId = "pcm-speaker";
    frame.width = 1280;
    frame.height = 720;
    frame.naturalWidth = 1280;
    frame.naturalHeight = 720;
    frame.timestampMs = ++tick_ * 16;
    return {frame};
  }

  std::vector<corevideo::modules::AudioFrame> pollAudioFrames() override {
    corevideo::modules::AudioFrame frame;
    frame.participantId = "pcm-speaker";
    frame.sampleRate = 48000;
    frame.channels = 2;
    frame.sampleCount = 480;
    frame.pcm.resize(static_cast<size_t>(frame.sampleCount) * frame.channels);
    const int64_t baseSample = tick_ * frame.sampleCount;
    for (int index = 0; index < frame.sampleCount; ++index) {
      const double phase = 2.0 * corevideo::modules::kAudioPi * 440.0 *
                           static_cast<double>(baseSample + index) / 48000.0;
      const auto sample = static_cast<float>(0.25 * std::sin(phase));
      frame.pcm[static_cast<size_t>(index) * 2] = sample;
      frame.pcm[static_cast<size_t>(index) * 2 + 1] = sample;
    }
    return {frame};
  }

 private:
  int64_t tick_ = 0;
};

// #535 slice 3b: the module set carries a decoder FACTORY, so MediaTransports
// builds ONE of these per media source, on that source's own worker thread. A
// test can no longer hold the single instance it injected, so everything a test
// reads is STATIC and mutex-guarded, and reset() is called by every test that
// reads it. `seenSourceIds` is first-seen order with no duplicates and
// `seenLoops` carries the LATEST loop flag per id, because a worker polls its
// own layer over and over rather than the whole plan once per tick.
class SolidMediaFrameSource final : public corevideo::modules::IMediaFrameSource {
 public:
  SolidMediaFrameSource() { ++created; }

  static std::mutex& seenMutex() {
    static std::mutex mutex;
    return mutex;
  }
  static void reset() {
    std::lock_guard<std::mutex> lock(seenMutex());
    seenSourceIds.clear();
    seenLoops.clear();
    pollCount = 0;
    audioPollCount = 0;
    created = 0;
  }
  static std::vector<std::string> sourceIds() {
    std::lock_guard<std::mutex> lock(seenMutex());
    return seenSourceIds;
  }
  static std::vector<std::string> sortedSourceIds() {
    auto ids = sourceIds();
    std::sort(ids.begin(), ids.end());
    return ids;
  }
  // The loop flag last seen for `sourceId`; false when it was never requested.
  static bool loopFor(const std::string& sourceId) {
    std::lock_guard<std::mutex> lock(seenMutex());
    const auto found = seenLoops.find(sourceId);
    return found != seenLoops.end() && found->second;
  }

  std::vector<corevideo::modules::VideoFrame> pollMediaFrames(
      const std::vector<corevideo::modules::CompositorRenderPlanLayer>& layers,
      int64_t timestampMs) override {
    const int64_t poll = ++pollCount;
    std::vector<corevideo::modules::VideoFrame> frames;
    for (const auto& layer : layers) {
      if (layer.mediaAssetId.empty() || layer.mediaAssetPath.empty()) {
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(seenMutex());
        const auto id = layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId;
        if (std::find(seenSourceIds.begin(), seenSourceIds.end(), id) == seenSourceIds.end()) {
          seenSourceIds.push_back(id);
        }
        seenLoops[id] = layer.mediaAssetLoop;
      }
      corevideo::modules::VideoFrame frame;
      frame.participantId = layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId;
      frame.width = kWidth;
      frame.height = kHeight;
      frame.naturalWidth = kWidth;
      frame.naturalHeight = kHeight;
      frame.timestampMs = timestampMs;
      frame.pixelWidth = kWidth;
      frame.pixelHeight = kHeight;
      frame.pixelStride = kWidth * 4;
      frame.frameId = poll;
      auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4u);
      for (size_t index = 0; index < pixels->size(); index += 4) {
        (*pixels)[index + 0] = blue;
        (*pixels)[index + 1] = green;
        (*pixels)[index + 2] = red;
        (*pixels)[index + 3] = 0xff;
      }
      frame.pixels = std::move(pixels);
      frames.push_back(std::move(frame));
    }
    return frames;
  }

  std::vector<corevideo::modules::AudioFrame> pollMediaAudioFrames(
      const std::vector<corevideo::modules::CompositorRenderPlanLayer>& layers,
      int64_t timestampMs) override {
    std::vector<corevideo::modules::AudioFrame> frames;
    for (const auto& layer : layers) {
      if (layer.kind != "media-video" || layer.mediaAssetId.empty() || !layer.mediaAssetPlaying) {
        continue;
      }

      corevideo::modules::AudioFrame frame;
      // Exactly what the real decoder stamps since #408
      // (MediaFoundationMediaFrameSourceAdapter decodeLayerAudio →
      // mediaFrameSourceId): the layer's own `media:<assetId>`. This fake used
      // to emit the shell's generic "media", which is why no test saw T1.6.
      frame.participantId = layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId;
      frame.sampleRate = 48000;
      frame.channels = 2;
      frame.timestampMs = timestampMs;
      frame.sampleCount = 480;
      frame.voiceActive = true;
      frame.pcm.resize(static_cast<size_t>(frame.sampleCount) * static_cast<size_t>(frame.channels));
      const int64_t baseSample = audioPollCount * frame.sampleCount;
      for (int sampleIndex = 0; sampleIndex < frame.sampleCount; ++sampleIndex) {
        // Whole periods per 480-sample block keep independently scheduled fake
        // decoders in phase. 330 Hz shifted each block by 108 degrees, so the
        // two-clip summing test could cancel under sanitizer scheduling.
        const double phase = 2.0 * corevideo::modules::kAudioPi * 300.0 *
                             static_cast<double>(baseSample + sampleIndex) / static_cast<double>(frame.sampleRate);
        const auto sample = static_cast<float>(0.2 * std::sin(phase));
        frame.pcm[static_cast<size_t>(sampleIndex) * 2u] = sample;
        frame.pcm[static_cast<size_t>(sampleIndex) * 2u + 1u] = sample;
      }
      frames.push_back(std::move(frame));
    }
    if (!frames.empty()) {
      ++audioPollCount;
    }
    return frames;
  }

  static constexpr int kWidth = 16;
  static constexpr int kHeight = 16;
  static constexpr uint8_t blue = 0x22;
  static constexpr uint8_t green = 0xb4;
  static constexpr uint8_t red = 0xf1;
  static inline std::atomic<int64_t> pollCount{0};
  static inline std::atomic<int64_t> audioPollCount{0};
  // Decoder instances the factory built: ONE per media source id.
  static inline std::atomic<int> created{0};
  static inline std::vector<std::string> seenSourceIds;          // guarded by seenMutex()
  static inline std::map<std::string, bool> seenLoops;           // guarded by seenMutex()
};

class CapturingOutputSender final : public corevideo::modules::IOutputSender {
 public:
  corevideo::modules::OutputSenderSession sync(
      const std::vector<std::string>& destinations,
      const corevideo::modules::ProgramFrame*,
      double,
      const std::vector<corevideo::modules::OutputDestinationSettings>& destinationSettings = {},
      const std::vector<float>* programAudioPcm = nullptr,
      int audioChannels = 0,
      int audioSampleRate = 0) override {
    destinations_ = destinations;
    destinationSettings_ = destinationSettings;
    audioFramesSent_ = programAudioPcm && audioChannels > 0
                           ? static_cast<int>(programAudioPcm->size() / static_cast<size_t>(audioChannels))
                           : 0;
    audioChannels_ = audioChannels;
    audioSampleRate_ = audioSampleRate;
    firstAudioSample_ = programAudioPcm && !programAudioPcm->empty() ? programAudioPcm->front() : 0.f;
    maxAudioSample_ = 0.f;
    if (programAudioPcm) {
      for (const auto sample : *programAudioPcm) {
        maxAudioSample_ = std::max(maxAudioSample_, std::abs(sample));
      }
    }
    corevideo::modules::OutputSenderSession session;
    session.status = destinations.empty() ? "idle" : "live";
    session.activeSenderCount = static_cast<int>(destinations.size());
    return session;
  }

  corevideo::modules::OutputSenderSession fail(const std::string&, const std::string&, double) override {
    return {};
  }

  corevideo::modules::OutputSenderSession recover(const std::string&, double, const std::string&) override {
    return {};
  }

  corevideo::modules::OutputSenderSession session() const override {
    return {};
  }

  std::vector<std::string> destinations_;
  std::vector<corevideo::modules::OutputDestinationSettings> destinationSettings_;
  int audioFramesSent_ = 0;
  int audioChannels_ = 0;
  int audioSampleRate_ = 0;
  float firstAudioSample_ = 0.f;
  float maxAudioSample_ = 0.f;
};

void writeLe16(std::ofstream& stream, uint16_t value) {
  const char bytes[] = {
      static_cast<char>(value & 0xff),
      static_cast<char>((value >> 8) & 0xff),
  };
  stream.write(bytes, sizeof(bytes));
}

void writeLe32(std::ofstream& stream, uint32_t value) {
  const char bytes[] = {
      static_cast<char>(value & 0xff),
      static_cast<char>((value >> 8) & 0xff),
      static_cast<char>((value >> 16) & 0xff),
      static_cast<char>((value >> 24) & 0xff),
  };
  stream.write(bytes, sizeof(bytes));
}

void writeSineWaveFile(const std::filesystem::path& path, int sampleRate = 48000, int channels = 2) {
  constexpr int kFrames = 4800;
  constexpr int kBitsPerSample = 16;
  const int blockAlign = channels * kBitsPerSample / 8;
  const int byteRate = sampleRate * blockAlign;
  const int dataBytes = kFrames * blockAlign;

  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write("RIFF", 4);
  writeLe32(stream, 36u + static_cast<uint32_t>(dataBytes));
  stream.write("WAVE", 4);
  stream.write("fmt ", 4);
  writeLe32(stream, 16);
  writeLe16(stream, 1);
  writeLe16(stream, static_cast<uint16_t>(channels));
  writeLe32(stream, static_cast<uint32_t>(sampleRate));
  writeLe32(stream, static_cast<uint32_t>(byteRate));
  writeLe16(stream, static_cast<uint16_t>(blockAlign));
  writeLe16(stream, kBitsPerSample);
  stream.write("data", 4);
  writeLe32(stream, static_cast<uint32_t>(dataBytes));
  for (int index = 0; index < kFrames; ++index) {
    const double phase = 2.0 * corevideo::modules::kAudioPi * 440.0 * static_cast<double>(index) / sampleRate;
    const auto sample = static_cast<int16_t>(std::round(std::sin(phase) * 12000.0));
    for (int channel = 0; channel < channels; ++channel) {
      writeLe16(stream, static_cast<uint16_t>(sample));
    }
  }
}

class ThrowingOutputSender final : public corevideo::modules::IOutputSender {
 public:
  corevideo::modules::OutputSenderSession sync(
      const std::vector<std::string>&,
      const corevideo::modules::ProgramFrame*,
      double,
      const std::vector<corevideo::modules::OutputDestinationSettings>& = {},
      const std::vector<float>* = nullptr,
      int = 0,
      int = 0) override {
    throw std::runtime_error("simulated sender startup failure");
  }

  corevideo::modules::OutputSenderSession fail(const std::string& destination, const std::string& message, double elapsedMs) override {
    sender_.senderId = destination + ":program";
    sender_.destination = destination;
    sender_.status = "failed";
    sender_.stoppedAtMs = elapsedMs;
    sender_.warning = message;
    sender_.destinationHealth = "failed";
    sender_.lastResultCode = "failed";
    sender_.lastError = message;

    corevideo::modules::OutputSenderSession session;
    session.status = "degraded";
    session.activeSenderCount = 0;
    session.senders = {sender_};
    session_ = session;
    return session_;
  }

  corevideo::modules::OutputSenderSession recover(const std::string&, double, const std::string&) override {
    return session_;
  }

  corevideo::modules::OutputSenderSession session() const override {
    return session_;
  }

 private:
  corevideo::modules::OutputSender sender_;
  corevideo::modules::OutputSenderSession session_;
};

// Records what MediaCore's monitor path pushed, so a test can assert that real
// sample-frames reached the device at the operator's volume.
class RecordingMonitorOutput final : public corevideo::modules::IAudioMonitorOutput {
 public:
  bool start(const std::string& deviceId, int, int) override {
    lastDeviceId = deviceId;
    active_ = true;
    ++startCount;
    return true;
  }
  void stop() override {
    active_ = false;
    ++stopCount;
  }
  bool render(const float* interleaved, int frameCount, int channels, double volume) override {
    if (!active_ || interleaved == nullptr || frameCount <= 0) {
      return false;
    }
    framesRendered += frameCount;
    lastChannels = channels;
    lastVolume = volume;
    lastFirstSample = interleaved[0];
    ++renderCalls;
    return true;
  }
  bool active() const override { return active_; }
  bool hardwareOutput() const override { return true; }
  std::string deviceName() const override { return "Recording Monitor"; }
  std::vector<std::string> warnings() const override { return {}; }

  std::string lastDeviceId;
  int startCount = 0;
  int stopCount = 0;
  int renderCalls = 0;
  int64_t framesRendered = 0;
  int lastChannels = 0;
  double lastVolume = 0.0;
  float lastFirstSample = 0.f;

 private:
  bool active_ = false;
};

class RecoveringMonitorOutput final : public corevideo::modules::IAudioMonitorOutput {
 public:
  bool start(const std::string& deviceId, int, int) override {
    lastDeviceId = deviceId;
    active_ = true;
    return true;
  }

  void stop() override { active_ = false; }

  bool render(const float* interleaved, int frameCount, int channels, double volume) override {
    ++renderCalls;
    if (failNextRender) {
      failNextRender = false;
      return false;
    }
    if (!active_ || interleaved == nullptr || frameCount <= 0 || channels <= 0) {
      return false;
    }
    framesRendered += frameCount;
    lastVolume = volume;
    return true;
  }

  bool active() const override { return active_; }
  bool hardwareOutput() const override { return true; }
  std::string deviceName() const override { return "Recovering Monitor"; }
  std::vector<std::string> warnings() const override { return warnings_; }

  bool failNextRender = true;
  int renderCalls = 0;
  int64_t framesRendered = 0;
  double lastVolume = 0.0;
  std::string lastDeviceId;
  std::vector<std::string> warnings_;

 private:
  bool active_ = false;
};

class RecordingAudioCaptureSource final : public corevideo::modules::IAudioCaptureSource {
 public:
  void configure(const std::vector<corevideo::modules::CaptureAudioSourceConfig>& sources) override {
    ++configureCount;
    lastSources = sources;
  }

  std::vector<corevideo::modules::AudioFrame> pollAudioFrames(int64_t) override {
    return {};
  }

  std::vector<corevideo::modules::CaptureAudioSourceMetrics> metrics() const override {
    return reportedMetrics;
  }

  std::vector<std::string> warnings() const override {
    return reportedWarnings;
  }

  int configureCount = 0;
  std::vector<corevideo::modules::CaptureAudioSourceConfig> lastSources;
  std::vector<corevideo::modules::CaptureAudioSourceMetrics> reportedMetrics;
  std::vector<std::string> reportedWarnings;
};

}  // namespace

TEST(MediaCoreCommand, VerboseDiagnosticsCommandChangesTheLiveProcessMode) {
  corevideo::core::setNativeVerboseLoggingEnabled(false);
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());

  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-verbose-diagnostics"}, {"enabled", true}});
  EXPECT_TRUE(corevideo::core::nativeVerboseLoggingEnabled());

  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-verbose-diagnostics"}, {"enabled", false}});
  EXPECT_FALSE(corevideo::core::nativeVerboseLoggingEnabled());
}

TEST(MediaCoreCommand, LiveBatchAppliesSceneWithoutRenderingCatchUp) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  mediaCore.enableAudioOutputWorker();
  const auto cold = mediaCore.sessionState();
  auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "first"}}}, 60000);
  EXPECT_EQ(state.getString("sceneId"), "first");
  EXPECT_EQ(state.getNumber("programFrameCount"), cold.getNumber("programFrameCount"));
  EXPECT_EQ(state.getString("renderPlanId"), cold.getString("renderPlanId"));
  mediaCore.renderDisplayTick();
  const auto rendered = mediaCore.sessionState();
  EXPECT_EQ(rendered.getNumber("programFrameCount"), cold.getNumber("programFrameCount") + 1);
  EXPECT_EQ(rendered.getString("renderPlanId"), "first:0:0");

  state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "second"}}}, 3600000);
  EXPECT_EQ(state.getString("sceneId"), "second");
  EXPECT_EQ(state.getString("renderPlanId"), rendered.getString("renderPlanId"));
  EXPECT_EQ(state.getNumber("programFrameCount"), rendered.getNumber("programFrameCount"));
  mediaCore.renderDisplayTick();
  state = mediaCore.sessionState();
  EXPECT_EQ(state.getString("renderPlanId"), "second:0:0");
  EXPECT_EQ(state.getNumber("programFrameCount"), rendered.getNumber("programFrameCount") + 1);
}

TEST(MediaCoreCommand, RenderedProgramAttributionWaitsForCompositionAndPreservesFixedIdentity) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  mediaCore.enableAudioOutputWorker();
  auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "attribution"},
          {"routes", corevideo::rpc::Json::Array{
              corevideo::rpc::Json::Object{{"routeId", "guest"}, {"mode", "fixed"}, {"participantId", "missing-guest"}},
              corevideo::rpc::Json::Object{{"routeId", "camera"}, {"mode", "capture-input"}, {"captureDeviceId", "camera-1"}}}}},
      corevideo::rpc::Json::Object{{"type", "set-overlay-asset"}, {"overlayId", "title"}, {"text", "Title"}}});
  EXPECT_EQ(state.get("programFrame")->getString("sceneId"), "");
  EXPECT_TRUE(state.get("programFrame")->get("videoSources")->asArray().empty());
  mediaCore.renderDisplayTick();
  state = mediaCore.sessionState();
  EXPECT_EQ(state.get("programFrame")->getString("sceneId"), "attribution");
  const auto& sources = state.get("programFrame")->get("videoSources")->asArray();
  ASSERT_EQ(sources.size(), 2u);
  EXPECT_EQ(sources[0].getString("layerId"), "route:guest");
  EXPECT_EQ(sources[0].getString("sourceId"), "zoom:missing-guest");
  EXPECT_EQ(sources[0].getString("participantId"), "missing-guest");
  EXPECT_EQ(sources[1].getString("sourceId"), "capture:camera-1");
  EXPECT_EQ(sources[1].getString("participantId"), "capture:camera-1");
  const auto previous = state.get("programFrame")->get("videoSources")->stringify();
  state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "next"},
          {"routes", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
              {"routeId", "new"}, {"mode", "fixed"}, {"participantId", "next-guest"}}}}}});
  EXPECT_EQ(state.get("programFrame")->getString("sceneId"), "attribution");
  EXPECT_EQ(state.get("programFrame")->get("videoSources")->stringify(), previous);
  mediaCore.renderDisplayTick();
  state = mediaCore.sessionState();
  EXPECT_EQ(state.get("programFrame")->getString("sceneId"), "next");
  EXPECT_EQ(state.get("programFrame")->get("videoSources")->asArray().front().getString("sourceId"), "zoom:next-guest");
}

TEST(MediaCoreCommand, LiveLowerThirdAnimationAdvancesOnlyOnDisplayTicks) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  mediaCore.enableAudioOutputWorker();
  auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "set-overlay-asset"}, {"overlayId", "key:lower-third"},
          {"text", "Speaker"}, {"position", "lower-third"}, {"enabled", true},
          {"keyPhase", "building-in"}, {"buildInMs", 1000}}});
  const auto initialProgress = state.get("overlayState")->get("overlays")->asArray().front().getNumber("keyProgress");
  for (int i = 0; i < 40; ++i) {
    state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
        corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "live"}}}, 60000);
  }
  const auto& unchanged = state.get("overlayState")->get("overlays")->asArray().front();
  EXPECT_EQ(unchanged.getString("keyPhase"), "building-in");
  EXPECT_EQ(unchanged.getNumber("keyProgress"), initialProgress);
  EXPECT_EQ(state.getNumber("programFrameCount"), 0);
  mediaCore.renderDisplayTick();
  state = mediaCore.sessionState();
  EXPECT_GT(state.get("overlayState")->get("overlays")->asArray().front().getNumber("keyProgress"), initialProgress);
  for (int i = 0; i < 65; ++i) mediaCore.renderDisplayTick();
  state = mediaCore.sessionState();
  EXPECT_EQ(state.get("overlayState")->get("overlays")->asArray().front().getString("keyPhase"), "on-air");
}

TEST(MediaCoreCommand, SourceBoundLowerThirdIsSuppressedOnFirstFrameAfterTake) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  mediaCore.enableAudioOutputWorker();
  auto scene = [](const std::string& guest) {
    return corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", guest},
        {"routes", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
            {"routeId", "guest"}, {"mode", "fixed"}, {"participantId", guest}}}}};
  };
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{scene("alice"),
      corevideo::rpc::Json::Object{{"type", "set-overlay-asset"}, {"overlayId", "key"},
          {"position", "lower-third"}, {"sourceId", "zoom:alice"}, {"text", "Alice"}, {"keyPhase", "on-air"}}});
  mediaCore.renderDisplayTick();
  EXPECT_EQ(mediaCore.sessionState().get("programFrame")->getNumber("layerCount"), 2);
  EXPECT_TRUE(mediaCore.sessionState().get("overlayState")->get("overlays")->asArray().front().get("visible")->asBool());
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{scene("bob")});
  mediaCore.renderDisplayTick();
  EXPECT_EQ(mediaCore.sessionState().get("programFrame")->getNumber("layerCount"), 1);
  EXPECT_FALSE(mediaCore.sessionState().get("overlayState")->get("overlays")->asArray().front().get("visible")->asBool());
  // The desired key still exists, but cannot paint Alice over Bob. Unbound
  // legacy text remains usable as a generic title on any scene.
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "set-overlay-asset"}, {"overlayId", "key"}, {"sourceId", ""}}});
  mediaCore.renderDisplayTick();
  EXPECT_EQ(mediaCore.sessionState().get("programFrame")->getNumber("layerCount"), 2);
}

TEST(MediaCoreCommand, ReboundOverlayRequiresNewRenderedContentAndHiddenFrameIsNotProof) {
  corevideo::core::MediaCore core(corevideo::modules::createStubModules());
  core.enableAudioOutputWorker();
  (void)core.applyCommands(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"}, {"sceneId", "guests"}, {"routes", corevideo::rpc::Json::Array{
          corevideo::rpc::Json::Object{{"routeId", "a"}, {"mode", "fixed"}, {"participantId", "alice"}},
          corevideo::rpc::Json::Object{{"routeId", "b"}, {"mode", "fixed"}, {"participantId", "bob"}}}}}});
  auto key = [](const std::string& person, const std::string& phase) {
    return corevideo::rpc::Json::Object{{"type", "set-overlay-asset"}, {"overlayId", "key"},
        {"position", "lower-third"}, {"sourceId", "zoom:" + person}, {"text", person}, {"keyPhase", phase}};
  };
  auto visible = [&] {
    return core.sessionState().get("overlayState")->get("overlays")->asArray().front().get("visible")->asBool();
  };
  (void)core.applyCommands(corevideo::rpc::Json::Array{key("alice", "on-air")});
  core.renderDisplayTick();
  EXPECT_TRUE(visible());
  (void)core.applyCommands(corevideo::rpc::Json::Array{key("bob", "on-air")});
  EXPECT_FALSE(visible());
  core.renderDisplayTick();
  EXPECT_TRUE(visible());
  (void)core.applyCommands(corevideo::rpc::Json::Array{key("bob", "hidden")});
  core.renderDisplayTick();
  (void)core.applyCommands(corevideo::rpc::Json::Array{key("bob", "on-air")});
  EXPECT_FALSE(visible());
  core.renderDisplayTick();
  EXPECT_TRUE(visible());
}

TEST(MediaCoreCommand, FailedCompositionInvalidatesPriorProgramAndOverlayEvidence) {
  class MetadataOnlyCompositor final : public corevideo::modules::ICompositor {
   public:
    explicit MetadataOnlyCompositor(std::unique_ptr<corevideo::modules::ICompositor> inner) : inner_(std::move(inner)) {}
    std::string rendererName() const override { return "test"; }
    corevideo::modules::ProgramFrame render(const corevideo::modules::CompositorRenderPlan& plan,
        const std::vector<corevideo::modules::VideoFrame>& frames) override {
      auto frame = inner_->render(plan, frames);
      if (fail) { frame.preview.bgra.clear(); frame.gpuComposed = false; frame.health = "degraded"; }
      return frame;
    }
    bool fail = false;
   private:
    std::unique_ptr<corevideo::modules::ICompositor> inner_;
  };
  auto modules = corevideo::modules::createStubModules();
  auto compositor = std::make_unique<MetadataOnlyCompositor>(std::move(modules.compositor));
  auto* probe = compositor.get();
  modules.compositor = std::move(compositor);
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();
  (void)core.applyCommands(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"}, {"sceneId", "first"}, {"routes", corevideo::rpc::Json::Array{
          corevideo::rpc::Json::Object{{"routeId", "a"}, {"mode", "fixed"}, {"participantId", "alice"}}}}},
      corevideo::rpc::Json::Object{{"type", "set-overlay-asset"}, {"overlayId", "key"},
          {"position", "lower-third"}, {"sourceId", "zoom:alice"}, {"keyPhase", "on-air"}}});
  core.renderDisplayTick();
  EXPECT_EQ(core.sessionState().get("programFrame")->getString("sceneId"), "first");
  probe->fail = true;
  core.renderDisplayTick();
  const auto state = core.sessionState();
  EXPECT_EQ(state.get("programFrame")->getString("sceneId"), "");
  EXPECT_TRUE(state.get("programFrame")->get("videoSources")->asArray().empty());
  EXPECT_FALSE(state.get("overlayState")->get("overlays")->asArray().front().get("visible")->asBool());
}

TEST(MediaCoreCommand, DirectBatchRetainsSynchronousElapsedTimeRendering) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "direct"}}}, 99);
  EXPECT_EQ(state.getNumber("programFrameCount"), 3);
  EXPECT_EQ(state.getString("renderPlanId"), "direct:0:0");
}

TEST(MediaCoreCommand, AppliesSceneGraphTransformsOverlaysAndOutput) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "interview"},
          {"routes", corevideo::rpc::Json::Array{
                         corevideo::rpc::Json::Object{{"routeId", "a"}, {"mode", "fixed"}, {"audioRole", "mix"}},
                         corevideo::rpc::Json::Object{{"routeId", "b"}, {"mode", "active-speaker"}, {"audioRole", "mix"}},
                     }},
      },
      corevideo::rpc::Json::Object{
          {"type", "set-participant-transform"},
          {"participantId", "participant-1"},
          {"crop", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}},
          {"scale", 1},
      },
      corevideo::rpc::Json::Object{
          {"type", "set-overlay-asset"},
          {"overlayId", "lower-third"},
          {"text", "Speaker"},
          {"position", "lower-third"},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-program-output"},
          {"destinations", corevideo::rpc::Json::Array{"recording"}},
          {"isoParticipantIds", corevideo::rpc::Json::Array{"participant-1"}},
      },
  });

  EXPECT_EQ(state.getString("sceneId"), "interview");
  EXPECT_EQ(state.get("routeCount")->asNumber(), 2);
  EXPECT_EQ(state.get("transformCount")->asNumber(), 1);
  EXPECT_EQ(state.get("overlayCount")->asNumber(), 1);
  EXPECT_TRUE(state.get("active")->asBool());
  EXPECT_EQ(state.get("health")->getString("status"), "live");
}

TEST(MediaCoreCommand, PreservesStreamDestinationSettingsForNativeSenders) {
  auto modules = corevideo::modules::createStubModules();
  auto sender = std::make_unique<CapturingOutputSender>();
  auto* senderPtr = sender.get();
  modules.outputSender = std::move(sender);

  corevideo::core::MediaCore mediaCore(std::move(modules));
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "start-program-output"},
          {"destinations", corevideo::rpc::Json::Array{"rtmp", "srt", "ndi"}},
          {"streamOutputProfile",
           corevideo::rpc::Json::Object{
               {"width", 1920},
               {"height", 1080},
               {"fps", 30},
               {"targetBitrateMbps", 4.5},
               {"codec", "h265"}}},
          {"destinationSettings",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{},
               corevideo::rpc::Json::Object{
                   {"id", "rtmp"},
                   {"label", "RTMP"},
                   {"protocol", "rtmps"},
                   {"url", "rtmps://live.example.com/app"},
                   {"streamKey", "stream-key"},
                   {"ffmpegBinDirectory", "C:\\ffmpeg\\bin"},
                   {"encoderMode", "nvenc"}},
               corevideo::rpc::Json::Object{
                   {"id", "srt"},
                   {"label", "SRT"},
                   {"mode", "caller"},
                   {"host", "receiver.example.com"},
                   {"port", 9000},
                   {"latencyMs", 120},
                   {"latencyUs", 120000},
                   {"passphrase", "secret-passphrase"},
                   {"keyLength", 32},
                   {"streamId", "publish/live/main"}},
               corevideo::rpc::Json::Object{
                   {"id", "ndi"},
                   {"label", "NDI"},
                   {"ndiName", "CoreVideo Pro Program"},
                   {"ndiGroup", "public"}},
           }},
      },
  });

  ASSERT_TRUE(senderPtr->destinations_ == (std::vector<std::string>{"rtmp", "srt", "ndi"}));
  ASSERT_TRUE(senderPtr->destinationSettings_.size() == 3u);

  const auto& rtmp = senderPtr->destinationSettings_[0];
  EXPECT_EQ(rtmp.id, "rtmp");
  EXPECT_EQ(rtmp.protocol, "rtmps");
  EXPECT_EQ(rtmp.url, "rtmps://live.example.com/app");
  EXPECT_EQ(rtmp.streamKey, "stream-key");
  EXPECT_EQ(rtmp.ffmpegBinDirectory, "C:\\ffmpeg\\bin");
  EXPECT_EQ(rtmp.fps, 30);
  EXPECT_TRUE(std::abs(rtmp.targetBitrateMbps - 4.5) < 0.001);
  EXPECT_EQ(rtmp.videoCodec, "h265");
  EXPECT_EQ(rtmp.encoderMode, "nvenc");

  const auto& srt = senderPtr->destinationSettings_[1];
  EXPECT_EQ(srt.id, "srt");
  EXPECT_EQ(srt.mode, "caller");
  EXPECT_EQ(srt.host, "receiver.example.com");
  EXPECT_EQ(srt.port, 9000);
  EXPECT_EQ(srt.latencyMs, 120);
  EXPECT_EQ(srt.latencyUs, 120000);
  EXPECT_EQ(srt.passphrase, "secret-passphrase");
  EXPECT_EQ(srt.keyLength, 32);
  EXPECT_EQ(srt.streamId, "publish/live/main");

  const auto& ndi = senderPtr->destinationSettings_[2];
  EXPECT_EQ(ndi.id, "ndi");
  EXPECT_EQ(ndi.ndiName, "CoreVideo Pro Program");
  EXPECT_EQ(ndi.ndiGroup, "public");
}

TEST(MediaCoreCommand, StreamingSenderFailureDoesNotEscapeRenderTick) {
  auto modules = corevideo::modules::createStubModules();
  modules.outputSender = std::make_unique<ThrowingOutputSender>();

  corevideo::core::MediaCore mediaCore(std::move(modules));
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "start-program-output"},
          {"destinations", corevideo::rpc::Json::Array{"rtmp"}},
          {"destinationSettings",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{
                   {"id", "rtmp"},
                   {"label", "RTMP"},
                   {"protocol", "rtmps"},
                   {"url", "rtmps://live.example.com/app"},
                   {"streamKey", "stream-key"}},
           }},
      },
  });

  const auto* output = state.get("outputSenderSession");
  ASSERT_NE(output, nullptr);
  EXPECT_EQ(output->getString("status"), "degraded");
  const auto* senders = output->get("senders");
  ASSERT_NE(senders, nullptr);
  ASSERT_TRUE(senders->isArray());
  ASSERT_FALSE(senders->asArray().empty());
  const auto& sender = senders->asArray().front();
  EXPECT_EQ(sender.getString("destination"), "rtmp");
  EXPECT_EQ(sender.getString("status"), "failed");
  EXPECT_EQ(sender.getString("lastResultCode"), "failed");
  EXPECT_NE(sender.getString("lastError").find("simulated sender startup failure"), std::string::npos);
}

// #597 Task 6 fix round 1 (declared coverage gap, section 4 of the review):
// before this, ZERO tests in the tree touched `encoderExport` or
// `get("backpressure")` on the JSON snapshot itself - every existing test
// drives the underlying mechanism (the policy, the compositor's shed count,
// the sender's own struct) but nothing ever asserted on the ACTUAL wire keys
// Task 8's live gate reads from `/snapshot`. A key typo or a mis-nested
// object would ship silently. This test and the one below it are that
// coverage, stub-buildable (`createStubModules()`, no GPU/FFmpeg/RTMP needed)
// - they are WRITTEN AND BUILT here (they compile into the same
// corevideo-native-tests.exe the two permitted filters run from) but
// DELIBERATELY NOT RUN under either `RtmpOutputSenderBackpressure.*` or
// `StreamBackpressurePolicy.*`, per the hard testing constraint. The
// coordinator will run them (and the rest of the suite) at the first clear
// window.
//
// Mirrors `MonitorShedIntegration.TheSnapshotPublishesTheHealthyStateUnconditionally`
// (MonitorShedPolicyTest.cpp) - the precedent named for exactly this property.
TEST(MediaCoreCommand, EncoderExportIsPublishedUnconditionallyOnAFreshStubCore) {
  corevideo::core::MediaCore core(corevideo::modules::createStubModules());
  const auto state = core.sessionState();
  const auto* evidence = state.get("realtimeEvidence");
  ASSERT_NE(evidence, nullptr);
  const auto* encoderExport = evidence->get("encoderExport");
  ASSERT_NE(encoderExport, nullptr)
      << "realtimeEvidence.encoderExport must exist before any stream starts, "
         "exactly like monitorShed beside it - a divisor of 1 and 0 shed frames "
         "is the healthy READING, not an absent node";
  EXPECT_EQ(encoderExport->getNumber("divisor"), 1);
  // #597 fix round 2, item 3: EXPECT_EQ(getNumber("shedFrames"), 0) alone
  // cannot catch a typo in the key "shedFrames" - Json::getNumber's fallback
  // for a MISSING key is also 0, so a mis-spelled key and a genuinely healthy
  // reading are indistinguishable to that assertion. Null-check the key
  // itself first, the same way the `exporting` assertion right below already
  // does.
  const auto* shedFrames = encoderExport->get("shedFrames");
  ASSERT_NE(shedFrames, nullptr);
  EXPECT_EQ(shedFrames->asNumber(), 0);
  const auto* exporting = encoderExport->get("exporting");
  ASSERT_NE(exporting, nullptr);
  EXPECT_FALSE(exporting->asBool())
      << "a stub core with no compositor export activity is not exporting";
}

namespace {
// A minimal fake sender that publishes every field of OutputBackpressureState
// with a DISTINCT, non-default value, so a test reading the JSON back can
// catch a key typo (a wrong field reads a default/zero and a naive test could
// pass by accident) or a value silently dropped in the MediaCore.cpp mapping.
class FullBackpressureFieldsSender : public corevideo::modules::IOutputSender {
 public:
  corevideo::modules::OutputSenderSession sync(
      const std::vector<std::string>& /*destinations*/,
      const corevideo::modules::ProgramFrame* /*frame*/,
      double /*elapsedMs*/,
      const std::vector<corevideo::modules::OutputDestinationSettings>& /*settings*/,
      const std::vector<float>* /*pcm*/,
      int /*channels*/,
      int /*sampleRate*/) override {
    return session();
  }
  corevideo::modules::OutputSenderSession fail(const std::string&, const std::string&, double) override {
    return session();
  }
  corevideo::modules::OutputSenderSession recover(const std::string&, double, const std::string&) override {
    return session();
  }
  corevideo::modules::OutputSenderSession session() const override {
    corevideo::modules::OutputSenderSession out;
    out.status = "live";
    out.activeSenderCount = 1;
    corevideo::modules::OutputSender sender;
    sender.senderId = sender.destination = "rtmp";
    sender.status = "live";
    sender.destinationHealth = "ok";
    sender.lastResultCode = "encoder-input-accepted";
    corevideo::modules::OutputBackpressureState bp;
    bp.divisor = 3;
    bp.level = 2;
    bp.bufferedMs = 812;
    bp.queuedChunks = 47;
    bp.inFlightWriteMs = 686;
    bp.maxWriteMs = 691;
    bp.slowWriteCount = 3;
    bp.enteredCount = 5;
    bp.discardedChunks = 19;
    bp.discardEvents = 2;
    bp.lastReason = "buffered-above-threshold";
    bp.lastTransitionBufferedMs = 300;
    bp.runId = 4;
    bp.observedAtMs = 12345.0;
    sender.backpressure = bp;
    out.senders.push_back(sender);

    // FINAL-REVIEW FINDING 3: A HEALTHY SIBLING. Lever A is per-ENCODER - one
    // encoder texture feeds every GPU-direct destination - so this destination
    // asks for divisor 1 and is nonetheless FED at the max across senders (3).
    // Its node used to report a textbook-healthy `divisor: 1` and nothing else,
    // which is what an operator readout binds to.
    corevideo::modules::OutputSender sibling;
    sibling.senderId = sibling.destination = "rtmp-sibling";
    sibling.status = "live";
    sibling.destinationHealth = "ok";
    sibling.lastResultCode = "encoder-input-accepted";
    corevideo::modules::OutputBackpressureState healthy;  // every default: divisor 1, level 0
    healthy.runId = 4;
    healthy.observedAtMs = 12345.0;
    sibling.backpressure = healthy;
    out.senders.push_back(sibling);
    out.activeSenderCount = 2;
    return out;
  }
};
}  // namespace

// See the comment above EncoderExportIsPublishedUnconditionallyOnAFreshStubCore:
// same coverage gap, the per-sender half of the node. Also pins the CORRECTED
// wire path (`outputSenderSession.senders[].backpressure`) rather than the
// brief's/CLAUDE.md's stated `outputSenders.senders[]`, which does not exist
// on this core - the review corrected both docs upstream in 25961c5e.
TEST(MediaCoreCommand, OutputSenderSessionPublishesEveryBackpressureField) {
  auto modules = corevideo::modules::createStubModules();
  modules.outputSender = std::make_unique<FullBackpressureFieldsSender>();
  corevideo::core::MediaCore mediaCore(std::move(modules));
  const corevideo::rpc::Json startOutputs = corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"rtmp"}},
  };
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{startOutputs});

  const auto* output = state.get("outputSenderSession");
  ASSERT_NE(output, nullptr);
  const auto* senders = output->get("senders");
  ASSERT_NE(senders, nullptr);
  ASSERT_TRUE(senders->isArray());
  ASSERT_FALSE(senders->asArray().empty());
  const auto& sender = senders->asArray().front();
  const auto* bp = sender.get("backpressure");
  ASSERT_NE(bp, nullptr) << "outputSenderSession.senders[].backpressure must exist";
  EXPECT_EQ(bp->getNumber("divisor"), 3);
  EXPECT_EQ(bp->getNumber("level"), 2);
  EXPECT_EQ(bp->getNumber("bufferedMs"), 812);
  EXPECT_EQ(bp->getNumber("queuedChunks"), 47);
  EXPECT_EQ(bp->getNumber("inFlightWriteMs"), 686);
  EXPECT_EQ(bp->getNumber("maxWriteMs"), 691);
  EXPECT_EQ(bp->getNumber("slowWriteCount"), 3);
  EXPECT_EQ(bp->getNumber("enteredCount"), 5);
  EXPECT_EQ(bp->getNumber("discardedChunks"), 19);
  EXPECT_EQ(bp->getNumber("discardEvents"), 2);
  EXPECT_EQ(bp->getString("lastReason"), "buffered-above-threshold");
  EXPECT_EQ(bp->getNumber("lastTransitionBufferedMs"), 300);
  EXPECT_EQ(bp->getNumber("runId"), 4);
  EXPECT_EQ(bp->getNumber("observedAtMs"), 12345.0);

  // FINAL-REVIEW FINDING 3, THE HEALTHY SIBLING - the node lied for it.
  //
  // Lever A is per-ENCODER: one encoder texture feeds every GPU-direct sender,
  // so MediaCore applies the MAX divisor across the active ones. With two
  // GPU-direct destinations the healthy one's node reported `divisor: 1` while
  // it was actually being fed at the maximum - a textbook-healthy reading for a
  // source running at a quarter rate. The lever's per-encoder limitation was
  // named in three comments; the NODE's was not, and the node is what an
  // operator readout binds to.
  //
  // `appliedDivisor` is the rate the compositor is actually exporting at,
  // written by MediaCore where that fact exists. Sourcing it from `bp.divisor`
  // instead - the obvious wrong fix - passes for the throttled sender above and
  // fails here, which is the whole point of asserting it on the SIBLING.
  ASSERT_GT(senders->asArray().size(), 1u)
      << "this test needs the healthy sibling to say anything about finding 3";
  const auto* siblingBp = senders->asArray()[1].get("backpressure");
  ASSERT_NE(siblingBp, nullptr);
  EXPECT_EQ(siblingBp->getNumber("divisor"), 1)
      << "the sibling's own REQUEST is unthrottled - that part was always true";
  EXPECT_EQ(siblingBp->getNumber("appliedDivisor"), 3)
      << "the sibling is fed at the MAX across senders; its node must say so instead of "
         "publishing a healthy-looking rate it is not running at";
  // And the throttled sender's own node carries it too, so a reader never has
  // to know which destination is the worst one to learn the applied rate.
  ASSERT_NE(bp->get("appliedDivisor"), nullptr);
  EXPECT_EQ(bp->getNumber("appliedDivisor"), 3);
}

TEST(MediaCoreCommand, AudioMonitorRendersRoutedMonBusWhenPresent) {
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  auto monitor = std::make_unique<RecordingMonitorOutput>();
  auto* monitorPtr = monitor.get();
  modules.monitorOutput = std::move(monitor);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-monitor"},
          {"enabled", true},
          {"deviceId", "render-device"},
          {"deviceName", "Render Device"},
          {"volume", 1.0},
      },
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-routing-matrix"},
          {"sends", corevideo::rpc::Json::Array{
                        corevideo::rpc::Json::Object{{"sourceId", "pcm-speaker"}, {"busId", "mon"}, {"gainDb", -6.0}},
                    }},
      },
  });

  EXPECT_TRUE(monitorPtr->renderCalls > 0);
  EXPECT_EQ(monitorPtr->lastDeviceId, "render-device");
  EXPECT_EQ(monitorPtr->lastChannels, 2);
  EXPECT_TRUE(std::abs(monitorPtr->lastVolume - 1.0) < 0.001);
  EXPECT_TRUE(std::abs(monitorPtr->lastFirstSample - 0.2506f) < 0.01f);
  EXPECT_EQ(state.get("audioMixSession")->getString("monitorStatus"), "playing");
  EXPECT_TRUE(state.get("audioMixSession")->get("monitorFramesPlayed")->asNumber() > 0);
}

TEST(MediaCoreCommand, DefaultFallbackDoesNotFabricateAudioSignal) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-monitor"},
          {"enabled", true},
          {"deviceId", "stub-render"},
          {"deviceName", "Stub Render"},
          {"volume", 0.75},
      },
  });

  const auto* mix = state.get("audioMixSession");
  ASSERT_NE(mix, nullptr);
  EXPECT_EQ(mix->getString("status"), "idle");
  EXPECT_EQ(mix->get("masterLevel")->asNumber(), 0);
  EXPECT_EQ(mix->get("mixedFrameCount")->asNumber(), 0);
  EXPECT_EQ(mix->getString("monitorStatus"), "armed");
  EXPECT_EQ(mix->get("monitorFramesPlayed")->asNumber(), 0);
  EXPECT_TRUE(jsonArrayContains(
      *mix->get("warnings"),
      "Audio monitor is armed but no PCM reached the MON bus or fallback monitor mix."));
  ASSERT_NE(mix->get("participants"), nullptr);
  EXPECT_TRUE(mix->get("participants")->asArray().empty());
}

TEST(MediaCoreCommand, AudioMonitorReportsZeroVolumeExplicitly) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-monitor"},
          {"enabled", true},
          {"deviceId", "stub-render"},
          {"deviceName", "Stub Render"},
          {"volume", 0.0},
      },
  });

  const auto* mix = state.get("audioMixSession");
  ASSERT_NE(mix, nullptr);
  EXPECT_TRUE(mix->get("monitorEnabled")->asBool());
  EXPECT_EQ(mix->getString("monitorStatus"), "volume-zero");
  EXPECT_EQ(mix->get("monitorFramesPlayed")->asNumber(), 0);
}

TEST(MediaCoreCommand, StableOverlayIdDoesNotDuplicateKeyLayer) {
  corevideo::core::MediaCore mediaCore;
  const auto first = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-overlay-asset"},
          {"overlayId", "key:lower-third"},
          {"text", "David Chen"},
          {"position", "lower-third"},
          {"sourceId", "p2"},
          {"sourceName", "David Chen"},
          {"title", "Chief Product Officer"},
          {"org", "Main room"},
          {"keyPhase", "building-in"},
          {"buildInMs", 350},
          {"buildOutMs", 275},
      },
  });
  const auto second = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-overlay-asset"},
          {"overlayId", "key:lower-third"},
          {"text", "David Chen"},
          {"position", "lower-third"},
          {"sourceId", "p2"},
          {"sourceName", "David Chen"},
          {"keyPhase", "on-air"},
      },
  });

  EXPECT_EQ(first.get("overlayCount")->asNumber(), 1);
  EXPECT_EQ(second.get("overlayCount")->asNumber(), 1);

  const auto* overlayState = second.get("overlayState");
  ASSERT_NE(overlayState, nullptr);
  EXPECT_EQ(overlayState->getString("status"), "live");
  EXPECT_EQ(overlayState->get("lowerThirdCount")->asNumber(), 1);
  const auto* overlays = overlayState->get("overlays");
  ASSERT_NE(overlays, nullptr);
  ASSERT_TRUE(overlays->isArray());
  ASSERT_TRUE(overlays->asArray().size() == 1u);
  const auto& lowerThird = overlays->asArray().front();
  EXPECT_EQ(lowerThird.getString("overlayId"), "key:lower-third");
  EXPECT_EQ(lowerThird.getString("kind"), "lower-third");
  EXPECT_EQ(lowerThird.getString("sourceId"), "p2");
  EXPECT_EQ(lowerThird.getString("sourceName"), "David Chen");
  EXPECT_EQ(lowerThird.getString("title"), "Chief Product Officer");
  EXPECT_EQ(lowerThird.getString("org"), "Main room");
  EXPECT_EQ(lowerThird.getString("keyPhase"), "on-air");
  EXPECT_EQ(lowerThird.get("buildInMs")->asNumber(), 350);
  EXPECT_EQ(lowerThird.get("buildOutMs")->asNumber(), 275);
}

TEST(MediaCoreCommand, LowerThirdAnimationHonorsConfiguredBuildDurations) {
  corevideo::core::MediaCore mediaCore;
  auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-overlay-asset"},
          {"overlayId", "key:lower-third"},
          {"text", "Ada Otieno"},
          {"position", "lower-third"},
          {"enabled", true},
          {"keyPhase", "building-in"},
          {"buildInMs", 1000},
          {"buildOutMs", 900},
      },
  });

  // Twelve default render ticks are roughly 400 ms. The former hard-coded
  // 420 ms clock settled here even though the operator requested one second.
  for (int i = 0; i < 11; ++i) {
    state = mediaCore.applyCommands(corevideo::rpc::Json::Array{});
  }
  const auto* overlays = state.get("overlayState")->get("overlays");
  ASSERT_NE(overlays, nullptr);
  ASSERT_EQ(overlays->asArray().size(), 1u);
  EXPECT_EQ(overlays->asArray().front().getString("keyPhase"), "building-in");
  EXPECT_LT(overlays->asArray().front().get("keyProgress")->asNumber(), 0.75);

  for (int i = 0; i < 55; ++i) {
    state = mediaCore.applyCommands(corevideo::rpc::Json::Array{});
  }
  overlays = state.get("overlayState")->get("overlays");
  ASSERT_NE(overlays, nullptr);
  ASSERT_EQ(overlays->asArray().size(), 1u);
  EXPECT_EQ(overlays->asArray().front().getString("keyPhase"), "on-air");
}

TEST(MediaCoreCommand, SettledExplicitLowerThirdBuildOutDoesNotRecreateOrFlash) {
  corevideo::core::MediaCore mediaCore;
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-overlay-asset"},
          {"overlayId", "key:lower-third"},
          {"text", "Ada Otieno"},
          {"position", "lower-third"},
          {"enabled", true},
          {"keyPhase", "on-air"},
          {"buildOutMs", 50},
      },
  });

  auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-overlay-asset"},
          {"overlayId", "key:lower-third"},
          {"text", "Ada Otieno"},
          {"position", "lower-third"},
          {"enabled", true},
          {"keyPhase", "building-out"},
          {"buildOutMs", 50},
      },
  });
  for (int i = 0; i < 8; ++i) {
    state = mediaCore.applyCommands(corevideo::rpc::Json::Array{});
  }

  const auto* overlays = state.get("overlayState")->get("overlays");
  ASSERT_NE(overlays, nullptr);
  ASSERT_EQ(overlays->asArray().size(), 1u);
  EXPECT_EQ(overlays->asArray().front().getString("keyPhase"), "building-out");
  EXPECT_EQ(overlays->asArray().front().get("keyProgress")->asNumber(), 1.0);

  // A normal scene refresh can resend the phase after its visual duration.
  // It must update the settled invisible asset, not create a fresh build-out.
  state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-overlay-asset"},
          {"overlayId", "key:lower-third"},
          {"text", "Ada Otieno"},
          {"position", "lower-third"},
          {"enabled", true},
          {"keyPhase", "building-out"},
          {"buildOutMs", 50},
      },
  });
  overlays = state.get("overlayState")->get("overlays");
  ASSERT_NE(overlays, nullptr);
  ASSERT_EQ(overlays->asArray().size(), 1u);
  EXPECT_EQ(overlays->asArray().front().get("keyProgress")->asNumber(), 1.0);

  state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-overlay-asset"},
          {"overlayId", "key:lower-third"},
          {"position", "lower-third"},
          {"enabled", false},
          {"keyPhase", "hidden"},
      },
  });
  overlays = state.get("overlayState")->get("overlays");
  ASSERT_NE(overlays, nullptr);
  EXPECT_TRUE(overlays->asArray().empty());
  EXPECT_EQ(state.get("overlayCount")->asNumber(), 0);
}

TEST(MediaCoreCommand, NativeOwnedBuildOutRetiresOverlayIdentityCompletely) {
  corevideo::core::MediaCore mediaCore;
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-overlay-asset"},
          {"overlayId", "generic-overlay"},
          {"text", "Generic"},
          {"enabled", true},
          {"keyPhase", "on-air"},
          {"buildOutMs", 50},
      },
  });
  auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-overlay-asset"},
          {"overlayId", "generic-overlay"},
          {"enabled", false},
      },
  });
  for (int i = 0; i < 8; ++i) {
    state = mediaCore.applyCommands(corevideo::rpc::Json::Array{});
  }

  const auto* overlays = state.get("overlayState")->get("overlays");
  ASSERT_NE(overlays, nullptr);
  EXPECT_TRUE(overlays->asArray().empty());
  EXPECT_EQ(state.get("overlayCount")->asNumber(), 0);
}

TEST(MediaCoreCommand, DisabledOverlayClearsStableKeyLayer) {
  corevideo::core::MediaCore mediaCore;
  const auto active = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-overlay-asset"},
          {"overlayId", "key:lower-third"},
          {"text", "David Chen"},
          {"position", "lower-third"},
          {"enabled", true},
      },
  });
  EXPECT_EQ(active.get("overlayCount")->asNumber(), 1);

  const auto cleared = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-overlay-asset"},
          {"overlayId", "key:lower-third"},
          {"position", "lower-third"},
          {"enabled", false},
      },
  });

  EXPECT_EQ(cleared.get("overlayCount")->asNumber(), 0);
}

TEST(MediaCoreCommand, OverlayAssetRastersAnimatedLowerThirdIntoProgramFrame) {
  corevideo::core::MediaCore mediaCore;
  // Enable a lower-third overlay; the first tick captures the building-in
  // animation (low alpha), later ticks settle it on-air.
  const auto earlyState = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-overlay-asset"},
          {"overlayId", "key:lower-third"},
          {"position", "lower-third"},
          {"title", "ADA OTIENO"},
          {"org", "AETHELRED LABS"},
          {"keyPosition", "lower-left"},
      },
  });

  auto bandDistinctColors = [](const corevideo::rpc::Json& previewEvent) -> size_t {
    const auto* preview = previewEvent.get("preview");
    if (!preview) {
      preview = previewEvent.get("programFramePreview");
    }
    if (!preview) {
      return 0;
    }
    const int width = static_cast<int>(preview->get("width")->asNumber());
    const int height = static_cast<int>(preview->get("height")->asNumber());
    const auto decoded = corevideo::modules::base64Decode(preview->getString("bgraBase64"));
    std::set<uint32_t> colors;
    const int top = static_cast<int>(0.78f * height);
    const int bottom = static_cast<int>(0.94f * height);
    for (int y = top; y < bottom; ++y) {
      for (int x = 0; x < width; ++x) {
        const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
        if (offset + 3 >= decoded.size()) {
          continue;
        }
        colors.insert((static_cast<uint32_t>(decoded[offset + 3]) << 24) |
                      (static_cast<uint32_t>(decoded[offset + 2]) << 16) |
                      (static_cast<uint32_t>(decoded[offset + 1]) << 8) |
                      static_cast<uint32_t>(decoded[offset + 0]));
      }
    }
    return colors.size();
  };

  // Preview EVENTS are wall-clock throttled (~30fps), so WHICH animation tick
  // lands in the event queue depends on host speed — on a fast machine the
  // whole settle loop below can complete inside one throttle window. Keep the
  // events as an emission smoke check only, and probe pixels through the
  // per-call state, which embeds the same preview unthrottled: tick 1 vs tick
  // 13 is deterministic on every host.
  const auto earlyPreviews = mediaCore.drainProgramFramePreviewEvents();
  ASSERT_FALSE(earlyPreviews.empty());
  const size_t earlyColors = bandDistinctColors(earlyState);

  // Advance several ticks so the building-in animation settles on-air.
  corevideo::rpc::Json settledState = nullptr;
  for (int i = 0; i < 12; ++i) {
    settledState = mediaCore.applyCommands(corevideo::rpc::Json::Array{});
  }
  const size_t settledColors = bandDistinctColors(settledState);

  // Settled overlay rasters real text/brand pixels (non-uniform band) and is
  // more opaque/legible than the first building-in frame.
  EXPECT_TRUE(settledColors > 2u);
  EXPECT_TRUE(settledColors >= earlyColors);
}

TEST(MediaCoreCommand, CaptionCueRastersStyledLowerBand) {
  corevideo::core::MediaCore mediaCore;
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "push-caption-cue"},
          {"text", "TESTING LIVE CAPTIONS"},
          {"speaker", "ADA"},
      },
  });

  const auto previews = mediaCore.drainProgramFramePreviewEvents();
  ASSERT_FALSE(previews.empty());
  const auto* preview = previews.back().get("preview");
  ASSERT_NE(preview, nullptr);
  const int width = static_cast<int>(preview->get("width")->asNumber());
  const int height = static_cast<int>(preview->get("height")->asNumber());
  const auto decoded = corevideo::modules::base64Decode(preview->getString("bgraBase64"));
  std::set<uint32_t> colors;
  const int top = static_cast<int>(0.86f * height);
  const int bottom = static_cast<int>(0.96f * height);
  for (int y = top; y < bottom; ++y) {
    for (int x = static_cast<int>(0.08f * width); x < static_cast<int>(0.92f * width); ++x) {
      const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
      if (offset + 3 >= decoded.size()) {
        continue;
      }
      colors.insert((static_cast<uint32_t>(decoded[offset + 2]) << 16) |
                    (static_cast<uint32_t>(decoded[offset + 1]) << 8) |
                    static_cast<uint32_t>(decoded[offset + 0]));
    }
  }
  EXPECT_TRUE(colors.size() > 2u);
}

TEST(MediaCoreCommand, SurfacesInvalidSceneGraphAsDegradedProgramFrameMetadata) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", ""},
          {"routes", corevideo::rpc::Json::Array{
                         corevideo::rpc::Json::Object{{"routeId", ""}, {"mode", "teleport"}},
                     }},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-program-output"},
          {"destinations", corevideo::rpc::Json::Array{"recording"}},
          {"isoParticipantIds", corevideo::rpc::Json::Array{}},
      },
  });

  const auto* frame = state.get("programFrame");
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->getString("health"), "degraded");
  EXPECT_EQ(frame->getString("renderer"), "software");
  EXPECT_GE(frame->get("frameNumber")->asNumber(), 1);
  EXPECT_NE(frame->get("programPixelSignature")->asNumber(), 0);
  EXPECT_NE(frame->get("renderPlanSignature")->asNumber(), 0);
  EXPECT_GE(frame->get("warnings")->asArray().size(), 2u);
  EXPECT_EQ(state.get("health")->getString("programFrameHealth"), "degraded");
  EXPECT_NE(state.get("health")->get("messages")->asArray().back().asString().find("Compositor warning"), std::string::npos);
}

TEST(MediaCoreCommand, PerRouteColorGradeChangesCompositorRenderPlanSignature) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  const auto first = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "graded"},
          {"routes", corevideo::rpc::Json::Array{
                         corevideo::rpc::Json::Object{
                             {"routeId", "a"},
                             {"mode", "fixed"},
                             {"audioRole", "mix"},
                             {"participantId", "speaker-1"},
                             {"colorGrade",
                              corevideo::rpc::Json::Object{
                                  {"exposure", 8},
                                  {"contrast", 12},
                                  {"saturation", -6},
                                  {"temperature", 15},
                              }},
                         },
                     }},
      },
  });
  const auto second = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "graded"},
          {"routes", corevideo::rpc::Json::Array{
                         corevideo::rpc::Json::Object{
                             {"routeId", "a"},
                             {"mode", "fixed"},
                             {"audioRole", "mix"},
                             {"participantId", "speaker-1"},
                             {"colorGrade",
                              corevideo::rpc::Json::Object{
                                  {"exposure", 8},
                                  {"contrast", 3},
                                  {"saturation", -6},
                                  {"temperature", 15},
                              }},
                         },
                     }},
      },
  });

  ASSERT_NE(first.get("programFrame"), nullptr);
  ASSERT_NE(second.get("programFrame"), nullptr);
  EXPECT_NE(
      first.get("programFrame")->get("renderPlanSignature")->asNumber(),
      second.get("programFrame")->get("renderPlanSignature")->asNumber());
}

// CHROMA KEY. The core advertised a "chroma-key" capability — and listed it as
// REQUIRED in kRequiredMvpCapabilities — while implementing none of it: the only
// command carrying a chromaKey payload discarded it (setParticipantTransform
// takes an UNNAMED rpc::Json), the render-plan layer had no key fields, and
// neither shader had keying math. Anything gating on that capability got a true
// answer that meant nothing. These pin the plumbing that now backs the claim.
TEST(MediaCoreCommand, PerRouteChromaKeyChangesCompositorRenderPlanSignature) {
  const auto sceneWith = [](double similarity) {
    return corevideo::rpc::Json::Array{
        corevideo::rpc::Json::Object{
            {"type", "load-scene-graph"},
            {"sceneId", "keyed"},
            {"routes", corevideo::rpc::Json::Array{
                           corevideo::rpc::Json::Object{
                               {"routeId", "a"},
                               {"mode", "fixed"},
                               {"audioRole", "mix"},
                               {"participantId", "speaker-1"},
                               {"chromaKey",
                                corevideo::rpc::Json::Object{
                                    {"keyR", 0.0},
                                    {"keyG", 1.0},
                                    {"keyB", 0.0},
                                    {"similarity", similarity},
                                    {"smoothness", 0.1},
                                    {"spill", 0.2},
                                }},
                           },
                       }},
        },
    };
  };
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  const auto first = mediaCore.applyCommands(sceneWith(0.4));
  const auto second = mediaCore.applyCommands(sceneWith(0.6));
  ASSERT_NE(first.get("programFrame"), nullptr);
  ASSERT_NE(second.get("programFrame"), nullptr);
  // A key parameter the operator changed MUST reach the render plan; if the
  // payload were discarded again both signatures would be identical.
  EXPECT_NE(first.get("programFrame")->get("renderPlanSignature")->asNumber(),
            second.get("programFrame")->get("renderPlanSignature")->asNumber());
}

// `enabled:false` must actually disable it. A route can carry key settings the
// operator has switched off, and keying anyway would punch holes in a live
// program.
TEST(MediaCoreCommand, ChromaKeyDisabledRendersIdenticallyToNoKeyAtAll) {
  const auto scene = [](bool withKey, bool enabled) {
    corevideo::rpc::Json::Object route{
        {"routeId", "a"},
        {"mode", "fixed"},
        {"audioRole", "mix"},
        {"participantId", "speaker-1"},
    };
    if (withKey) {
      route["chromaKey"] = corevideo::rpc::Json::Object{
          {"enabled", enabled},
          {"keyG", 1.0},
          {"similarity", 0.4},
      };
    }
    return corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
        {"type", "load-scene-graph"},
        {"sceneId", "keyed"},
        {"routes", corevideo::rpc::Json::Array{route}},
    }};
  };
  corevideo::core::MediaCore withoutKey(corevideo::modules::createStubModules());
  corevideo::core::MediaCore disabledKey(corevideo::modules::createStubModules());
  const auto plain = withoutKey.applyCommands(scene(false, false));
  const auto off = disabledKey.applyCommands(scene(true, false));
  ASSERT_NE(plain.get("programFrame"), nullptr);
  ASSERT_NE(off.get("programFrame"), nullptr);
  EXPECT_EQ(plain.get("programFrame")->get("renderPlanSignature")->asNumber(),
            off.get("programFrame")->get("renderPlanSignature")->asNumber());
}

// Borders NEVER composite into program/preview — they exist solely to separate
// tiles in the multiview (owner rule, 2026-07-31). Whatever borderStyle a route
// carries on the wire (missing, "none", or an explicit "accent"), the composed
// feed is identical: the old "accent" default baked a studio-green frame into
// the program, which the virtual camera, recordings, and streams all inherit.
TEST(MediaCoreCommand, ExplicitSceneRouteBordersCompositeIntoProgram) {
  const auto loadScene = [](const char* borderStyle) {
    auto route = corevideo::rpc::Json::Object{
        {"routeId", "a"},
        {"mode", "fixed"},
        {"audioRole", "mix"},
        {"participantId", "speaker-1"},
    };
    if (borderStyle != nullptr) {
      route["borderStyle"] = borderStyle;
    }
    return corevideo::rpc::Json::Array{
        corevideo::rpc::Json::Object{
            {"type", "load-scene-graph"},
            {"sceneId", "solo"},
            {"routes", corevideo::rpc::Json::Array{std::move(route)}},
        },
    };
  };
  const auto signatureOf = [](const corevideo::rpc::Json& state) {
    const auto* frame = state.get("programFrame");
    EXPECT_NE(frame, nullptr);
    return frame->get("renderPlanSignature")->asNumber();
  };

  corevideo::core::MediaCore defaulted(corevideo::modules::createStubModules());
  corevideo::core::MediaCore borderless(corevideo::modules::createStubModules());
  corevideo::core::MediaCore accented(corevideo::modules::createStubModules());

  const auto defaultSignature = signatureOf(defaulted.applyCommands(loadScene(nullptr)));
  const auto noneSignature = signatureOf(borderless.applyCommands(loadScene("none")));
  const auto accentSignature = signatureOf(accented.applyCommands(loadScene("accent")));

  EXPECT_EQ(defaultSignature, noneSignature);
  EXPECT_NE(defaultSignature, accentSignature);
}

// The compositor must be ALWAYS ON: even with no scene graph, no Zoom input frames,
// and an empty command tick, every applyCommands call advances the program frame and
// emits a synthetic black/slate program preview so the operator's Preview/Program
// surfaces are never stuck on "waiting for compositor output".
TEST(MediaCoreCommand, EmptyTickEmitsSyntheticProgramFrameWithoutAnyInput) {
  corevideo::core::MediaCore mediaCore;

  // A bare tick with zero commands and zero Zoom video frames.
  const auto first = mediaCore.applyCommands(corevideo::rpc::Json::Array{});

  const auto* frame = first.get("programFrame");
  ASSERT_NE(frame, nullptr);
  EXPECT_GE(frame->get("frameNumber")->asNumber(), 1);
  EXPECT_GE(frame->get("width")->asNumber(), 1);
  EXPECT_GE(frame->get("height")->asNumber(), 1);
  // Black slate still produces a stable, non-zero pixel signature.
  EXPECT_NE(frame->get("programPixelSignature")->asNumber(), 0);
  EXPECT_GE(first.get("programFrameCount")->asNumber(), 1);

  // A synthetic program preview must be published even with no input frames.
  const auto* preview = first.get("programFramePreview");
  ASSERT_NE(preview, nullptr);
  EXPECT_GE(preview->get("width")->asNumber(), 1);
  EXPECT_GE(preview->get("height")->asNumber(), 1);

  // A follow-up empty tick keeps the compositor running (frame number advances),
  // proving the program feed is continuous rather than gated on capture/input.
  const auto second = mediaCore.applyCommands(corevideo::rpc::Json::Array{});
  EXPECT_TRUE(second.get("programFrameCount")->asNumber() > first.get("programFrameCount")->asNumber());
}

TEST(MediaCoreCommand, ProfileMirrorsNativeMediaCoreShape) {
  corevideo::core::MediaCore mediaCore;
  const auto profile = mediaCore.profile();

#if COREVIDEO_WITH_D3D11
  EXPECT_EQ(profile.getString("name"), "CoreVideo Pro Native Media Core");
  EXPECT_EQ(profile.getString("renderer"), "d3d11");
#elif COREVIDEO_WITH_METAL
  EXPECT_EQ(profile.getString("name"), "CoreVideo Pro Native Media Core");
  EXPECT_EQ(profile.getString("renderer"), "metal");
#else
  EXPECT_EQ(profile.getString("name"), "CoreVideo Pro Native Media Core Stub");
  EXPECT_EQ(profile.getString("renderer"), "software");
#endif
  EXPECT_EQ(profile.getString("maxProgramResolution"), "3840x2160");
  EXPECT_EQ(profile.get("maxProgramFps")->asNumber(), 60);
  EXPECT_GE(profile.get("maxParticipantFeeds")->asNumber(), 8);
  EXPECT_GE(profile.get("maxIsoRecordings")->asNumber(), 8);
  ASSERT_NE(profile.get("capabilities"), nullptr);
  const auto& capabilities = *profile.get("capabilities");
  EXPECT_TRUE(jsonArrayContains(capabilities, "audio-mixer"));
  EXPECT_TRUE(jsonArrayContains(capabilities, "scene-graph-rendering"));
  EXPECT_TRUE(jsonArrayContains(capabilities, "dynamic-overlays"));
  const bool hasAudioCapture = corevideo::modules::createWasapiAudioCaptureSource() != nullptr ||
                               corevideo::modules::createCoreAudioCaptureSource() != nullptr;
  const bool hasAudioMonitor = corevideo::modules::createWasapiMonitorOutput() != nullptr ||
                               corevideo::modules::createCoreAudioMonitorOutput() != nullptr;
  EXPECT_EQ(jsonArrayContains(capabilities, "local-audio-capture"), hasAudioCapture);
  EXPECT_EQ(jsonArrayContains(capabilities, "audio-monitor-output"), hasAudioMonitor);
#if COREVIDEO_WITH_D3D11 || COREVIDEO_WITH_METAL
  EXPECT_TRUE(jsonArrayContains(capabilities, "gpu-compositor"));
  EXPECT_TRUE(jsonArrayContains(capabilities, "chroma-key"));
  EXPECT_TRUE(jsonArrayContains(capabilities, "smart-framing"));
#else
  EXPECT_FALSE(jsonArrayContains(capabilities, "gpu-compositor"));
#endif
#if COREVIDEO_WITH_ZOOM
  EXPECT_TRUE(jsonArrayContains(capabilities, "zoom-raw-video"));
  EXPECT_TRUE(jsonArrayContains(capabilities, "zoom-raw-audio"));
#else
  EXPECT_FALSE(jsonArrayContains(capabilities, "zoom-raw-video"));
#endif
#if COREVIDEO_WITH_MF_ENCODER
  EXPECT_TRUE(jsonArrayContains(capabilities, "program-recording"));
  EXPECT_TRUE(jsonArrayContains(capabilities, "iso-recording"));
#elif COREVIDEO_WITH_AVF_ENCODER
  EXPECT_TRUE(jsonArrayContains(capabilities, "program-recording"));
  EXPECT_TRUE(jsonArrayContains(capabilities, "iso-recording"));
#else
  EXPECT_FALSE(jsonArrayContains(capabilities, "program-recording"));
#endif
#if COREVIDEO_WITH_UVC || COREVIDEO_WITH_AVF_CAPTURE
  EXPECT_TRUE(jsonArrayContains(capabilities, "uvc-capture"));
#else
  EXPECT_FALSE(jsonArrayContains(capabilities, "uvc-capture"));
#endif
#if COREVIDEO_WITH_RTMP_OUTPUT
  EXPECT_TRUE(jsonArrayContains(capabilities, "rtmp-output"));
#else
  EXPECT_FALSE(jsonArrayContains(capabilities, "rtmp-output"));
#endif
#if COREVIDEO_WITH_NDI_OUTPUT
  EXPECT_TRUE(jsonArrayContains(capabilities, "ndi-output"));
#else
  EXPECT_FALSE(jsonArrayContains(capabilities, "ndi-output"));
#endif
#if COREVIDEO_WITH_SRT_OUTPUT
  EXPECT_TRUE(jsonArrayContains(capabilities, "srt-output"));
#else
  EXPECT_FALSE(jsonArrayContains(capabilities, "srt-output"));
#endif
}

TEST(MediaCoreCommand, DefaultFactoryReportsActiveRendererInHealth) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"recording"}},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}},
  });
  EXPECT_TRUE(state.get("active")->asBool());
  const auto health = mediaCore.health();
#if COREVIDEO_WITH_D3D11
  EXPECT_EQ(health.getString("renderer"), "d3d11");
#elif COREVIDEO_WITH_METAL
  EXPECT_EQ(health.getString("renderer"), "metal");
#else
  EXPECT_EQ(health.getString("renderer"), "software");
#endif
}

TEST(MediaCoreCommand, PublishesCumulativeRenderDeadlineMissesToCompositorTelemetry) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  mediaCore.reportRenderDeadlineMisses(1499);
  mediaCore.reportRenderDeadlineMisses(3);

  const auto state = mediaCore.sessionState();
  const auto* compositor = state.get("compositor");
  ASSERT_NE(compositor, nullptr);
  EXPECT_EQ(compositor->get("droppedFrameCount")->asNumber(), 1502);
  EXPECT_EQ(state.get("health")->get("renderDeadlineMisses")->asNumber(), 1502);
}

TEST(MediaCoreCommand, SessionAndHealthExposeZoomReadinessEvidenceWithoutSdk) {
  corevideo::core::MediaCore mediaCore;

  const auto state = mediaCore.sessionState();
  const auto* zoom = state.get("zoom");
  ASSERT_NE(zoom, nullptr);
  EXPECT_EQ(zoom->get("readiness")->getString("status"), "ready");
  EXPECT_EQ(zoom->get("readiness")->getString("mode"), "stub");
  EXPECT_FALSE(zoom->get("readiness")->get("sdkAvailable")->asBool());
  EXPECT_EQ(zoom->get("evidence")->getString("source"), "native-core-stub");
  EXPECT_TRUE(zoom->get("evidence")->get("synthetic")->asBool());
  EXPECT_FALSE(zoom->get("evidence")->get("joined")->asBool());

  const auto joined = mediaCore.joinZoom(corevideo::rpc::Json::Object{{"displayName", "Operator"}});
  EXPECT_EQ(joined.get("evidence")->get("participantCount")->asNumber(), 2);
  EXPECT_EQ(joined.get("evidence")->getString("activeSpeakerId"), "operator-1");

  const auto health = mediaCore.health();
  ASSERT_NE(health.get("zoom"), nullptr);
  EXPECT_EQ(health.get("zoom")->get("readiness")->getString("mode"), "stub");
  EXPECT_TRUE(health.get("zoom")->get("evidence")->get("joined")->asBool());
}

// #535 slice 3b: set-media-playback is a SELECTION echo now. The status is the
// selected asset transport state ("idle" with nothing selected, "unavailable"
// when the selection is on neither bus, else cued|live|paused|ended), and the
// wire `playing`/`mediaPlaybackKey` fields are ignored. The empty-id warning
// is unchanged.
TEST(MediaCoreCommand, AppliesMediaPlaybackCommandAndWarnsOnEmptyAsset) {
  corevideo::core::MediaCore mediaCore;

  const auto idle = mediaCore.sessionState();
  ASSERT_NE(idle.get("mediaPlayback"), nullptr);
  EXPECT_EQ(idle.get("mediaPlayback")->getString("status"), "idle");
  EXPECT_FALSE(idle.get("mediaPlayback")->get("playing")->asBool());

  const auto selected = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-media-playback"},
      {"mediaAssetId", "clip-intro"},
      {"mediaAssetName", "Intro Sting"},
      {"mediaAssetKind", "stinger"},
      {"mediaAssetPath", "C:/media/intro.mp4"},
      {"mediaPlaybackKey", "program-take:3:media:clip-intro"},
      {"playing", true},
  });
  const auto* playback = selected.get("mediaPlayback");
  ASSERT_NE(playback, nullptr);
  // No scene routes this asset, so it has no transport: the node says so
  // rather than echoing the wire flag back as truth.
  EXPECT_EQ(playback->getString("status"), "unavailable");
  EXPECT_EQ(playback->getString("mediaAssetId"), "clip-intro");
  EXPECT_EQ(playback->getString("mediaAssetName"), "Intro Sting");
  EXPECT_EQ(playback->getString("mediaAssetKind"), "stinger");
  EXPECT_EQ(playback->getString("mediaAssetPath"), "C:/media/intro.mp4");
  EXPECT_EQ(playback->getString("mediaPlaybackKey"), "");
  EXPECT_FALSE(playback->get("playing")->asBool());
  EXPECT_EQ(playback->getString("summary"), "Intro Sting unavailable.");

  const auto moved = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-media-playback"},
      {"mediaAssetId", "clip-outro"},
      {"mediaAssetName", "Outro Loop"},
      {"playing", false},
  });
  ASSERT_NE(moved.get("mediaPlayback"), nullptr);
  EXPECT_EQ(moved.get("mediaPlayback")->getString("mediaAssetId"), "clip-outro");
  EXPECT_EQ(moved.get("mediaPlayback")->getString("status"), "unavailable");
  EXPECT_FALSE(moved.get("mediaPlayback")->get("playing")->asBool());

  const auto empty = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-media-playback"},
      {"mediaAssetId", ""},
      {"mediaAssetName", ""},
      {"playing", true},
  });
  const auto* emptyPlayback = empty.get("mediaPlayback");
  ASSERT_NE(emptyPlayback, nullptr);
  EXPECT_EQ(emptyPlayback->getString("status"), "idle");
  EXPECT_FALSE(emptyPlayback->get("playing")->asBool());
  const auto& warnings = emptyPlayback->get("warnings")->asArray();
  EXPECT_TRUE(std::any_of(warnings.begin(), warnings.end(), [](const corevideo::rpc::Json& warning) {
    return warning.isString() && warning.asString().find("no media asset id") != std::string::npos;
  }));
}

TEST(MediaCoreCommand, BrandKitSnapshotIncludesFullWinUiContract) {
  corevideo::core::MediaCore mediaCore;

  const auto idle = mediaCore.sessionState();
  const auto* idleBrandKit = idle.get("brandKit");
  ASSERT_NE(idleBrandKit, nullptr);
  EXPECT_EQ(idleBrandKit->getString("captionStyle"), "medium sentence captions");
  EXPECT_EQ(idleBrandKit->getString("defaultOverlayBehavior"), "all-off");

  const auto branded = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-brand-kit"},
      {"name", "Launch Night"},
      {"logoText", "CVP"},
      {"brandColor", "#102030"},
      {"accentColor", "#405060"},
      {"backgroundColor", "#070809"},
      {"fontFamily", "Poppins"},
      {"lowerThirdStyle", "minimal"},
      {"captionStyle", "boxed"},
      {"defaultOverlayBehavior", "auto"},
  });

  const auto* brandKit = branded.get("brandKit");
  ASSERT_NE(brandKit, nullptr);
  EXPECT_EQ(brandKit->getString("name"), "Launch Night");
  EXPECT_EQ(brandKit->getString("logoText"), "CVP");
  EXPECT_EQ(brandKit->getString("brandColor"), "#102030");
  EXPECT_EQ(brandKit->getString("accentColor"), "#405060");
  EXPECT_EQ(brandKit->getString("backgroundColor"), "#070809");
  EXPECT_EQ(brandKit->getString("fontFamily"), "Poppins");
  EXPECT_EQ(brandKit->getString("lowerThirdStyle"), "minimal");
  EXPECT_EQ(brandKit->getString("captionStyle"), "boxed");
  EXPECT_EQ(brandKit->getString("defaultOverlayBehavior"), "auto");
}

TEST(MediaCoreCommand, AudioMixSessionDoesNotFakeMetersWithoutPcm) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "sync-participant-audio-mix"},
      {"channels",
       corevideo::rpc::Json::Array{
           corevideo::rpc::Json::Object{
               {"participantId", "quiet-guest"},
               {"inputLevel", -20},
               {"noiseSuppression", true},
               {"muted", false},
           },
           corevideo::rpc::Json::Object{
               {"participantId", "hot-host"},
               {"inputLevel", 120},
               {"manualGainDb", 40},
               {"noiseSuppression", false},
               {"muted", false},
           },
           corevideo::rpc::Json::Object{
               {"participantId", "muted-panelist"},
               {"inputLevel", 64},
               {"manualGainDb", -40},
               {"noiseSuppression", false},
               {"muted", true},
           },
       }},
  });

  const auto* mix = state.get("audioMixSession");
  ASSERT_NE(mix, nullptr);
  EXPECT_EQ(mix->getString("status"), "warning");
  EXPECT_EQ(mix->get("masterLevel")->asNumber(), 0);
  EXPECT_EQ(mix->get("mixedFrameCount")->asNumber(), 0);
  EXPECT_FALSE(mix->get("limiterActive")->asBool());
  // Real BS.1770 members sit at the silence floor without PCM; anything at or
  // below -59 renders as "LUFS -" in the shell. The old expectation pinned the
  // hardcoded -60 this path used to fake (owner-reported stuck "-16").
  EXPECT_TRUE(mix->get("loudnessLufs")->asNumber() <= -59.0);
  EXPECT_NE(mix->getString("summary").find("waiting for PCM"), std::string::npos);
  EXPECT_EQ(mix->getString("summary").find("boosted"), std::string::npos);
  EXPECT_NE(mix->getString("summary").find("manual"), std::string::npos);
  const auto& warnings = mix->get("warnings")->asArray();
  ASSERT_TRUE(warnings.size() >= 2u);
  EXPECT_TRUE(std::any_of(warnings.begin(), warnings.end(), [](const corevideo::rpc::Json& warning) {
    return warning.isString() && warning.asString().find("No native PCM has been mixed for quiet-guest") != std::string::npos;
  }));

  const auto& participants = mix->get("participants")->asArray();
  ASSERT_TRUE(participants.size() == 3u);
  EXPECT_EQ(participants[0].getString("participantId"), "quiet-guest");
  EXPECT_EQ(participants[0].get("inputLevel")->asNumber(), 0);
  EXPECT_EQ(participants[0].get("gainDb")->asNumber(), 6);
  EXPECT_FALSE(participants[0].get("noiseSuppression")->asBool());
  EXPECT_EQ(participants[0].get("outputLevel")->asNumber(), 0);
  EXPECT_EQ(participants[0].get("rmsDbfs")->asNumber(), -120);
  EXPECT_EQ(participants[0].getString("status"), "waiting-for-pcm");

  EXPECT_EQ(participants[1].getString("participantId"), "hot-host");
  EXPECT_EQ(participants[1].get("inputLevel")->asNumber(), 0);
  EXPECT_EQ(participants[1].get("gainDb")->asNumber(), 12);
  EXPECT_EQ(participants[1].get("manualGainDb")->asNumber(), 24);
  EXPECT_EQ(participants[1].get("outputLevel")->asNumber(), 0);
  EXPECT_FALSE(participants[1].get("limiterActive")->asBool());
  EXPECT_EQ(participants[1].getString("status"), "waiting-for-pcm");

  EXPECT_EQ(participants[2].getString("status"), "muted");
  EXPECT_EQ(participants[2].get("gainDb")->asNumber(), -60);
  EXPECT_EQ(participants[2].get("manualGainDb")->asNumber(), -24);
  EXPECT_EQ(participants[2].get("outputLevel")->asNumber(), 0);
}

TEST(MediaCoreCommand, AudioLimiterBypassStillRequiresPcmForMeters) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "sync-participant-audio-mix"},
      {"limiterEnabled", false},
      {"channels",
       corevideo::rpc::Json::Array{
           corevideo::rpc::Json::Object{
               {"participantId", "hot-host"},
               {"inputLevel", 100},
               {"manualGainDb", 24},
               {"noiseSuppression", false},
               {"muted", false},
           },
       }},
  });

  const auto* mix = state.get("audioMixSession");
  ASSERT_NE(mix, nullptr);
  EXPECT_FALSE(mix->get("limiterEnabled")->asBool());
  EXPECT_FALSE(mix->get("limiterActive")->asBool());
  const auto& participants = mix->get("participants")->asArray();
  ASSERT_TRUE(participants.size() == 1u);
  EXPECT_EQ(participants[0].get("outputLevel")->asNumber(), 0);
  EXPECT_EQ(participants[0].getString("status"), "waiting-for-pcm");
  EXPECT_FALSE(participants[0].get("limiterActive")->asBool());
}

TEST(MediaCoreCommand, SummarizesAudioRoutingGainMatrix) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "sync-audio-routing-matrix"},
      {"sends",
       corevideo::rpc::Json::Array{
           corevideo::rpc::Json::Object{
               {"sourceId", "input-01"},
               {"busId", "pgm-l"},
               {"gainDb", -3.0},
               {"busPluginInserts", corevideo::rpc::Json::Array{"Built-in EQ", "Compressor"}}},
           corevideo::rpc::Json::Object{{"sourceId", "input-01"}, {"busId", "pgm-r"}, {"gainDb", -3.0}},
           corevideo::rpc::Json::Object{{"sourceId", "input-01"}, {"busId", "mon"}, {"gainDb", -6.0}},
           corevideo::rpc::Json::Object{{"sourceId", "input-02"}, {"busId", "pgm-l"}, {"gainDb", 0.0}},
           corevideo::rpc::Json::Object{{"sourceId", "input-02"}, {"busId", "pgm-r"}, {"gainDb", 0.0}},
           corevideo::rpc::Json::Object{{"sourceId", "input-03"}, {"busId", "iso-8"}, {"gainDb", 0.0}},
           corevideo::rpc::Json::Object{{"sourceId", "input-04"}, {"busId", "bus-01"}, {"gainDb", -1.5}},
       }},
  });

  const auto* routing = state.get("audioRoutingMatrix");
  ASSERT_NE(routing, nullptr);
  EXPECT_EQ(routing->getString("status"), "live");
  EXPECT_EQ(routing->get("routedSendCount")->asNumber(), 7);
  EXPECT_EQ(routing->get("routedSourceCount")->asNumber(), 4);

  const auto& busSourceCounts = routing->get("busSourceCounts")->asArray();
  ASSERT_TRUE(busSourceCounts.size() >= 16u);
  const auto findBus = [&](const std::string& busId) -> int {
    for (const auto& bus : busSourceCounts) {
      if (bus.getString("busId") == busId) {
        return static_cast<int>(bus.get("sourceCount")->asNumber());
      }
    }
    return -1;
  };
  EXPECT_EQ(findBus("pgm-l"), 2);
  EXPECT_EQ(findBus("pgm-r"), 2);
  EXPECT_EQ(findBus("mon"), 1);
  EXPECT_EQ(findBus("iso-1"), 0);
  EXPECT_EQ(findBus("iso-8"), 1);
  EXPECT_EQ(findBus("bus-01"), 1);
  EXPECT_EQ(routing->get("sends")->asArray().size(), 7u);
  const auto& firstSendInserts = routing->get("sends")->asArray()[0].get("busPluginInserts")->asArray();
  ASSERT_TRUE(firstSendInserts.size() == 2u);
  EXPECT_EQ(firstSendInserts[0].asString(), "Built-in EQ");

  const auto& busProcessing = routing->get("busProcessing")->asArray();
  const auto pgmLProcessing = std::find_if(busProcessing.begin(), busProcessing.end(), [](const corevideo::rpc::Json& bus) {
    return bus.getString("busId") == "pgm-l";
  });
  ASSERT_TRUE(pgmLProcessing != busProcessing.end());
  EXPECT_EQ(pgmLProcessing->get("pluginInserts")->asArray()[1].asString(), "Compressor");
}

TEST(MediaCoreCommand, ClampsAndWarnsOnInvalidAudioRoutingSends) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "sync-audio-routing-matrix"},
      {"sends",
       corevideo::rpc::Json::Array{
           corevideo::rpc::Json::Object{{"sourceId", "input-01"}, {"busId", "pgm-l"}, {"gainDb", 99.0}},
           corevideo::rpc::Json::Object{{"sourceId", "input-02"}, {"busId", "ghost-bus"}, {"gainDb", 0.0}},
       }},
  });

  const auto* routing = state.get("audioRoutingMatrix");
  ASSERT_NE(routing, nullptr);
  EXPECT_EQ(routing->getString("status"), "warning");
  EXPECT_EQ(routing->get("routedSendCount")->asNumber(), 1);

  const auto& sends = routing->get("sends")->asArray();
  ASSERT_TRUE(sends.size() == 1u);
  EXPECT_EQ(sends[0].getString("sourceId"), "input-01");
  EXPECT_EQ(sends[0].get("gainDb")->asNumber(), 10);

  const auto& warnings = routing->get("warnings")->asArray();
  EXPECT_TRUE(std::any_of(warnings.begin(), warnings.end(), [](const corevideo::rpc::Json& warning) {
    return warning.isString() && warning.asString().find("outside [-60, 10]") != std::string::npos;
  }));
  EXPECT_TRUE(std::any_of(warnings.begin(), warnings.end(), [](const corevideo::rpc::Json& warning) {
    return warning.isString() && warning.asString().find("input-02 is routed to no bus") != std::string::npos;
  }));
}

TEST(MediaCoreCommand, AudioMixSessionFallsBackToNativeMixerMetrics) {
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  corevideo::core::MediaCore mediaCore(std::move(modules));
  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"recording"}},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}},
  });

  const auto* mix = state.get("audioMixSession");
  ASSERT_NE(mix, nullptr);
  EXPECT_TRUE(mix->getString("status") == "live" || mix->getString("status") == "warning");
  EXPECT_TRUE(mix->get("mixedFrameCount")->asNumber() > 0);
  EXPECT_TRUE(mix->get("masterLevel")->asNumber() > 0);
  EXPECT_EQ(mix->get("participants")->asArray().size(), 1u);
  EXPECT_NE(mix->getString("summary").find("native DSP mix"), std::string::npos);
}

TEST(MediaCoreCommand, LowLevelNoiseSuppressionDoesNotMakeAudioMixWarning) {
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<QuietPcmTestZoomSource>();
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"recording"}},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}},
  });

  const auto* mix = state.get("audioMixSession");
  ASSERT_NE(mix, nullptr);
  EXPECT_EQ(mix->getString("status"), "live");
  EXPECT_TRUE(mix->get("mixedFrameCount")->asNumber() > 0);
  EXPECT_TRUE(mix->get("masterLevel")->asNumber() > 0);
  EXPECT_TRUE(mix->get("warnings")->asArray().empty());
  const auto* participant = findParticipantMix(*mix, "quiet-speaker");
  ASSERT_NE(participant, nullptr);
  EXPECT_TRUE(participant->get("noiseSuppression")->asBool());
  EXPECT_EQ(participant->getString("status"), "cleaning");
}

TEST(MediaCoreCommand, AudioMixSessionUsesRealPcmMetersForSyncedChannels) {
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "sync-participant-audio-mix"},
      {"channels",
       corevideo::rpc::Json::Array{
           corevideo::rpc::Json::Object{
               {"participantId", "pcm-speaker"},
               {"inputLevel", 0},
               {"muted", false},
           },
       }},
  });

  const auto* mix = state.get("audioMixSession");
  ASSERT_NE(mix, nullptr);
  const auto* participant = findParticipantMix(*mix, "pcm-speaker");
  ASSERT_NE(participant, nullptr);
  EXPECT_TRUE(participant->get("inputLevel")->asNumber() > 0);
  EXPECT_TRUE(participant->get("outputLevel")->asNumber() > 0);
  EXPECT_TRUE(participant->get("rmsDbfs")->asNumber() > -20.0);
  EXPECT_TRUE(participant->get("peakDbfs")->asNumber() > -10.0);
}

TEST(MediaCoreCommand, MutedPcmChannelPublishesSilentOutputMeters) {
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "sync-participant-audio-mix"},
      {"channels",
       corevideo::rpc::Json::Array{
           corevideo::rpc::Json::Object{
               {"participantId", "pcm-speaker"},
               {"inputLevel", 0},
               {"muted", true},
           },
       }},
  });

  const auto* mix = state.get("audioMixSession");
  ASSERT_NE(mix, nullptr);
  const auto* participant = findParticipantMix(*mix, "pcm-speaker");
  ASSERT_NE(participant, nullptr);
  EXPECT_TRUE(participant->get("muted")->asBool());
  EXPECT_EQ(participant->get("outputLevel")->asNumber(), 0);
  EXPECT_EQ(participant->get("rmsDbfs")->asNumber(), -120.0);
  EXPECT_EQ(participant->get("peakDbfs")->asNumber(), -120.0);
}

// #481: a muted strip still has PCM arriving. The output meters correctly
// read silence (nothing reaches a bus), but the A1 needs to see the guest is
// talking, so the pre-mute INPUT meters must keep reporting real levels.
TEST(MediaCoreCommand, MutedPcmChannelStillPublishesLiveInputMeters) {
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "sync-participant-audio-mix"},
      {"channels",
       corevideo::rpc::Json::Array{
           corevideo::rpc::Json::Object{
               {"participantId", "pcm-speaker"},
               {"inputLevel", 0},
               {"muted", true},
           },
       }},
  });

  const auto* mix = state.get("audioMixSession");
  ASSERT_NE(mix, nullptr);
  const auto* participant = findParticipantMix(*mix, "pcm-speaker");
  ASSERT_NE(participant, nullptr);
  EXPECT_TRUE(participant->get("muted")->asBool());
  // Output meters stay honest: silent.
  EXPECT_EQ(participant->get("rmsDbfs")->asNumber(), -120.0);
  EXPECT_EQ(participant->get("peakDbfs")->asNumber(), -120.0);
  // Input meters, measured before mute/fader, still show the real signal.
  ASSERT_NE(participant->get("inputRmsDbfs"), nullptr);
  ASSERT_NE(participant->get("inputPeakDbfs"), nullptr);
  EXPECT_TRUE(participant->get("inputRmsDbfs")->asNumber() > -20.0);
  EXPECT_TRUE(participant->get("inputPeakDbfs")->asNumber() > -10.0);
}

TEST(AudioDsp, BoundsFramesAndTracksInternalBridgeFaultMetrics) {
  corevideo::modules::AudioFrame frame;
  frame.participantId = "host";
  frame.sampleRate = 384000;
  frame.channels = 0;
  frame.timestampMs = 2000;
  frame.sampleCount = -12;
  frame.rmsLevel = 2.5;
  frame.peakLevel = 1.4;
  frame.noiseFloorDb = -140.0;
  frame.voiceActive = true;

  corevideo::modules::AudioDspTimingReference timing;
  timing.hasPreviousTimestamp = true;
  timing.previousTimestampMs = 1000;
  timing.hasMixReferenceTimestamp = true;
  timing.mixReferenceTimestampMs = 100;

  const auto participant = corevideo::modules::analyzeAudioParticipantFrame(frame, timing);
  EXPECT_EQ(participant.inputLevel, 100);
  EXPECT_TRUE(participant.outputLevel <= 88);
  EXPECT_TRUE(participant.rmsLevel <= 1.0);
  EXPECT_TRUE(participant.peakLevel <= 1.0);
  EXPECT_GE(participant.noiseFloorDb, -96.0);
  EXPECT_TRUE(participant.underrunDetected);
  EXPECT_TRUE(participant.clippingDetected);
  EXPECT_FALSE(participant.silenceDetected);
  EXPECT_EQ(participant.avSyncOffsetMs, 500);
  EXPECT_EQ(participant.timingDriftMs, 500);

  const auto session = corevideo::modules::summarizeAudioMixMetrics({participant}, 1);
  EXPECT_EQ(session.underrunCount, 1);
  EXPECT_EQ(session.clippingCount, 1);
  EXPECT_EQ(session.silenceCount, 0);
  EXPECT_EQ(session.maxAbsAvSyncOffsetMs, 500);
  EXPECT_TRUE(session.warnings.size() >= 2u);
}

TEST(AudioDsp, TracksSilenceWithoutLeakingInternalMetricsIntoPublicAudioMixShape) {
  corevideo::modules::AudioFrame frame;
  frame.participantId = "muted-guest";
  frame.voiceActive = false;
  frame.rmsLevel = 0.0;
  frame.peakLevel = 0.0;

  const auto participant = corevideo::modules::analyzeAudioParticipantFrame(frame);
  EXPECT_TRUE(participant.silenceDetected);
  EXPECT_TRUE(participant.muted);
  const auto session = corevideo::modules::summarizeAudioMixMetrics({participant}, 1);
  EXPECT_EQ(session.silenceCount, 1);
  EXPECT_EQ(session.status, "warning");

  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  corevideo::core::MediaCore mediaCore(std::move(modules));
  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"recording"}},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}},
  });
  const auto* mix = state.get("audioMixSession");
  ASSERT_NE(mix, nullptr);
  EXPECT_EQ(mix->get("underrunCount"), nullptr);
  EXPECT_EQ(mix->get("clippingCount"), nullptr);
  EXPECT_EQ(mix->get("silenceCount"), nullptr);
  ASSERT_FALSE(mix->get("participants")->asArray().empty());
  EXPECT_EQ(mix->get("participants")->asArray()[0].get("avSyncOffsetMs"), nullptr);
  EXPECT_EQ(mix->get("participants")->asArray()[0].get("timingDriftMs"), nullptr);
}

TEST(MediaCoreCommand, ReportsEncoderMetadataInHealthAndSession) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"recording"}},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}},
  });

  EXPECT_TRUE(state.get("active")->asBool());
  EXPECT_FALSE(state.getString("encoder").empty());
  EXPECT_EQ(state.getString("codec"), "h264");
  const auto health = mediaCore.health();
#if COREVIDEO_WITH_MF_ENCODER
  EXPECT_EQ(health.getString("encoder"), "media-foundation");
  EXPECT_TRUE(health.get("hardwareEncoder")->asBool());
#elif COREVIDEO_WITH_AVF_ENCODER
  EXPECT_EQ(health.getString("encoder"), "videotoolbox");
  EXPECT_TRUE(health.get("hardwareEncoder")->asBool());
#else
  EXPECT_EQ(health.getString("encoder"), "software-counting");
  EXPECT_FALSE(health.get("hardwareEncoder")->asBool());
#endif
}

TEST(MediaCoreCommand, AppliesEncoderLifecycleAndRecordingCommands) {
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  corevideo::core::MediaCore mediaCore(std::move(modules));
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "prepare-encoder-session"},
          {"preparedAtMs", 1000},
          {"reason", "GPU encoder warmup."},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-program-output"},
          {"destinations", corevideo::rpc::Json::Array{"recording", "rtmp"}},
          {"isoParticipantIds", corevideo::rpc::Json::Array{"participant-1"}},
          {"streamOutputProfile",
           corevideo::rpc::Json::Object{
               {"profileId", "stream-1080p60"},
               {"resolution", "1920x1080"},
               {"width", 1920},
               {"height", 1080},
               {"fps", 60},
               {"targetBitrateMbps", 8.0},
               {"codec", "h265"},
           }},
          {"recordingOutputProfile",
           corevideo::rpc::Json::Object{
               {"profileId", "recording-1080p60-av1"},
               {"resolution", "1920x1080"},
               {"width", 1920},
               {"height", 1080},
               {"fps", 60},
               {"targetBitrateMbps", 8.0},
               {"codec", "av1"},
           }},
      },
      corevideo::rpc::Json::Object{
          {"type", "set-recording-targets"},
          {"targetFolder", "Recordings/CoreVideo Pro/native-core"},
          {"filenamePrefix", "program"},
          {"format", "mp4"},
          {"quality", "high"},
          {"isoParticipantIds", corevideo::rpc::Json::Array{"participant-1"}},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-recording-session"},
          {"sessionId", "show-1"},
          {"startedAtMs", 1010},
      },
  });

  const auto* encoderSession = state.get("encoderSession");
  ASSERT_NE(encoderSession, nullptr);
  EXPECT_EQ(encoderSession->getString("status"), "encoding");
  EXPECT_EQ(encoderSession->get("lifecycle")->getString("status"), "encoding");
  EXPECT_GE(encoderSession->get("programFrameCount")->asNumber(), 1);
  EXPECT_GE(encoderSession->get("targets")->asArray().size(), 2);

  const auto* recording = state.get("recording");
  ASSERT_NE(recording, nullptr);
  EXPECT_EQ(recording->getString("sessionId"), "show-1");
  EXPECT_EQ(recording->getString("status"), "recording");
  EXPECT_EQ(recording->getString("writerStatus"), "writing");
  EXPECT_EQ(recording->get("encoder")->getString("codec"), "av1");
  EXPECT_GE(recording->get("totalFramesWritten")->asNumber(), 1);
  EXPECT_GE(recording->get("streams")->asArray().size(), 2);

  const auto* proof = recording->get("proof");
  ASSERT_NE(proof, nullptr);
  EXPECT_GE(proof->get("durationMs")->asNumber(), 16);
  EXPECT_GE(proof->get("programFrameCount")->asNumber(), 1);
  // The stub has no per-ISO writer telemetry. Unknown must remain zero instead
  // of cloning Program's count into a stem that may not exist.
  EXPECT_EQ(proof->get("isoFrameCount")->asNumber(), 0);
  EXPECT_GE(proof->get("audioPacketsObserved")->asNumber(), 1);
  EXPECT_TRUE(proof->get("audioPresent")->asBool());
  EXPECT_TRUE(proof->get("metadataValid")->asBool());
  EXPECT_EQ(proof->getString("containerFormat"), "mp4");
  EXPECT_EQ(proof->getString("videoCodec"), "av1");
  EXPECT_EQ(proof->getString("audioCodec"), "aac");
  EXPECT_EQ(proof->get("width")->asNumber(), 1920);
  EXPECT_EQ(proof->get("height")->asNumber(), 1080);
  EXPECT_EQ(proof->get("frameRate")->asNumber(), 60);

  const auto& programStream = recording->get("streams")->asArray()[0];
  EXPECT_GE(programStream.get("durationMs")->asNumber(), 16);
  EXPECT_TRUE(programStream.get("hasAudio")->asBool());
  EXPECT_GE(programStream.get("audioSamples")->asNumber(), 1);
  EXPECT_TRUE(programStream.get("metadataValid")->asBool());
}

TEST(MediaCoreCommand, AppliesRecordingFailureAndRecoveryCommands) {
  corevideo::core::MediaCore mediaCore;
  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-recording-session"},
      {"sessionId", "show-1"},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}},
  });

  auto failed = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "fail-recording-session"},
      {"message", "Encoder process exited."},
  });
  ASSERT_NE(failed.get("recording"), nullptr);
  EXPECT_EQ(failed.get("recording")->getString("status"), "failed");
  EXPECT_EQ(failed.get("recording")->getString("writerStatus"), "failed");
  EXPECT_EQ(failed.get("recording")->getString("error"), "Encoder process exited.");
  EXPECT_EQ(failed.get("recording")->getString("lastFailure"), "Encoder process exited.");
  ASSERT_NE(failed.get("recording")->get("proof"), nullptr);
  EXPECT_EQ(failed.get("recording")->get("proof")->get("failureCount")->asNumber(), 1);

  auto recovered = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "recover-recording-session"},
      {"reason", "Recording writer recovered."},
  });
  ASSERT_NE(recovered.get("recording"), nullptr);
  EXPECT_EQ(recovered.get("recording")->getString("status"), "recording");
  EXPECT_EQ(recovered.get("recording")->getString("writerStatus"), "writing");
  EXPECT_EQ(recovered.get("recording")->getString("warning"), "Recording writer recovered.");
  EXPECT_EQ(recovered.get("recording")->getString("lastRecovery"), "Recording writer recovered.");
  ASSERT_NE(recovered.get("recording")->get("proof"), nullptr);
  EXPECT_EQ(recovered.get("recording")->get("proof")->get("failureCount")->asNumber(), 1);
  EXPECT_EQ(recovered.get("recording")->get("proof")->get("recoveryCount")->asNumber(), 1);
}

TEST(MediaCoreCommand, SyncsAudioMonitorState) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-monitor"},
          {"enabled", true},
          {"deviceId", "render-01"},
          {"deviceName", "Studio Headphones"},
          {"volume", 0.75},
      },
      corevideo::rpc::Json::Object{
          {"type", "sync-participant-audio-mix"},
          {"limiterEnabled", true},
          {"channels",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{
                   {"participantId", "participant-1"},
                   {"inputLevel", 68},
                   {"muted", false},
               },
           }},
      },
  });

  const auto* audio = state.get("audioMixSession");
  ASSERT_NE(audio, nullptr);
  EXPECT_TRUE(audio->get("monitorEnabled")->asBool());
  EXPECT_EQ(audio->getString("monitorDeviceId"), "render-01");
  EXPECT_EQ(audio->getString("monitorDeviceName"), "Studio Headphones");
  EXPECT_EQ(audio->get("monitorVolume")->asNumber(), 0.75);
  EXPECT_TRUE(audio->getString("monitorStatus") == "armed" || audio->getString("monitorStatus") == "playing" ||
              audio->getString("monitorStatus") == "stub-monitor" || audio->getString("monitorStatus") == "unavailable");
}

TEST(MediaCoreAudioMonitor, MixerSumsParticipantPcmIntoStereoMonitorBus) {
  auto modules = corevideo::modules::createStubModules();

  corevideo::modules::AudioFrame mono;
  mono.participantId = "mono";
  mono.channels = 1;
  mono.sampleCount = 4;
  mono.pcm = {0.25f, 0.25f, 0.25f, 0.25f};

  corevideo::modules::AudioFrame stereo;
  stereo.participantId = "stereo";
  stereo.channels = 2;
  stereo.sampleCount = 4;
  stereo.pcm = {0.10f, -0.10f, 0.10f, -0.10f, 0.10f, -0.10f, 0.10f, -0.10f};

  modules.mixer->mix({mono, stereo});

  EXPECT_EQ(modules.mixer->monitorBusChannels(), 2);
  EXPECT_EQ(modules.mixer->monitorBusSampleRate(), 48000);
  const auto& bus = modules.mixer->monitorBusPcm();
  ASSERT_TRUE(bus.size() == 8u);  // 4 sample-frames * 2 channels
  // Peak (0.35) sits below the -1 dBFS limiter threshold, so sums pass through.
  EXPECT_TRUE(std::fabs(bus[0] - 0.35f) < 1e-4f);   // L: mono 0.25 + stereo.L 0.10
  EXPECT_TRUE(std::fabs(bus[1] - 0.15f) < 1e-4f);   // R: mono 0.25 + stereo.R -0.10
}

TEST(MediaCoreAudioMonitor, MixerMonitorBusIsEmptyWithoutPcm) {
  auto modules = corevideo::modules::createStubModules();

  corevideo::modules::AudioFrame metadataOnly;
  metadataOnly.participantId = "metadata";
  metadataOnly.channels = 1;
  metadataOnly.sampleCount = 480;
  metadataOnly.rmsLevel = 0.5;

  modules.mixer->mix({metadataOnly});

  EXPECT_TRUE(modules.mixer->monitorBusPcm().empty());
}

TEST(MediaCoreAudioMonitor, RendersMonitorBusThroughOutputDeviceAtOperatorVolume) {
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  auto* monitor = new RecordingMonitorOutput();
  modules.monitorOutput.reset(monitor);
  corevideo::core::MediaCore mediaCore{std::move(modules)};

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-monitor"},
          {"enabled", true},
          {"deviceId", "render-stereo"},
          {"deviceName", "Studio Monitors"},
          {"volume", 0.5},
      },
  });

  const auto* audio = state.get("audioMixSession");
  ASSERT_NE(audio, nullptr);
  EXPECT_EQ(audio->getString("monitorStatus"), "playing");
  EXPECT_TRUE(audio->get("monitorFramesPlayed")->asNumber() > 0);
  const auto* captureAudioSources = state.get("captureAudioSources");
  ASSERT_NE(captureAudioSources, nullptr);
  EXPECT_TRUE(captureAudioSources->get("fallbackMonitorFrames")->asNumber() > 0);
  EXPECT_EQ(captureAudioSources->get("routedMonitorFrames")->asNumber(), 0);

  EXPECT_TRUE(monitor->active());
  EXPECT_GE(monitor->startCount, 1);
  EXPECT_EQ(monitor->lastDeviceId, "render-stereo");
  EXPECT_TRUE(monitor->framesRendered > 0);
  EXPECT_EQ(monitor->lastChannels, 2);
  EXPECT_TRUE(std::fabs(monitor->lastVolume - 0.5) < 1e-6);
}

TEST(MediaCoreAudioMonitor, EmptyMonitorDeviceUsesSystemDefaultOutput) {
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  auto* monitor = new RecordingMonitorOutput();
  modules.monitorOutput.reset(monitor);
  corevideo::core::MediaCore mediaCore{std::move(modules)};

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-monitor"},
          {"enabled", true},
          {"deviceId", ""},
          {"deviceName", ""},
          {"volume", 0.5},
      },
  });

  const auto* audio = state.get("audioMixSession");
  ASSERT_NE(audio, nullptr);
  EXPECT_EQ(audio->getString("monitorStatus"), "playing");
  EXPECT_EQ(audio->getString("monitorDeviceId"), "");
  EXPECT_EQ(audio->getString("monitorDeviceName"), "System default output");
  EXPECT_TRUE(audio->get("monitorFramesPlayed")->asNumber() > 0);
  EXPECT_GE(monitor->startCount, 1);
  EXPECT_EQ(monitor->lastDeviceId, "");
  EXPECT_TRUE(monitor->framesRendered > 0);
}

TEST(MediaCoreAudioMonitor, TransientRenderFailureReportsDroppingAndClearsAfterRecovery) {
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  auto* monitor = new RecoveringMonitorOutput();
  modules.monitorOutput.reset(monitor);
  corevideo::core::MediaCore mediaCore{std::move(modules)};

  const auto dropping = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-monitor"},
          {"enabled", true},
          {"deviceId", "render-stereo"},
          {"deviceName", "Studio Monitors"},
          {"volume", 0.5},
      },
  });

  const auto* droppingAudio = dropping.get("audioMixSession");
  ASSERT_NE(droppingAudio, nullptr);
  EXPECT_EQ(droppingAudio->getString("monitorStatus"), "dropping");
  EXPECT_TRUE(jsonArrayContains(
      *droppingAudio->get("warnings"),
      "Native audio monitor accepted no frames this tick; endpoint buffer may be full."));

  const auto recovered = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "sync-participant-audio-mix"},
      {"channels", corevideo::rpc::Json::Array{}},
  });

  const auto* recoveredAudio = recovered.get("audioMixSession");
  ASSERT_NE(recoveredAudio, nullptr);
  EXPECT_EQ(recoveredAudio->getString("monitorStatus"), "playing");
  EXPECT_TRUE(recoveredAudio->get("monitorFramesPlayed")->asNumber() > 0);
  EXPECT_FALSE(jsonArrayContains(
      *recoveredAudio->get("warnings"),
      "Native audio monitor accepted no frames this tick; endpoint buffer may be full."));
  EXPECT_GE(monitor->renderCalls, 2);
  EXPECT_TRUE(monitor->framesRendered > 0);
}

TEST(MediaCoreAudioMonitor, DisablingMonitorStopsTheOutputDevice) {
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  auto* monitor = new RecordingMonitorOutput();
  modules.monitorOutput.reset(monitor);
  corevideo::core::MediaCore mediaCore{std::move(modules)};

  const auto armed = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-monitor"},
          {"enabled", true},
          {"deviceId", "render-stereo"},
          {"volume", 0.8},
      },
  });
  (void)armed;
  EXPECT_TRUE(monitor->active());

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-monitor"},
          {"enabled", false},
      },
  });
  EXPECT_FALSE(monitor->active());
  EXPECT_GE(monitor->stopCount, 1);
  ASSERT_NE(state.get("audioMixSession"), nullptr);
  EXPECT_EQ(state.get("audioMixSession")->getString("monitorStatus"), "muted");
}

TEST(MediaCoreAudioMonitor, RoutingMatrixMixesPcmIntoProgramAndIsoTaps) {
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  corevideo::core::MediaCore mediaCore{std::move(modules)};

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-routing-matrix"},
          {"sends",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{{"sourceId", "pcm-speaker"}, {"busId", "master"}, {"gainDb", 0}},
               corevideo::rpc::Json::Object{{"sourceId", "pcm-speaker"}, {"busId", "iso-1"}, {"gainDb", 0}},
           }},
      },
  });

  const auto* matrix = state.get("audioRoutingMatrix");
  ASSERT_NE(matrix, nullptr);
  const auto* busTaps = matrix->get("busTaps");
  ASSERT_NE(busTaps, nullptr);

  bool sawMaster = false;
  bool sawIso = false;
  for (const auto& tap : busTaps->asArray()) {
    if (tap.getString("busId") == "master") {
      sawMaster = true;
      EXPECT_EQ(tap.get("frames")->asNumber(), 480);  // PcmTestZoomSource emits 480 frames
      EXPECT_EQ(tap.get("channels")->asNumber(), 2);
    }
    if (tap.getString("busId") == "iso-1") {
      sawIso = true;
    }
  }
  EXPECT_TRUE(sawMaster);
  EXPECT_TRUE(sawIso);
  EXPECT_EQ(matrix->get("programTapFrames")->asNumber(), 480);

  // The program tap accessor exposes the master bus as interleaved stereo PCM.
  EXPECT_TRUE(mediaCore.programAudioTapPcm().size() == 960u);  // 480 frames * 2 channels
  const auto busIds = mediaCore.routedAudioBusIds();
  EXPECT_TRUE(std::find(busIds.begin(), busIds.end(), "master") != busIds.end());
  EXPECT_TRUE(std::find(busIds.begin(), busIds.end(), "iso-1") != busIds.end());
}

TEST(MediaCoreCommand, RecordsRealProgramAudioPcmIntoMux) {
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  corevideo::core::MediaCore mediaCore(std::move(modules));
  const auto state = mediaCore.applyCommands(
      corevideo::rpc::Json::Array{
          corevideo::rpc::Json::Object{
              {"type", "start-program-output"},
              {"destinations", corevideo::rpc::Json::Array{"recording"}},
              {"isoParticipantIds", corevideo::rpc::Json::Array{}},
          },
          corevideo::rpc::Json::Object{
              {"type", "set-recording-targets"},
              {"targetFolder", "Recordings/CoreVideo Pro/native-core"},
              {"filenamePrefix", "program"},
              {"format", "mp4"},
              {"quality", "high"},
          },
          corevideo::rpc::Json::Object{
              {"type", "start-recording-session"},
              {"sessionId", "audio-proof"},
              {"startedAtMs", 1000},
          },
      },
      200.0);  // ~6 ticks of real program audio

  const auto* recording = state.get("recording");
  ASSERT_NE(recording, nullptr);
  const auto* proof = recording->get("proof");
  ASSERT_NE(proof, nullptr);
  EXPECT_TRUE(proof->get("audioPresent")->asBool());
  EXPECT_GE(proof->get("audioPacketsObserved")->asNumber(), 1);
  EXPECT_TRUE(proof->get("audioSampleCount")->asNumber() > 0);  // real muxed PCM, not a frame counter
  EXPECT_EQ(proof->get("audioChannels")->asNumber(), 2);
  EXPECT_EQ(proof->get("audioSampleRate")->asNumber(), 48000);
}

TEST(MediaCoreCommand, MeasuresBs1770MasterLoudnessOnProgramTap) {
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<SinePcmTestZoomSource>();
  corevideo::core::MediaCore mediaCore(std::move(modules));
  const auto state = mediaCore.applyCommands(
      corevideo::rpc::Json::Array{
          corevideo::rpc::Json::Object{
              {"type", "start-program-output"},
              {"destinations", corevideo::rpc::Json::Array{"recording"}},
              {"isoParticipantIds", corevideo::rpc::Json::Array{}},
          },
      },
      2500.0);  // enough ticks to fill the 400 ms gating block + window

  const auto* mix = state.get("audioMixSession");
  ASSERT_NE(mix, nullptr);
  const auto* meter = mix->get("masterMeter");
  ASSERT_NE(meter, nullptr);
  // A continuous program tone reads a sane loudness/true-peak (present, below FS).
  EXPECT_TRUE(meter->get("momentaryLufs")->asNumber() > -50.0);
  EXPECT_TRUE(meter->get("momentaryLufs")->asNumber() < 0.0);
  EXPECT_TRUE(meter->get("shortTermLufs")->asNumber() > -50.0);
  EXPECT_TRUE(meter->get("integratedLufs")->asNumber() > -50.0);
  EXPECT_TRUE(meter->get("truePeakDbfs")->asNumber() > -40.0);
  EXPECT_TRUE(meter->get("truePeakDbfs")->asNumber() <= 1.0);
  EXPECT_TRUE(meter->get("windowMs")->asNumber() > 0);
}

TEST(MediaCoreAudioMonitor, BusInsertCompressorActsOnRoutedBusPcm) {
  // Route the same PCM source to the master bus with and without a compressor
  // insert, then compare the resulting program-tap peak. The insert must shape
  // the real bus samples, not merely live as routing state.
  const auto programTapPeak = [](const corevideo::rpc::Json::Object& send) -> double {
    auto modules = corevideo::modules::createStubModules();
    modules.zoom = std::make_unique<PcmTestZoomSource>();
    corevideo::core::MediaCore mediaCore{std::move(modules)};
    (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
        corevideo::rpc::Json::Object{
            {"type", "sync-audio-routing-matrix"},
            {"sends", corevideo::rpc::Json::Array{send}},
        },
    });
    double peak = 0.0;
    for (const float sample : mediaCore.programAudioTapPcm()) {
      peak = std::max(peak, std::fabs(static_cast<double>(sample)));
    }
    return peak;
  };

  const double plainPeak =
      programTapPeak(corevideo::rpc::Json::Object{{"sourceId", "pcm-speaker"}, {"busId", "master"}, {"gainDb", 0}});
  const double compressedPeak = programTapPeak(corevideo::rpc::Json::Object{
      {"sourceId", "pcm-speaker"},
      {"busId", "master"},
      {"gainDb", 0},
      {"busPluginInserts", corevideo::rpc::Json::Array{"Compressor"}},
  });

  EXPECT_TRUE(plainPeak > 0.0);
  EXPECT_TRUE(compressedPeak < plainPeak);  // the compressor reduced the bus peak
}

TEST(MediaCoreCommand, SyncsNetworkOutputSenderSession) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"recording", "rtmp"}},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}},
  });

  const auto* senderSession = state.get("outputSenderSession");
  ASSERT_NE(senderSession, nullptr);
#if COREVIDEO_WITH_RTMP_OUTPUT
  EXPECT_TRUE(senderSession->getString("status") == "live" || senderSession->getString("status") == "warning");
#else
  EXPECT_EQ(senderSession->getString("status"), "live");
#endif
  EXPECT_EQ(senderSession->get("activeSenderCount")->asNumber(), 1);
  ASSERT_TRUE(senderSession->get("senders")->asArray().size() == 1);
  const auto& sender = senderSession->get("senders")->asArray()[0];
  EXPECT_EQ(sender.getString("destination"), "rtmp");
#if COREVIDEO_WITH_RTMP_OUTPUT
  EXPECT_TRUE(sender.getString("status") == "live" || sender.getString("status") == "warning");
#else
  EXPECT_EQ(sender.getString("status"), "live");
  EXPECT_GE(sender.get("framesSent")->asNumber(), 1);
  EXPECT_EQ(sender.getString("destinationHealth"), "ok");
  EXPECT_EQ(sender.getString("lastResultCode"), "ok");
  EXPECT_GE(sender.get("bytesSent")->asNumber(), 1);
#endif
}

TEST(MediaCoreCommand, AppliesOutputSenderFailureAndRecoveryCommands) {
  corevideo::core::MediaCore mediaCore;
  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"rtmp"}},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}},
  });

  const auto failed = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "fail-output-sender"},
      {"destination", "rtmp"},
      {"message", "RTMP ingest rejected credentials."},
      {"failedAtMs", 2000},
  });
  const auto* failedSession = failed.get("outputSenderSession");
  ASSERT_NE(failedSession, nullptr);
  EXPECT_EQ(failedSession->getString("status"), "failed");
  ASSERT_TRUE(failedSession->get("senders")->asArray().size() == 1);
  EXPECT_EQ(failedSession->get("senders")->asArray()[0].getString("status"), "failed");
  EXPECT_EQ(failedSession->get("senders")->asArray()[0].getString("warning"), "RTMP ingest rejected credentials.");
  EXPECT_EQ(failedSession->get("senders")->asArray()[0].getString("destinationHealth"), "failed");
  EXPECT_EQ(failedSession->get("senders")->asArray()[0].getString("lastResultCode"), "failed");
  EXPECT_EQ(failedSession->get("senders")->asArray()[0].getString("lastError"), "RTMP ingest rejected credentials.");

  const auto recovered = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "recover-output-sender"},
      {"destination", "rtmp"},
      {"reason", "RTMP sender recovered."},
      {"recoveredAtMs", 2500},
  });
  const auto* recoveredSession = recovered.get("outputSenderSession");
  ASSERT_NE(recoveredSession, nullptr);
  EXPECT_EQ(recoveredSession->getString("status"), "warning");
  ASSERT_TRUE(recoveredSession->get("senders")->asArray().size() == 1);
#if COREVIDEO_WITH_RTMP_OUTPUT
  EXPECT_TRUE(recoveredSession->get("senders")->asArray()[0].getString("status") == "starting" ||
              recoveredSession->get("senders")->asArray()[0].getString("status") == "warning");
#else
  EXPECT_EQ(recoveredSession->get("senders")->asArray()[0].getString("status"), "starting");
#endif
  EXPECT_EQ(recoveredSession->get("senders")->asArray()[0].getString("warning"), "RTMP sender recovered.");
  EXPECT_EQ(recoveredSession->get("senders")->asArray()[0].getString("lastResultCode"), "recovered");
}

TEST(OutputSenderAdapter, StubKeepsMultiDestinationDiagnosticsIsolated) {
  auto modules = corevideo::modules::createStubModules();
  corevideo::modules::ProgramFrame frame;
  frame.width = 1920;
  frame.height = 1080;
  frame.layerCount = 2;
  frame.frameNumber = 7;
  frame.renderPlanId = "multi-destination-plan";

  auto session = modules.outputSender->sync({"rtmp", "srt"}, &frame, 33);
  ASSERT_TRUE(session.senders.size() == 2);
  EXPECT_EQ(session.senders[0].destination, "rtmp");
  EXPECT_EQ(session.senders[0].destinationHealth, "ok");
  EXPECT_EQ(session.senders[0].lastResultCode, "ok");
  EXPECT_EQ(session.senders[1].destination, "srt");
  EXPECT_EQ(session.senders[1].destinationHealth, "ok");
  EXPECT_EQ(session.senders[1].lastResultCode, "ok");

  session = modules.outputSender->fail("rtmp", "RTMP ingest rejected credentials.", 66);
  ASSERT_TRUE(session.senders.size() == 2);
  EXPECT_EQ(session.status, "failed");
  EXPECT_EQ(session.activeSenderCount, 1);
  EXPECT_EQ(session.senders[0].destination, "rtmp");
  EXPECT_EQ(session.senders[0].status, "failed");
  EXPECT_EQ(session.senders[0].destinationHealth, "failed");
  EXPECT_EQ(session.senders[0].lastResultCode, "failed");
  EXPECT_EQ(session.senders[0].lastError, "RTMP ingest rejected credentials.");
  EXPECT_EQ(session.senders[1].destination, "srt");
  EXPECT_EQ(session.senders[1].status, "live");
  EXPECT_EQ(session.senders[1].destinationHealth, "ok");
  EXPECT_EQ(session.senders[1].lastResultCode, "ok");

  frame.frameNumber = 8;
  session = modules.outputSender->sync({"rtmp", "srt", "ndi"}, &frame, 99);
  ASSERT_TRUE(session.senders.size() == 3);
  EXPECT_EQ(session.activeSenderCount, 2);
  EXPECT_EQ(session.senders[0].destination, "ndi");
  EXPECT_EQ(session.senders[0].framesSent, 1);
  EXPECT_EQ(session.senders[0].destinationHealth, "ok");
  EXPECT_EQ(session.senders[1].destination, "rtmp");
  EXPECT_EQ(session.senders[1].status, "failed");
  EXPECT_EQ(session.senders[1].framesSent, 1);
  EXPECT_EQ(session.senders[2].destination, "srt");
  EXPECT_EQ(session.senders[2].status, "live");
  EXPECT_EQ(session.senders[2].framesSent, 2);
  EXPECT_TRUE(session.senders[2].bytesSent > session.senders[1].bytesSent);
}

TEST(OutputSenderAdapter, DefaultSenderWarnsWhenRequestedDestinationHasNoModule) {
#if COREVIDEO_WITH_RTMP_OUTPUT && !COREVIDEO_WITH_NDI_OUTPUT
  auto modules = corevideo::modules::createDefaultModules();
  corevideo::modules::ProgramFrame frame;
  frame.width = 1920;
  frame.height = 1080;
  frame.frameNumber = 9;
  frame.renderPlanId = "missing-ndi-output-plan";

  const auto session = modules.outputSender->sync({"ndi"}, &frame, 123);

  ASSERT_FALSE(session.senders.empty());
  EXPECT_EQ(session.status, "warning");
  EXPECT_EQ(session.senders[0].destination, "ndi");
  EXPECT_EQ(session.senders[0].status, "warning");
  EXPECT_EQ(session.senders[0].lastResultCode, "ndi-output-unavailable");
  EXPECT_TRUE(session.senders[0].warning.find("NDI output is selected") != std::string::npos);

  const auto polledSession = modules.outputSender->session();
  ASSERT_FALSE(polledSession.senders.empty());
  EXPECT_EQ(polledSession.status, "warning");
  EXPECT_EQ(polledSession.senders[0].destination, "ndi");
  EXPECT_EQ(polledSession.senders[0].status, "warning");
  EXPECT_EQ(polledSession.senders[0].lastResultCode, "ndi-output-unavailable");
  EXPECT_TRUE(polledSession.senders[0].warning.find("NDI output is selected") != std::string::npos);
#else
  EXPECT_TRUE(true);
#endif
}

TEST(OutputSenderAdapter, SyncAcceptsRealProgramAudioWithoutChangingDiagnostics) {
  // The IOutputSender::sync boundary now carries the real program-audio mix. The
  // synthetic stub sender must accept the extra audio params and keep identical
  // diagnostics (it does not encode), so existing output-sender behavior holds.
  auto modules = corevideo::modules::createStubModules();
  corevideo::modules::ProgramFrame frame;
  frame.width = 1920;
  frame.height = 1080;
  frame.frameNumber = 5;
  frame.renderPlanId = "audio-aware-plan";

  const std::vector<float> programAudioPcm(960, 0.25f);  // 480 stereo frames
  auto withAudio =
      modules.outputSender->sync({"rtmp"}, &frame, 33, {}, &programAudioPcm, 2, 48000);
  ASSERT_TRUE(withAudio.senders.size() == 1u);
  EXPECT_EQ(withAudio.senders[0].destination, "rtmp");
  EXPECT_EQ(withAudio.senders[0].destinationHealth, "ok");
  EXPECT_EQ(withAudio.senders[0].lastResultCode, "ok");
  EXPECT_EQ(withAudio.senders[0].audioFramesSent, 480);
  EXPECT_EQ(withAudio.senders[0].audioBytesSent, static_cast<int64_t>(programAudioPcm.size() * sizeof(float)));
  EXPECT_EQ(withAudio.senders[0].audioChannels, 2);
  EXPECT_EQ(withAudio.senders[0].audioSampleRate, 48000);

  // Same call without audio (default args) yields the same health/result.
  auto withoutAudio = corevideo::modules::createStubModules().outputSender->sync({"rtmp"}, &frame, 33);
  ASSERT_TRUE(withoutAudio.senders.size() == 1u);
  EXPECT_EQ(withoutAudio.senders[0].destinationHealth, withAudio.senders[0].destinationHealth);
  EXPECT_EQ(withoutAudio.senders[0].lastResultCode, withAudio.senders[0].lastResultCode);
  EXPECT_EQ(withoutAudio.senders[0].audioFramesSent, 0);
  EXPECT_EQ(withoutAudio.senders[0].audioBytesSent, 0);
  EXPECT_EQ(withoutAudio.senders[0].audioChannels, 0);
  EXPECT_EQ(withoutAudio.senders[0].audioSampleRate, 0);
}

TEST(MediaCoreCommand, StreamOutputPrefersRoutedStreamBusAudio) {
  auto modules = corevideo::modules::createStubModules();
  auto* outputSender = new CapturingOutputSender();
  modules.outputSender.reset(outputSender);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-capture-audio-sources"},
          {"sources",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{
                   {"captureDeviceId", "local-machine-audio"},
                   {"audioDeviceId", "system-loopback"},
                   {"audioDeviceName", "System audio loopback"},
                   {"audioSourceKind", "wasapi-loopback"},
                   {"nativeAudioDeviceId", "default-render"},
                   {"audioDriverName", "WASAPI"},
               },
           }},
      },
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-routing-matrix"},
          {"sends",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{{"sourceId", "local-machine-audio"}, {"busId", "master"}, {"gainDb", -20}},
               corevideo::rpc::Json::Object{{"sourceId", "local-machine-audio"}, {"busId", "stream"}, {"gainDb", 0}},
               corevideo::rpc::Json::Object{{"sourceId", "local-machine-audio"}, {"busId", "mon"}, {"gainDb", 0}},
           }},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-program-output"},
          {"destinations", corevideo::rpc::Json::Array{"rtmp"}},
      },
  });

  const auto peakOf = [](const std::vector<float>& samples) {
    float peak = 0.f;
    for (const auto sample : samples) {
      peak = std::max(peak, std::abs(sample));
    }
    return peak;
  };

  const auto& master = mediaCore.programAudioTapPcm();
  const auto& stream = mediaCore.audioBusTapPcm("stream");
  ASSERT_FALSE(master.empty());
  ASSERT_FALSE(stream.empty());
  EXPECT_TRUE(peakOf(stream) > peakOf(master) * 5.f);
  EXPECT_EQ(outputSender->destinations_, std::vector<std::string>{"rtmp"});
  EXPECT_EQ(outputSender->audioFramesSent_, static_cast<int>(stream.size() / 2u));
  EXPECT_EQ(outputSender->audioChannels_, 2);
  EXPECT_EQ(outputSender->audioSampleRate_, 48000);
  EXPECT_TRUE(std::abs(outputSender->maxAudioSample_ - peakOf(stream)) <= 0.0001f);

  const auto* captureSources = state.get("captureAudioSources");
  ASSERT_NE(captureSources, nullptr);
  EXPECT_TRUE(captureSources->get("routedMasterFrames")->asNumber() > 0);
  EXPECT_TRUE(captureSources->get("routedStreamFrames")->asNumber() > 0);
}

TEST(MediaCoreCommand, SceneMediaAudioRoutesToStreamOutput) {
  auto modules = corevideo::modules::createStubModules();
  SolidMediaFrameSource::reset();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
  auto* outputSender = new CapturingOutputSender();
  modules.outputSender.reset(outputSender);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "media-program-audio"},
          {"routes",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{
                   {"routeId", "media-main"},
                   {"mode", "fixed"},
                   {"mediaAssetId", "clip-intro"},
                   {"mediaAssetName", "Intro"},
                   {"mediaAssetKind", "video"},
                   {"mediaAssetPath", "C:\\media\\intro.mp4"},
                   {"mediaPlaybackKey", "program-take:9:media:clip-intro"},
                   {"mediaAssetPlaying", true},
                   {"rect", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}},
               },
           }},
      },
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-routing-matrix"},
          {"sends",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{{"sourceId", "media"}, {"busId", "master"}, {"gainDb", -18}},
               corevideo::rpc::Json::Object{{"sourceId", "media"}, {"busId", "stream"}, {"gainDb", 0}},
               corevideo::rpc::Json::Object{{"sourceId", "media"}, {"busId", "mon"}, {"gainDb", 0}},
           }},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-program-output"},
          {"destinations", corevideo::rpc::Json::Array{"rtmp"}},
      },
  });

  const auto peakOf = [](const std::vector<float>& samples) {
    float peak = 0.f;
    for (const auto sample : samples) {
      peak = std::max(peak, std::abs(sample));
    }
    return peak;
  };

  // #535 slice 3b: the decoder runs on its own worker, so its PCM cannot be on
  // the tick that loaded the scene. Pump full ticks until it is, and read the
  // snapshot of THAT tick - media audio is a window, not a level.
  corevideo::rpc::Json state;
  ASSERT_TRUE(corevideo::testing::applyUntil(mediaCore, [&](corevideo::core::MediaCore& core) {
    return peakOf(core.audioBusTapPcm("stream")) > 0.05f && peakOf(core.programAudioTapPcm()) > 0.f;
  }, 3000, &state)) << "media PCM never reached the stream bus";
  EXPECT_TRUE(SolidMediaFrameSource::audioPollCount > 0);
  const auto& stream = mediaCore.audioBusTapPcm("stream");
  const auto& master = mediaCore.programAudioTapPcm();
  ASSERT_FALSE(stream.empty());
  ASSERT_FALSE(master.empty());
  EXPECT_TRUE(peakOf(stream) > peakOf(master) * 5.f);
  EXPECT_EQ(outputSender->destinations_, std::vector<std::string>{"rtmp"});
  EXPECT_EQ(outputSender->audioFramesSent_, static_cast<int>(stream.size() / 2u));
  EXPECT_EQ(outputSender->audioChannels_, 2);
  EXPECT_EQ(outputSender->audioSampleRate_, 48000);
  EXPECT_TRUE(outputSender->maxAudioSample_ > 0.05f);

  const auto* mix = state.get("audioMixSession");
  ASSERT_NE(mix, nullptr);
  const auto* participants = mix->get("participants");
  ASSERT_NE(participants, nullptr);
  // Headless (no console synced): meters carry each clip's own identity.
  const auto mediaParticipant = std::find_if(
      participants->asArray().begin(), participants->asArray().end(), [](const corevideo::rpc::Json& participant) {
        return participant.getString("participantId") == "media:clip-intro";
      });
  ASSERT_TRUE(mediaParticipant != participants->asArray().end());
  EXPECT_TRUE(mediaParticipant->get("outputLevel")->asNumber() > 0);
}

namespace {

float peakOfSamples(const std::vector<float>& samples) {
  float peak = 0.f;
  for (const auto sample : samples) {
    peak = std::max(peak, std::abs(sample));
  }
  return peak;
}

corevideo::rpc::Json playingMediaRoute(const std::string& routeId, const std::string& assetId) {
  return corevideo::rpc::Json::Object{
      {"routeId", routeId},
      {"mode", "fixed"},
      {"mediaAssetId", assetId},
      {"mediaAssetName", assetId},
      {"mediaAssetKind", "video"},
      {"mediaAssetPath", "C:\\media\\" + assetId + ".mp4"},
      {"mediaPlaybackKey", "media:" + assetId + ":live:1"},
      {"mediaAssetPlaying", true},
      {"rect", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}},
  };
}

// A channel strip exactly as MediaCoreCommandBuilder.BuildAudioMixCommand
// serializes one.
corevideo::rpc::Json shellAudioStrip(const std::string& participantId, bool muted = false) {
  return corevideo::rpc::Json::Object{
      {"participantId", participantId},
      {"inputLevel", 0},
      {"muted", muted},
      {"noiseSuppression", false},
      {"manualGainDb", 0},
      {"pan", 0},
      {"solo", false},
      {"pluginInserts", corevideo::rpc::Json::Array{}},
      {"insertSettings", corevideo::rpc::Json::Object{}},
  };
}

corevideo::rpc::Json shellAudioMix(corevideo::rpc::Json::Array channels) {
  return corevideo::rpc::Json::Object{
      {"type", "sync-participant-audio-mix"},
      {"limiterEnabled", true},
      {"channels", std::move(channels)},
  };
}

// The routing command as MediaCoreCommandBuilder.BuildAudioRoutingMatrixCommand
// serializes it, with the sends StudioViewModel.EnsureDefaultMediaAudioRoutingSends
// seeds whenever Program carries a media route.
corevideo::rpc::Json shellMediaRouting(corevideo::rpc::Json::Array extraSends = {}) {
  corevideo::rpc::Json::Array sends;
  for (const char* busId : {"master", "pgm-l", "pgm-r", "stream", "mon"}) {
    sends.emplace_back(corevideo::rpc::Json::Object{
        {"sourceId", "media"}, {"busId", busId}, {"gainDb", 0}, {"busPluginInserts", corevideo::rpc::Json::Array{}}});
  }
  for (auto& send : extraSends) {
    sends.emplace_back(std::move(send));
  }
  return corevideo::rpc::Json::Object{
      {"type", "sync-audio-routing-matrix"},
      {"sends", std::move(sends)},
      {"busSends", corevideo::rpc::Json::Array{}},
      {"monitorBusId", ""},
  };
}

const corevideo::rpc::Json* audioMixParticipant(const corevideo::rpc::Json& state, const std::string& participantId) {
  const auto* mix = state.get("audioMixSession");
  if (mix == nullptr || mix->get("participants") == nullptr) return nullptr;
  for (const auto& participant : mix->get("participants")->asArray()) {
    if (participant.getString("participantId") == participantId) return &participant;
  }
  return nullptr;
}

}  // namespace

TEST(AudioControlSourcePolicy, MediaClipsAreGovernedByTheShellMediaControls) {
  using corevideo::core::audioControlSourceIdFor;
  using corevideo::core::joinsMediaAudioPreSum;
  using std::string_view_literals::operator""sv;
  EXPECT_EQ(audioControlSourceIdFor("media:clip-intro"), "media"sv);
  EXPECT_EQ(audioControlSourceIdFor("media:media-5f953bd23617"), "media"sv);
  EXPECT_EQ(audioControlSourceIdFor("media"), "media"sv);
  EXPECT_EQ(audioControlSourceIdFor("media:"), "media:"sv);  // no asset id: not a clip
  EXPECT_EQ(audioControlSourceIdFor("mediafoo"), "mediafoo"sv);
  EXPECT_EQ(audioControlSourceIdFor("zoom-mix"), "zoom-mix"sv);
  EXPECT_EQ(audioControlSourceIdFor("capture:cam-1"), "capture:cam-1"sv);
  EXPECT_EQ(audioControlSourceIdFor("background:clip"), "background:clip"sv);

  // Pre-sum membership: a clip joins unless it has its own strip or send row.
  EXPECT_TRUE(joinsMediaAudioPreSum("media:a", false, false));
  EXPECT_FALSE(joinsMediaAudioPreSum("media:a", true, false));
  EXPECT_FALSE(joinsMediaAudioPreSum("media:a", false, true));
  EXPECT_TRUE(joinsMediaAudioPreSum("media", true, true));  // legacy id cannot collide with the pre-sum
  EXPECT_FALSE(joinsMediaAudioPreSum("zoom-mix", false, false));
  EXPECT_FALSE(joinsMediaAudioPreSum("media:", false, false));
}

// T1.6 / #455, RED before the alias: with the console the shell ALWAYS syncs
// (a "media" strip and "media" sends), media PCM keyed `media:<assetId>` had
// no strip, the FADER LAW dropped it, and master/stream/recording were silent.
TEST(MediaCoreCommand, SceneMediaAudioReachesMasterThroughTheShellMediaStrip) {
  auto modules = corevideo::modules::createStubModules();
  SolidMediaFrameSource::reset();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
  corevideo::core::MediaCore mediaCore(std::move(modules));

  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "media-shell-console"},
          {"routes", corevideo::rpc::Json::Array{playingMediaRoute("media-main", "clip-intro")}},
      },
      shellAudioMix(corevideo::rpc::Json::Array{shellAudioStrip("media"), shellAudioStrip("zoom-mix")}),
      shellMediaRouting(),
  });

  corevideo::rpc::Json state;
  ASSERT_TRUE(corevideo::testing::applyUntil(mediaCore, [](corevideo::core::MediaCore& core) {
    return peakOfSamples(core.programAudioTapPcm()) > 0.05f &&
           peakOfSamples(core.audioBusTapPcm("stream")) > 0.05f;
  }, 3000, &state)) << "media PCM never reached the master bus";
  ASSERT_TRUE(SolidMediaFrameSource::audioPollCount > 0);
  const auto& master = mediaCore.programAudioTapPcm();
  ASSERT_FALSE(master.empty()) << "media PCM never reached the master bus";
  EXPECT_TRUE(peakOfSamples(master) > 0.05f);
  EXPECT_TRUE(peakOfSamples(mediaCore.audioBusTapPcm("stream")) > 0.05f);

  // The operator's "Media playback" strip meters the clip it governs.
  const auto* mediaStrip = audioMixParticipant(state, "media");
  ASSERT_NE(mediaStrip, nullptr);
  EXPECT_TRUE(mediaStrip->get("outputLevel")->asNumber() > 0);
  EXPECT_NE(mediaStrip->getString("status"), "waiting-for-pcm");

  // The alias respects the fader: muting the "media" strip silences the clip.
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      shellAudioMix(corevideo::rpc::Json::Array{shellAudioStrip("media", true), shellAudioStrip("zoom-mix")}),
  });
  const auto mutedState = mediaCore.applyCommands(corevideo::rpc::Json::Array{});
  EXPECT_TRUE(peakOfSamples(mediaCore.programAudioTapPcm()) < 0.001f)
      << "a muted Media strip must keep media PCM off master";
  EXPECT_TRUE(peakOfSamples(mediaCore.audioBusTapPcm("stream")) < 0.001f);
  const auto* mutedStrip = audioMixParticipant(mutedState, "media");
  ASSERT_NE(mutedStrip, nullptr);
  EXPECT_EQ(mutedStrip->get("outputLevel")->asNumber(), 0);
}

// Two clips on Program under the ONE "media" strip + send: both sum into master
// (each keeps its own source slot — neither overwrites the other).
TEST(MediaCoreCommand, TwoMediaClipsBothSumThroughTheOneMediaStrip) {
  auto modules = corevideo::modules::createStubModules();
  SolidMediaFrameSource::reset();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
  corevideo::core::MediaCore mediaCore(std::move(modules));

  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "two-clips"},
          {"routes", corevideo::rpc::Json::Array{playingMediaRoute("clip-a", "clip-a"),
                                                 playingMediaRoute("clip-b", "clip-b")}},
      },
      shellAudioMix(corevideo::rpc::Json::Array{shellAudioStrip("media")}),
      shellMediaRouting(),
  });

  // The fake emits the same 0.2-peak in-phase sine per clip: one clip alone
  // peaks at ~0.2, both summed at ~0.4.
  EXPECT_TRUE(corevideo::testing::applyUntil(mediaCore, [](corevideo::core::MediaCore& core) {
    return peakOfSamples(core.programAudioTapPcm()) > 0.3f;
  }));
}

// An explicit per-clip strip or send (exact `media:<assetId>`) wins over the alias.
TEST(MediaCoreCommand, AnExplicitPerClipStripAndSendWinOverTheMediaAlias) {
  {
    auto modules = corevideo::modules::createStubModules();
    SolidMediaFrameSource::reset();
    modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
    corevideo::core::MediaCore mediaCore(std::move(modules));
    // Generic Media strip muted, the clip's own strip open: the clip is audible.
    (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
        corevideo::rpc::Json::Object{
            {"type", "load-scene-graph"},
            {"sceneId", "explicit-strip"},
            {"routes", corevideo::rpc::Json::Array{playingMediaRoute("media-main", "clip-intro")}},
        },
        shellAudioMix(corevideo::rpc::Json::Array{shellAudioStrip("media", true),
                                                  shellAudioStrip("media:clip-intro")}),
        shellMediaRouting(),
    });
    EXPECT_TRUE(corevideo::testing::applyUntil(mediaCore, [](corevideo::core::MediaCore& core) {
      return peakOfSamples(core.programAudioTapPcm()) > 0.05f;
    }));
  }
  {
    auto modules = corevideo::modules::createStubModules();
    SolidMediaFrameSource::reset();
    modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
    corevideo::core::MediaCore mediaCore(std::move(modules));
    // The clip's own send (aux-1 only) replaces the generic media sends for it.
    (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
        corevideo::rpc::Json::Object{
            {"type", "load-scene-graph"},
            {"sceneId", "explicit-send"},
            {"routes", corevideo::rpc::Json::Array{playingMediaRoute("media-main", "clip-intro")}},
        },
        shellAudioMix(corevideo::rpc::Json::Array{shellAudioStrip("media")}),
        shellMediaRouting(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
            {"sourceId", "media:clip-intro"}, {"busId", "aux-1"}, {"gainDb", 0},
            {"busPluginInserts", corevideo::rpc::Json::Array{}}}}),
    });
    EXPECT_TRUE(corevideo::testing::applyUntil(mediaCore, [](corevideo::core::MediaCore& core) {
      return peakOfSamples(core.audioBusTapPcm("aux-1")) > 0.05f;
    }));
    EXPECT_TRUE(peakOfSamples(mediaCore.programAudioTapPcm()) < 0.001f);
  }
}

// A clip with its own send row but no strip of its own is still governed by the
// "media" strip: muting it silences the clip on EVERY bus (the FADER LAW holds
// through the alias strip), including the clip's own explicit send.
TEST(MediaCoreCommand, AClipWithItsOwnSendIsStillSilencedByTheMutedMediaStrip) {
  auto modules = corevideo::modules::createStubModules();
  SolidMediaFrameSource::reset();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
  corevideo::core::MediaCore mediaCore(std::move(modules));
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "own-send-muted-strip"},
          {"routes", corevideo::rpc::Json::Array{playingMediaRoute("media-main", "clip-intro")}},
      },
      shellAudioMix(corevideo::rpc::Json::Array{shellAudioStrip("media", true), shellAudioStrip("zoom-mix")}),
      shellMediaRouting(corevideo::rpc::Json::Array{
          corevideo::rpc::Json::Object{{"sourceId", "media:clip-intro"}, {"busId", "aux-1"}, {"gainDb", 0},
                                       {"busPluginInserts", corevideo::rpc::Json::Array{}}},
          corevideo::rpc::Json::Object{{"sourceId", "media:clip-intro"}, {"busId", "master"}, {"gainDb", 0},
                                       {"busPluginInserts", corevideo::rpc::Json::Array{}}}}),
  });
  // The decoder is asynchronous now, so run real ticks before asserting an
  // ABSENCE - a tick-zero assertion would pass whether or not the fader law
  // holds. The decoder has certainly produced by the time it has been polled.
  corevideo::testing::applyTicks(mediaCore, 40);
  EXPECT_TRUE(SolidMediaFrameSource::pollCount > 0) << "the clip never decoded, so silence proves nothing";
  for (const char* busId : {"master", "pgm-l", "pgm-r", "stream", "mon", "aux-1"}) {
    EXPECT_TRUE(peakOfSamples(mediaCore.audioBusTapPcm(busId)) < 0.001f) << busId;
  }
}

// Fix round 1 (Important): the "media" strip processes the MIX of its clips,
// not each clip separately. A compressor at -12 dBFS (hard knee) leaves one
// 0.2-peak clip (-14 dBFS) alone but must bite on two summed (0.4, -8 dBFS) —
// per-clip processing would report no gain reduction in either case. And the
// strip chain runs once: one persistent DSP state, keyed "media".
TEST(MediaCoreCommand, TheMediaStripCompressesTheSumOfItsClipsInOneChain) {
  const auto compressedMediaStrip = [] {
    return corevideo::rpc::Json::Object{
        {"participantId", "media"},
        {"inputLevel", 0},
        {"muted", false},
        {"noiseSuppression", false},
        {"manualGainDb", 0},
        {"pan", 0},
        {"solo", false},
        {"pluginInserts", corevideo::rpc::Json::Array{"compressor"}},
        {"insertSettings",
         corevideo::rpc::Json::Object{
             {"compressor", corevideo::rpc::Json::Object{{"thresholdDb", -12}, {"kneeDb", 0}, {"ratio", 4}}}}},
    };
  };
  const auto runWithClips = [&](corevideo::rpc::Json::Array routes, std::vector<std::string>* dspIds) {
    auto modules = corevideo::modules::createStubModules();
    SolidMediaFrameSource::reset();
    modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
    corevideo::core::MediaCore mediaCore(std::move(modules));
    (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
        corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "compressed-media"},
                                     {"routes", std::move(routes)}},
        shellAudioMix(corevideo::rpc::Json::Array{compressedMediaStrip(), shellAudioStrip("zoom-mix")}),
        shellMediaRouting(),
    });
    // #535 slice 3b: each clip decodes on its OWN worker, so the two only land
    // in the same 20 ms audio window once both are rolling - a single tick can
    // legitimately carry one clip and read 0 dB. Take the PEAK gain reduction
    // over a bounded window: one clip can never compress (its 0.2 peak is below
    // the -12 dBFS threshold), so a max is exact for both legs of this test.
    double peakGr = -1.0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{});
      if (dspIds != nullptr) *dspIds = mediaCore.channelDspStateIdsForTest();
      if (const auto* strip = audioMixParticipant(state, "media")) {
        peakGr = std::max(peakGr, strip->get("gainReductionDb")->asNumber());
      }
      if (peakGr > 0.5) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return peakGr < 0.0 ? -1.0 : peakGr;
  };

  const double oneClipGr = runWithClips(corevideo::rpc::Json::Array{playingMediaRoute("clip-a", "clip-a")}, nullptr);
  std::vector<std::string> dspIds;
  const double twoClipGr = runWithClips(corevideo::rpc::Json::Array{playingMediaRoute("clip-a", "clip-a"),
                                                                    playingMediaRoute("clip-b", "clip-b")},
                                        &dspIds);
  EXPECT_EQ(oneClipGr, 0.0);
  EXPECT_TRUE(twoClipGr > 0.5) << "the Media strip compressor must react to the combined clips";
  EXPECT_TRUE(std::find(dspIds.begin(), dspIds.end(), "media") != dspIds.end());
  EXPECT_TRUE(std::find(dspIds.begin(), dspIds.end(), "media:clip-a") == dspIds.end());
  EXPECT_TRUE(std::find(dspIds.begin(), dspIds.end(), "media:clip-b") == dspIds.end());
}

#if !COREVIDEO_STUB && COREVIDEO_WITH_MF_ENCODER
// A 1 s, 30-frame, 64x64 H.264 MP4 (Media Foundation decodes it natively).
void writeMfVideoFixture(const std::filesystem::path& path) {
  const auto videoBytes = corevideo::modules::base64Decode(R"(
AAAAIGZ0eXBpc29tAAACAGlzb21pc28yYXZjMW1wNDEAAASibW9vdgAAAGxtdmhkAAAAAAAAAAAAAAAAAAAD6AAAA+gAAQAAAQAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAA8x0cmFrAAAAXHRraGQAAAADAAAAAAAAAAAAAAABAAAAAAAAA+gAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAAEAAAABAAAAAAAAkZWR0cwAAABxlbHN0AAAAAAAAAAEAAAPoAAAEAAABAAAAAANEbWRpYQAAACBtZGhkAAAAAAAAAAAAAAAAAAA8AAAAPABVxAAAAAAALWhkbHIAAAAAAAAAAHZpZGUAAAAAAAAAAAAAAABWaWRlb0hhbmRsZXIAAAAC721pbmYAAAAUdm1oZAAAAAEAAAAAAAAAAAAAACRkaW5mAAAAHGRyZWYAAAAAAAAAAQAAAAx1cmwgAAAAAQAAAq9zdGJsAAAAv3N0c2QAAAAAAAAAAQAAAK9hdmMxAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAEAAQABIAAAASAAAAAAAAAABFUxhdmM2Mi4yOC4xMDEgbGlieDI2NAAAAAAAAAAAAAAAGP//AAAANWF2Y0MBZAAK/+EAGGdkAAqs2UQmwEQAAAMABAAAAwDwPEiWWAEABmjr48siwP34+AAAAAAQcGFzcAAAAAEAAAABAAAAFGJ0cnQAAAAAAAAj8AAAAAAAAAAYc3R0cwAAAAAAAAABAAAAHgAAAgAAAAAUc3RzcwAAAAAAAAABAAAAAQAAAQBjdHRzAAAAAAAAAB4AAAABAAAEAAAAAAEAAAoAAAAAAQAABAAAAAABAAAAAAAAAAEAAAIAAAAAAQAACgAAAAABAAAEAAAAAAEAAAAAAAAAAQAAAgAAAAABAAAKAAAAAAEAAAQAAAAAAQAAAAAAAAABAAACAAAAAAEAAAoAAAAAAQAABAAAAAABAAAAAAAAAAEAAAIAAAAAAQAACgAAAAABAAAEAAAAAAEAAAAAAAAAAQAAAgAAAAABAAAKAAAAAAEAAAQAAAAAAQAAAAAAAAABAAACAAAAAAEAAAoAAAAAAQAABAAAAAABAAAAAAAAAAEAAAIAAAAAAQAABAAAAAAcc3RzYwAAAAAAAAABAAAAAQAAAB4AAAABAAAAjHN0c3oAAAAAAAAAAAAAAB4AAALcAAAADgAAAAwAAAAMAAAADAAAABQAAAAOAAAADAAAAAwAAAAUAAAADgAAAAwAAAAMAAAAFAAAAA4AAAAMAAAADAAAABQAAAAOAAAADAAAAAwAAAAUAAAADgAAAAwAAAAMAAAAFAAAAA4AAAAMAAAADAAAABQAAAAUc3RjbwAAAAAAAAABAAAE0gAAAGJ1ZHRhAAAAWm1ldGEAAAAAAAAAIWhkbHIAAAAAAAAAAG1kaXJhcHBsAAAAAAAAAAAAAAAALWlsc3QAAAAlqXRvbwAAAB1kYXRhAAAAAQAAAABMYXZmNjIuMTIuMTAxAAAACGZyZWUAAASGbWRhdAAAAq4GBf//qtxF6b3m2Ui3lizYINkj7u94MjY0IC0gY29yZSAxNjUgcjMyMjMgMDQ4MGNiMCAtIEguMjY0L01QRUctNCBBVkMgY29kZWMgLSBDb3B5bGVmdCAyMDAzLTIwMjUgLSBodHRwOi8vd3d3LnZpZGVvbGFuLm9yZy94MjY0Lmh0bWwgLSBvcHRpb25zOiBjYWJhYz0xIHJlZj0zIGRlYmxvY2s9MTowOjAgYW5hbHlzZT0weDM6MHgxMTMgbWU9aGV4IHN1Ym1lPTcgcHN5PTEgcHN5X3JkPTEuMDA6MC4wMiBtaXhlZF9yZWY9MSBtZV9yYW5nZT0xNiBjaHJvbWFfbWU9MSB0cmVsbGlzPTEgOHg4ZGN0PTEgY3FtPTAgZGVhZHpvbmU9MjEsMTEgZmFzdF9wc2tpcD0xIGNocm9tYV9xcF9vZmZzZXQ9LTIgdGhyZWFkcz0yIGxvb2thaGVhZF90aHJlYWRzPTEgc2xpY2VkX3RocmVhZHM9MCBucj0wIGRlY2ltYXRlPTEgaW50ZXJsYWNlZD0wIGJsdXJheV9jb21wYXQ9MCBjb25zdHJhaW5lZF9pbnRyYT0wIGJmcmFtZXM9MyBiX3B5cmFtaWQ9MiBiX2FkYXB0PTEgYl9iaWFzPTAgZGlyZWN0PTEgd2VpZ2h0Yj0xIG9wZW5fZ29wPTAgd2VpZ2h0cD0yIGtleWludD0yNTAga2V5aW50X21pbj0yNSBzY2VuZWN1dD00MCBpbnRyYV9yZWZyZXNoPTAgcmNfbG9va2FoZWFkPTQwIHJjPWNyZiBtYnRyZWU9MSBjcmY9MjMuMCBxY29tcD0wLjYwIHFwbWluPTAgcXBtYXg9NjkgcXBzdGVwPTQgaXBfcmF0aW89MS40MCBhcT0xOjEuMDAAgAAAACZliIQAN//+4QP4FM97+Yxq3VFlphXLkbcSjp8gDW8Tm/+RMQM11wAAAApBmiRsQ3/+p4+IAAAACEGeQniFfww5AAAACAGeYXRCfw5IAAAACAGeY2pCfw5JAAAAEEGaaEmoQWiZTAhv//6nj4kAAAAKQZ6GRREsK/8MOQAAAAgBnqV0Qn8OSQAAAAgBnqdqQn8OSAAAABBBmqxJqEFsmUwIb//+p4+IAAAACkGeykUVLCv/DDkAAAAIAZ7pdEJ/DkgAAAAIAZ7rakJ/DkgAAAAQQZrwSahBbJlMCG///qePiQAAAApBnw5FFSwr/ww5AAAACAGfLXRCfw5JAAAACAGfL2pCfw5IAAAAEEGbNEmoQWyZTAhv//6nj4gAAAAKQZ9SRRUsK/8MOQAAAAgBn3F0Qn8OSAAAAAgBn3NqQn8OSAAAABBBm3hJqEFsmUwIZ//+ni3xAAAACkGflkUVLCv/DDgAAAAIAZ+1dEJ/DkkAAAAIAZ+3akJ/DkkAAAAQQZu8SahBbJlMCFf//jiNwAAAAApBn9pFFSwr/ww5AAAACAGf+XRCfw5IAAAACAGf+2pCfw5JAAAAEEGb/UmoQWyZTAhP//3xrYE=
)");
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(videoBytes.data()), static_cast<std::streamsize>(videoBytes.size()));
}

TEST(MediaFoundationMediaFrameSource, DecodesFirstFrameForPausedPreviewCue) {
  const auto videoPath = std::filesystem::temp_directory_path() / "corevideo-mf-preview-cue.mp4";
  std::filesystem::remove(videoPath);
  writeMfVideoFixture(videoPath);

  auto source = corevideo::modules::createMediaFoundationMediaDecoderFactory()();
  ASSERT_NE(source, nullptr);
  corevideo::modules::CompositorRenderPlanLayer layer;
  layer.kind = "media-video";
  layer.sourceId = "preview:media:diagnostic";
  layer.mediaAssetId = "diagnostic";
  layer.mediaAssetKind = "stinger";
  layer.mediaAssetPath = videoPath.string();
  layer.mediaAssetPlaying = false;
  std::vector<corevideo::modules::VideoFrame> frames;
  const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (frames.empty() && std::chrono::steady_clock::now() < readyDeadline) {
    frames = source->pollMediaFrames({layer}, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (frames.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_FALSE(frames.empty());
  EXPECT_EQ(frames.front().participantId, "preview:media:diagnostic");
  EXPECT_TRUE(frames.front().hasPixels());
  EXPECT_EQ(frames.front().width, 64);
  EXPECT_EQ(frames.front().height, 64);
  ASSERT_EQ(frames.front().pixels->size(), 64u * 64u * 4u);
  for (size_t offset = 3; offset < frames.front().pixels->size(); offset += 4) {
    EXPECT_EQ((*frames.front().pixels)[offset], 0xff);
  }
  EXPECT_TRUE(source->warnings().empty());
  source.reset(); // Release owned decoder workers before deleting their fixture.
  std::filesystem::remove(videoPath);
}

TEST(MediaFoundationMediaFrameSource, DecodesSceneMediaAudioPcmFromLocalWav) {
  const auto wavPath = std::filesystem::temp_directory_path() / "corevideo-mf-media-audio-test.wav";
  std::filesystem::remove(wavPath);
  writeSineWaveFile(wavPath, 44100, 1);

  auto source = corevideo::modules::createMediaFoundationMediaDecoderFactory()();
  ASSERT_NE(source, nullptr);

  corevideo::modules::CompositorRenderPlanLayer layer;
  layer.kind = "media-video";
  layer.sourceId = "media:clip-audio";
  layer.mediaAssetId = "clip-audio";
  layer.mediaAssetKind = "video";
  layer.mediaAssetPath = wavPath.string();
  layer.mediaAssetPlaying = true;

  std::vector<corevideo::modules::AudioFrame> frames;
  const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (frames.empty() && std::chrono::steady_clock::now() < readyDeadline) {
    frames = source->pollMediaAudioFrames({layer}, 33);
    if (frames.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_FALSE(frames.empty());
  EXPECT_EQ(frames.front().participantId, "media:clip-audio");
  EXPECT_EQ(frames.front().sampleRate, 48000);
  EXPECT_EQ(frames.front().channels, 2);
  EXPECT_TRUE(frames.front().sampleCount > 0);
  ASSERT_FALSE(frames.front().pcm.empty());

  float peak = 0.f;
  for (const auto sample : frames.front().pcm) {
    peak = std::max(peak, std::abs(sample));
  }
  EXPECT_TRUE(peak > 0.05f);
  EXPECT_TRUE(source->warnings().empty());

  source.reset(); // Release owned decoder workers before deleting their fixture.
  std::filesystem::remove(wavPath);
}

namespace {
int64_t steadyNow100ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
}
// 48 kHz stereo float WAV whose every sample encodes its own media time:
// value = (millisecond + 1) / 4096. Coarse on purpose, so any resampler
// rounding still decodes to the right millisecond.
void writeTimecodedFloatWav(const std::filesystem::path& path, int seconds) {
  const uint32_t frames = static_cast<uint32_t>(seconds) * 48000u;
  const uint32_t dataBytes = frames * 2u * 4u;
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write("RIFF", 4);
  writeLe32(stream, 4u + (8u + 18u) + (8u + 4u) + (8u + dataBytes));
  stream.write("WAVE", 4);
  stream.write("fmt ", 4);
  writeLe32(stream, 18);
  writeLe16(stream, 3);  // WAVE_FORMAT_IEEE_FLOAT
  writeLe16(stream, 2);
  writeLe32(stream, 48000);
  writeLe32(stream, 48000u * 8u);
  writeLe16(stream, 8);
  writeLe16(stream, 32);
  writeLe16(stream, 0);  // cbSize
  stream.write("fact", 4);
  writeLe32(stream, 4);
  writeLe32(stream, frames);
  stream.write("data", 4);
  writeLe32(stream, dataBytes);
  for (uint32_t n = 0; n < frames; ++n) {
    const float value = static_cast<float>(n / 48u + 1u) / 4096.f;
    stream.write(reinterpret_cast<const char*>(&value), 4);
    stream.write(reinterpret_cast<const char*>(&value), 4);
  }
}
int timecodeMs(float sample) { return static_cast<int>(std::lround(sample * 4096.f)) - 1; }
}  // namespace

// T1.2, end to end through the real Media Foundation decoder: a clip paused
// mid-roll holds the frame that was on air (never its first frame), and Play
// continues with the NEXT frame of the same reader — not the top of the clip
// (frameId 1, a reopened decoder) and not the paused duration later.
TEST(MediaFoundationMediaFrameSource, PausingMidPlaybackHoldsTheOnAirFrameAndResumeContinues) {
  const auto videoPath = std::filesystem::temp_directory_path() /
      ("corevideo-mf-pause-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".mp4");
  writeMfVideoFixture(videoPath);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); } } cleanup{videoPath};
  auto source = corevideo::modules::createMediaFoundationMediaDecoderFactory()();
  ASSERT_NE(source, nullptr);
  corevideo::modules::CompositorRenderPlanLayer layer;
  layer.kind = "media-video";
  layer.sourceId = "media:pause-clip";
  layer.mediaAssetId = "pause-clip";
  layer.mediaAssetKind = "video";
  layer.mediaAssetPath = videoPath.string();
  layer.mediaAssetPlaying = true;

  int64_t held = -1;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (held < 5 && std::chrono::steady_clock::now() < deadline) {
    const auto frames = source->pollMediaFramesAt100ns({layer}, steadyNow100ns());
    if (!frames.empty()) held = frames.front().frameId;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(held >= 5);
  ASSERT_TRUE(held < 25); // Still mid-clip (30 frames), so resume has frames to continue with.

  layer.mediaAssetPlaying = false;
  const auto pauseEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  while (std::chrono::steady_clock::now() < pauseEnd) {
    const auto frames = source->pollMediaFramesAt100ns({layer}, steadyNow100ns());
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames.front().frameId, held);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  layer.mediaAssetPlaying = true;
  int64_t next = held;
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (next == held && std::chrono::steady_clock::now() < deadline) {
    const auto frames = source->pollMediaFramesAt100ns({layer}, steadyNow100ns());
    if (!frames.empty()) next = frames.front().frameId;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(next > held) << "held=" << held << " next=" << next;
  // 250 ms of pause is ~7 frames at 30 fps: skipping it would land far past
  // held+2; a reopened decoder would restart at 1.
  EXPECT_TRUE(next <= held + 2) << "held=" << held << " next=" << next;
  source.reset(); // Release owned decoder workers before deleting their fixture.
}

// The audio half: no PCM while paused, and Play resumes from the paused media
// position (within the 50 ms A/V budget) — not from 0, not after the pause.
TEST(MediaFoundationMediaFrameSource, PausedAudioIsSilentAndResumesFromThePausedPosition) {
  const auto wavPath = std::filesystem::temp_directory_path() /
      ("corevideo-mf-pause-audio-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
  writeTimecodedFloatWav(wavPath, 3);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); } } cleanup{wavPath};
  auto source = corevideo::modules::createMediaFoundationMediaDecoderFactory()();
  ASSERT_NE(source, nullptr);
  corevideo::modules::CompositorRenderPlanLayer layer;
  layer.kind = "media-video";
  layer.sourceId = "media:pause-audio";
  layer.mediaAssetId = "pause-audio";
  layer.mediaAssetKind = "video";
  layer.mediaAssetPath = wavPath.string();
  layer.mediaAssetPlaying = true;
  const auto nowMs = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  // MediaCore polls a Program layer on the video path every render tick as
  // well; that is what keeps the source alive while its audio is paused.
  const auto poll = [&] {
    (void)source->pollMediaFramesAt100ns({layer}, steadyNow100ns());
    return source->pollMediaAudioFrames({layer}, nowMs());
  };

  int lastHeardMs = -1;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (lastHeardMs < 300 && std::chrono::steady_clock::now() < deadline) {
    for (const auto& frame : poll())
      for (size_t i = frame.pcm.size(); i-- > 0;)
        if (frame.pcm[i] != 0.f) { lastHeardMs = timecodeMs(frame.pcm[i]); break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(lastHeardMs >= 300);

  layer.mediaAssetPlaying = false;
  const auto pauseEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while (std::chrono::steady_clock::now() < pauseEnd) {
    EXPECT_TRUE(poll().empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  layer.mediaAssetPlaying = true;
  int resumedMs = -1;
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (resumedMs < 0 && std::chrono::steady_clock::now() < deadline) {
    for (const auto& frame : poll()) {
      for (const auto sample : frame.pcm)
        if (sample != 0.f) { resumedMs = timecodeMs(sample); break; }
      if (resumedMs >= 0) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(resumedMs >= 0);
  EXPECT_TRUE(std::abs(resumedMs - lastHeardMs) <= 50) << "lastHeard=" << lastHeardMs << "ms resumed=" << resumedMs << "ms";
  source.reset(); // Release owned decoder workers before deleting their fixture.
}

// ProRes MOVs (Media Foundation has no decoder) run on the FFmpeg fallback,
// which paces itself with -re and cannot be paused in place: the adapter stops
// it on Pause and restarts it at the frozen clock position on Play. The clip's
// luma encodes media time, so the resumed picture proves where it resumed.
// Opt-in on machines with FFmpeg at C:\ffmpeg\bin (it generates the fixture).
TEST(MediaFoundationMediaFrameSource, AnFfmpegDecodedClipResumesFromThePausedPositionNotTheTop) {
  const std::filesystem::path ffmpegDir = "C:\\ffmpeg\\bin";
  std::error_code missing;
  if (!std::filesystem::exists(ffmpegDir / "ffmpeg.exe", missing)) {
    // The local gtest shim has no GTEST_SKIP; say so loudly rather than pass silently.
    std::fprintf(stderr, "[  SKIPPED ] MediaFoundationMediaFrameSource.AnFfmpegDecodedClipResumesFromThePausedPositionNotTheTop"
                         " (ffmpeg absent at C:\\ffmpeg\\bin) - this test did NOT run\n");
    return;
  }
  const auto dir = std::filesystem::temp_directory_path() /
      ("corevideo-ffmpeg-pause-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } cleanup{dir};
  const auto clip = dir / "ramp.mov";
  // Luma = 16 + 50 * t (limited range): 0.0 s -> 16, 4.0 s -> 216.
  const auto command = "\"\"" + (ffmpegDir / "ffmpeg.exe").string() +
      "\" -hide_banner -loglevel error -y -f lavfi -i \"color=c=black:s=64x64:r=30:d=4,format=yuv444p,"
      "geq=lum='min(235,16+T*50)':cb=128:cr=128\" -c:v prores_ks -profile:v 0 \"" + clip.string() + "\"\"";
  ASSERT_EQ(std::system(command.c_str()), 0);
  const char* previousDir = std::getenv("COREVIDEO_FFMPEG_BIN_DIR");
  const std::string restore = previousDir ? previousDir : "";
  _putenv_s("COREVIDEO_FFMPEG_BIN_DIR", ffmpegDir.string().c_str());
  struct RestoreEnv { std::string value; ~RestoreEnv() { _putenv_s("COREVIDEO_FFMPEG_BIN_DIR", value.c_str()); } } restoreEnv{restore};

  auto source = corevideo::modules::createMediaFoundationMediaDecoderFactory()();
  ASSERT_NE(source, nullptr);
  corevideo::modules::CompositorRenderPlanLayer layer;
  layer.kind = "media-video";
  layer.sourceId = "media:prores";
  layer.mediaAssetId = "prores";
  layer.mediaAssetKind = "video";
  layer.mediaAssetPath = clip.string();
  layer.mediaAssetPlaying = true;
  // Seconds of media time, read back from the frame's centre luma.
  const auto mediaSeconds = [](const corevideo::modules::VideoFrame& frame) {
    const auto centre = static_cast<size_t>(frame.pixelHeight / 2) * frame.pixelStride + static_cast<size_t>(frame.pixelWidth / 2) * 4;
    const double full = (*frame.pixels)[centre + 1];            // G of BGRA, full range.
    return (full * 219.0 / 255.0) / 50.0;                         // Back to limited, then to t.
  };

  corevideo::modules::VideoFrame held;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  while ((!held.hasPixels() || mediaSeconds(held) < 0.6) && std::chrono::steady_clock::now() < deadline) {
    const auto frames = source->pollMediaFramesAt100ns({layer}, steadyNow100ns());
    if (!frames.empty() && frames.front().hasPixels()) held = frames.front();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(held.hasPixels());
  const double heldSeconds = mediaSeconds(held);
  std::fprintf(stderr, "[ffmpeg-pause] decoder=%s held frame=%lld t=%.3fs\n",
               held.pixelWidth == 1920 ? "ffmpeg" : "media-foundation", static_cast<long long>(held.frameId), heldSeconds);
  ASSERT_TRUE(heldSeconds >= 0.6 && heldSeconds < 2.5);

  layer.mediaAssetPlaying = false;
  const auto pauseEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  while (std::chrono::steady_clock::now() < pauseEnd) {
    const auto frames = source->pollMediaFramesAt100ns({layer}, steadyNow100ns());
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames.front().frameId, held.frameId);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  layer.mediaAssetPlaying = true;
  corevideo::modules::VideoFrame resumed;
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (!resumed.hasPixels() && std::chrono::steady_clock::now() < deadline) {
    const auto frames = source->pollMediaFramesAt100ns({layer}, steadyNow100ns());
    if (!frames.empty() && frames.front().frameId != held.frameId) resumed = frames.front();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(resumed.hasPixels());
  const double resumedSeconds = mediaSeconds(resumed);
  std::fprintf(stderr, "[ffmpeg-pause] resumed frame=%lld t=%.3fs\n", static_cast<long long>(resumed.frameId), resumedSeconds);
  EXPECT_TRUE(resumed.frameId > held.frameId);
  // Not the top (t~0) and not the 1.5 s pause later. The FFmpeg path may land
  // up to its own start-up lag ahead of the held picture, where the shared
  // clock (and the audio) actually are.
  EXPECT_TRUE(resumedSeconds >= heldSeconds - 0.1) << "held=" << heldSeconds << " resumed=" << resumedSeconds;
  EXPECT_TRUE(resumedSeconds <= heldSeconds + 0.7) << "held=" << heldSeconds << " resumed=" << resumedSeconds;
  source.reset();
}

// If the FFmpeg restart on Play FAILS (FFmpeg briefly unavailable), the clip
// holds its paused frame, says so, and retries at the clock position; it must
// never fall back to a fresh open, which would roll the clip from the top.
TEST(MediaFoundationMediaFrameSource, AFailedFfmpegResumeRetriesAtTheClockPositionNeverFromTheTop) {
  const std::filesystem::path ffmpegDir = "C:\\ffmpeg\\bin";
  std::error_code missing;
  if (!std::filesystem::exists(ffmpegDir / "ffmpeg.exe", missing)) {
    std::fprintf(stderr, "[  SKIPPED ] MediaFoundationMediaFrameSource.AFailedFfmpegResumeRetriesAtTheClockPositionNeverFromTheTop"
                         " (ffmpeg absent at C:\\ffmpeg\\bin) - this test did NOT run\n");
    return;
  }
  const auto dir = std::filesystem::temp_directory_path() /
      ("corevideo-ffmpeg-resume-fail-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } cleanup{dir};
  const auto clip = dir / "ramp.mov";
  // 8 s, luma = 16 + 25 * t (limited range): long enough to land a late retry.
  const auto command = "\"\"" + (ffmpegDir / "ffmpeg.exe").string() +
      "\" -hide_banner -loglevel error -y -f lavfi -i \"color=c=black:s=64x64:r=30:d=8,format=yuv444p,"
      "geq=lum='min(235,16+T*25)':cb=128:cr=128\" -c:v prores_ks -profile:v 0 \"" + clip.string() + "\"\"";
  ASSERT_EQ(std::system(command.c_str()), 0);
  struct SavedEnv {
    std::string name, value;
    explicit SavedEnv(const char* n) : name(n) { const char* v = std::getenv(n); value = v ? v : ""; }
    ~SavedEnv() { _putenv_s(name.c_str(), value.c_str()); }
  } savedDir{"COREVIDEO_FFMPEG_BIN_DIR"}, savedAltDir{"FFMPEG_BIN_DIR"}, savedPath{"PATH"};
  _putenv_s("COREVIDEO_FFMPEG_BIN_DIR", ffmpegDir.string().c_str());

  auto source = corevideo::modules::createMediaFoundationMediaDecoderFactory()();
  ASSERT_NE(source, nullptr);
  corevideo::modules::CompositorRenderPlanLayer layer;
  layer.kind = "media-video";
  layer.sourceId = "media:prores-retry";
  layer.mediaAssetId = "prores-retry";
  layer.mediaAssetKind = "video";
  layer.mediaAssetPath = clip.string();
  layer.mediaAssetPlaying = true;
  const auto mediaSeconds = [](const corevideo::modules::VideoFrame& frame) {
    const auto centre = static_cast<size_t>(frame.pixelHeight / 2) * frame.pixelStride + static_cast<size_t>(frame.pixelWidth / 2) * 4;
    return ((*frame.pixels)[centre + 1] * 219.0 / 255.0) / 25.0;
  };

  corevideo::modules::VideoFrame held;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  while ((!held.hasPixels() || mediaSeconds(held) < 0.6) && std::chrono::steady_clock::now() < deadline) {
    const auto frames = source->pollMediaFramesAt100ns({layer}, steadyNow100ns());
    if (!frames.empty() && frames.front().hasPixels()) held = frames.front();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(held.hasPixels());
  ASSERT_TRUE(held.pixelWidth == 1920); // This test is about the FFmpeg path.
  const double heldSeconds = mediaSeconds(held);

  layer.mediaAssetPlaying = false;
  const auto pauseEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while (std::chrono::steady_clock::now() < pauseEnd) {
    (void)source->pollMediaFramesAt100ns({layer}, steadyNow100ns());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // FFmpeg disappears exactly as the operator presses Play.
  _putenv_s("COREVIDEO_FFMPEG_BIN_DIR", (dir / "no-ffmpeg-here").string().c_str());
  _putenv_s("FFMPEG_BIN_DIR", "");
  _putenv_s("PATH", "C:\\Windows\\System32");
  layer.mediaAssetPlaying = true;
  bool warned = false;
  const auto outageEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
  while (std::chrono::steady_clock::now() < outageEnd) {
    const auto frames = source->pollMediaFramesAt100ns({layer}, steadyNow100ns());
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames.front().frameId, held.frameId); // Holds the paused frame; nothing from the top.
    for (const auto& warning : source->warnings())
      warned = warned || warning.find("could not resume after a pause") != std::string::npos;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(warned);

  // FFmpeg is back: the next retry must land at the clock position.
  _putenv_s("COREVIDEO_FFMPEG_BIN_DIR", ffmpegDir.string().c_str());
  _putenv_s("PATH", savedPath.value.c_str());
  corevideo::modules::VideoFrame resumed;
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  while (!resumed.hasPixels() && std::chrono::steady_clock::now() < deadline) {
    const auto frames = source->pollMediaFramesAt100ns({layer}, steadyNow100ns());
    if (!frames.empty() && frames.front().frameId != held.frameId) resumed = frames.front();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(resumed.hasPixels());
  const double resumedSeconds = mediaSeconds(resumed);
  std::fprintf(stderr, "[ffmpeg-resume-retry] held t=%.3fs resumed t=%.3fs\n", heldSeconds, resumedSeconds);
  EXPECT_TRUE(resumed.frameId > held.frameId);
  // Never the top of the clip; at or after the paused picture (the clock kept
  // running during the outage, so a late retry lands later, not earlier).
  EXPECT_TRUE(resumedSeconds >= heldSeconds - 0.1) << "held=" << heldSeconds << " resumed=" << resumedSeconds;
  EXPECT_TRUE(resumedSeconds <= heldSeconds + 4.0) << "held=" << heldSeconds << " resumed=" << resumedSeconds;
  source.reset();
}
#endif

TEST(MediaCoreCommand, ReportsCaptureDevicesAndAppliesCaptureControls) {
  corevideo::core::MediaCore mediaCore;
  const auto devices = mediaCore.captureDevices();
  ASSERT_TRUE(devices.asArray().size() >= 2);
  EXPECT_EQ(devices.asArray()[0].getString("vendor"), "blackmagic");
  EXPECT_EQ(devices.asArray()[0].get("inputs")->asArray().size(), 2);
  // OS-level identity travels over the wire when the device carries one (the
  // native UVC adapter's symbolic link; the stub DeckLink mirrors it), and is
  // omitted when it doesn't.
  EXPECT_EQ(devices.asArray()[0].getString("nativeDeviceId"), "\\\\?\\stub#decklink-1");
  const auto deckLinkId = devices.asArray()[0].getString("id");
  const auto ajaDevice = std::find_if(devices.asArray().begin(), devices.asArray().end(), [](const corevideo::rpc::Json& device) {
    return device.getString("vendor") == "aja";
  });
  ASSERT_TRUE(ajaDevice != devices.asArray().end());
  const auto ajaId = ajaDevice->getString("id");

  const auto selected = mediaCore.selectCaptureInput(deckLinkId, "hdmi-1");
  ASSERT_TRUE(selected.asArray().size() >= 1);
  EXPECT_EQ(selected.asArray()[0].getString("selectedInputId"), "hdmi-1");

  const auto offset = mediaCore.setCaptureAudioSyncOffset(ajaId, 1200);
  const auto aja = std::find_if(offset.asArray().begin(), offset.asArray().end(), [&](const corevideo::rpc::Json& device) {
    return device.getString("id") == ajaId;
  });
  ASSERT_TRUE(aja != offset.asArray().end());
  EXPECT_EQ(aja->get("audioSyncOffsetMs")->asNumber(), 500);

  const auto connected = mediaCore.connectCaptureDevice(ajaId);
  const auto connectedAja = std::find_if(connected.asArray().begin(), connected.asArray().end(), [&](const corevideo::rpc::Json& device) {
    return device.getString("id") == ajaId;
  });
  ASSERT_TRUE(connectedAja != connected.asArray().end());
  EXPECT_EQ(connectedAja->getString("connectionState"), "connected");
  EXPECT_TRUE(connectedAja->get("signalPresent")->asBool());
}

TEST(MediaCoreCommand, SyncsTypedCaptureAudioSources) {
  corevideo::core::MediaCore mediaCore;

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-capture-audio-sources"},
          {"sources",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{
                   {"captureDeviceId", "decklink-1"},
                   {"audioDeviceId", "embedded-decklink-1"},
                   {"audioDeviceName", "DeckLink Mini Recorder embedded audio"},
                   {"audioSourceKind", "embedded-capture-audio"},
                   {"nativeAudioDeviceId", "decklink-native-1"},
                   {"audioDriverName", "Blackmagic DeckLink"},
                   {"embedded", true},
                   {"audioSyncOffsetMs", 42},
               },
               corevideo::rpc::Json::Object{
                   {"captureDeviceId", "local-machine-audio"},
                   {"audioDeviceId", "asio-1"},
                   {"audioDeviceName", "Focusrite USB ASIO"},
                   {"audioSourceKind", "asio-input"},
                   {"nativeAudioDeviceId", "asio:{focusrite}"},
                   {"audioDriverName", "ASIO"},
                   {"audioSyncOffsetMs", -24},
               },
           }},
      },
  });

  const auto* captureAudio = state.get("captureAudioSources");
  ASSERT_NE(captureAudio, nullptr);
  EXPECT_EQ(captureAudio->getString("status"), "warning");
  EXPECT_EQ(captureAudio->get("sourceCount")->asNumber(), 2);
  EXPECT_NE(captureAudio->getString("summary").find("audio input"), std::string::npos);
  ASSERT_TRUE(captureAudio->get("warnings")->isArray());
  EXPECT_TRUE(std::any_of(
      captureAudio->get("warnings")->asArray().begin(),
      captureAudio->get("warnings")->asArray().end(),
      [](const corevideo::rpc::Json& warning) {
        return warning.asString().find("native ASIO PCM capture requires") != std::string::npos;
      }));

  const auto& sources = captureAudio->get("sources")->asArray();
  ASSERT_TRUE(sources.size() == 2u);
  EXPECT_EQ(sources[0].getString("audioSourceKind"), "embedded-capture-audio");
  EXPECT_EQ(sources[0].getString("nativeAudioDeviceId"), "decklink-native-1");
  EXPECT_EQ(sources[0].getString("audioDriverName"), "Blackmagic DeckLink");
  EXPECT_TRUE(sources[0].get("embedded")->asBool());
  EXPECT_FALSE(sources[0].getString("warning").empty());
  EXPECT_EQ(sources[1].getString("audioSourceKind"), "asio-input");
  EXPECT_EQ(sources[1].getString("audioDriverName"), "ASIO");
  EXPECT_FALSE(sources[1].getString("warning").empty());
}

TEST(MediaCoreCommand, CaptureAudioSourceWarnsWhenStreamStartsWithoutPcmFrames) {
  auto modules = corevideo::modules::createStubModules();
  auto* audioCapture = new RecordingAudioCaptureSource();
  corevideo::modules::CaptureAudioSourceMetrics metric{
      "local-machine-audio",
      "local-machine-audio",
      "wasapi-loopback",
      true,
      0,
      3,
      48000,
      2,
      "default-render",
      "System audio loopback",
      {},
      {}};
  metric.framesRendered = 0;
  metric.queuedFrames = 128;
  metric.underrunCount = 2;
  audioCapture->reportedMetrics.push_back(metric);
  modules.audioCapture.reset(audioCapture);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-capture-audio-sources"},
          {"sources",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{
                   {"captureDeviceId", "local-machine-audio"},
                   {"audioDeviceId", "system-loopback"},
                   {"audioDeviceName", "System audio loopback"},
                   {"audioSourceKind", "wasapi-loopback"},
                   {"nativeAudioDeviceId", "default-render"},
                   {"audioDriverName", "WASAPI"},
               },
           }},
      },
  });

  const auto* captureAudio = state.get("captureAudioSources");
  ASSERT_NE(captureAudio, nullptr);
  EXPECT_EQ(captureAudio->getString("status"), "warning");
  EXPECT_EQ(captureAudio->get("streamingCount")->asNumber(), 1);
  EXPECT_EQ(captureAudio->get("captureFramesReceived")->asNumber(), 0);
  ASSERT_TRUE(captureAudio->get("sources")->asArray().size() == 1u);
  EXPECT_EQ(captureAudio->get("sources")->asArray()[0].get("captureFramesRendered")->asNumber(), 0);
  EXPECT_EQ(captureAudio->get("sources")->asArray()[0].get("captureQueuedFrames")->asNumber(), 128);
  EXPECT_EQ(captureAudio->get("sources")->asArray()[0].get("captureUnderrunCount")->asNumber(), 2);
  EXPECT_NE(
      captureAudio->get("sources")->asArray()[0].getString("warning").find("no PCM frames"),
      std::string::npos);
  ASSERT_TRUE(captureAudio->get("warnings")->isArray());
  EXPECT_TRUE(std::any_of(
      captureAudio->get("warnings")->asArray().begin(),
      captureAudio->get("warnings")->asArray().end(),
      [](const corevideo::rpc::Json& warning) {
        return warning.asString().find("no PCM frames") != std::string::npos;
      }));
}

TEST(MediaCoreCommand, CaptureAudioSourcePreservesAdapterWarningWhenStreamStartsWithoutPcmFrames) {
  auto modules = corevideo::modules::createStubModules();
  auto* audioCapture = new RecordingAudioCaptureSource();
  audioCapture->reportedMetrics.push_back(corevideo::modules::CaptureAudioSourceMetrics{
      "local-machine-audio",
      "local-machine-audio",
      "wasapi-loopback",
      true,
      0,
      4,
      48000,
      2,
      "default-render",
      "Game",
      "GetNextPacketSize hr=0x88890004",
      "WASAPI capture is open on 'Game' but the endpoint has not produced loopback packets."});
  modules.audioCapture.reset(audioCapture);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-capture-audio-sources"},
          {"sources",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{
                   {"captureDeviceId", "local-machine-audio"},
                   {"audioDeviceId", "system-loopback"},
                   {"audioDeviceName", "System audio loopback"},
                   {"audioSourceKind", "wasapi-loopback"},
                   {"nativeAudioDeviceId", "default-render"},
                   {"audioDriverName", "WASAPI"},
               },
           }},
      },
  });

  const auto* captureAudio = state.get("captureAudioSources");
  ASSERT_NE(captureAudio, nullptr);
  ASSERT_TRUE(captureAudio->get("sources")->asArray().size() == 1u);
  EXPECT_NE(
      captureAudio->get("sources")->asArray()[0].getString("warning").find("endpoint has not produced loopback packets"),
      std::string::npos);
  EXPECT_EQ(
      captureAudio->get("sources")->asArray()[0].getString("warning").find("no PCM frames"),
      std::string::npos);
}

TEST(MediaCoreCommand, CaptureAudioSourceReportsAdapterLastErrorWhenNotStreaming) {
  auto modules = corevideo::modules::createStubModules();
  auto* audioCapture = new RecordingAudioCaptureSource();
  audioCapture->reportedMetrics.push_back(corevideo::modules::CaptureAudioSourceMetrics{
      "local-machine-audio",
      "local-machine-audio",
      "wasapi-loopback",
      false,
      0,
      0,
      0,
      0,
      "default-render",
      "Game",
      "WASAPI open hr=0x88890004",
      {}});
  modules.audioCapture.reset(audioCapture);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-capture-audio-sources"},
          {"sources",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{
                   {"captureDeviceId", "local-machine-audio"},
                   {"audioDeviceId", "system-loopback"},
                   {"audioDeviceName", "System audio loopback"},
                   {"audioSourceKind", "wasapi-loopback"},
                   {"nativeAudioDeviceId", "default-render"},
                   {"audioDriverName", "WASAPI"},
               },
           }},
      },
  });

  const auto* captureAudio = state.get("captureAudioSources");
  ASSERT_NE(captureAudio, nullptr);
  ASSERT_TRUE(captureAudio->get("sources")->asArray().size() == 1u);
  EXPECT_EQ(captureAudio->getString("status"), "warning");
  EXPECT_NE(
      captureAudio->get("sources")->asArray()[0].getString("warning").find("Audio capture adapter is not streaming"),
      std::string::npos);
  EXPECT_NE(
      captureAudio->get("sources")->asArray()[0].getString("warning").find("0x88890004"),
      std::string::npos);
}

TEST(MediaCoreCommand, CaptureAudioSourceWarnsWhenPcmFramesAreSilent) {
  auto modules = corevideo::modules::createStubModules();
  auto* audioCapture = new RecordingAudioCaptureSource();
  audioCapture->reportedMetrics.push_back(corevideo::modules::CaptureAudioSourceMetrics{
      "local-machine-audio",
      "local-machine-audio",
      "wasapi-loopback",
      true,
      960,
      0,
      48000,
      2,
      "default-render",
      "System audio loopback",
      {},
      {},
      -120.0,
      -120.0,
      false});
  modules.audioCapture.reset(audioCapture);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-capture-audio-sources"},
          {"sources",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{
                   {"captureDeviceId", "local-machine-audio"},
                   {"audioDeviceId", "system-loopback"},
                   {"audioDeviceName", "System audio loopback"},
                   {"audioSourceKind", "wasapi-loopback"},
                   {"nativeAudioDeviceId", "default-render"},
                   {"audioDriverName", "WASAPI"},
               },
           }},
      },
  });

  const auto* captureAudio = state.get("captureAudioSources");
  ASSERT_NE(captureAudio, nullptr);
  EXPECT_EQ(captureAudio->getString("status"), "warning");
  EXPECT_EQ(captureAudio->get("captureFramesReceived")->asNumber(), 960);
  ASSERT_TRUE(captureAudio->get("sources")->asArray().size() == 1u);
  const auto& source = captureAudio->get("sources")->asArray()[0];
  EXPECT_FALSE(source.get("signalPresent")->asBool());
  EXPECT_EQ(source.get("peakDbfs")->asNumber(), -120.0);
  EXPECT_NE(source.getString("warning").find("silent PCM frames"), std::string::npos);
}

TEST(MediaCoreCommand, CaptureAudioSourceWarnsWhenPcmFramesAreStale) {
  auto modules = corevideo::modules::createStubModules();
  auto* audioCapture = new RecordingAudioCaptureSource();
  corevideo::modules::CaptureAudioSourceMetrics metric{
      "local-machine-audio",
      "local-machine-audio",
      "wasapi-loopback",
      true,
      960,
      0,
      48000,
      2,
      "default-render",
      "System audio loopback",
      {},
      {},
      -12.0,
      -18.0,
      true};
  metric.lastFrameAtMs = 1;
  audioCapture->reportedMetrics.push_back(metric);
  modules.audioCapture.reset(audioCapture);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-capture-audio-sources"},
          {"sources",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{
                   {"captureDeviceId", "local-machine-audio"},
                   {"audioDeviceId", "system-loopback"},
                   {"audioDeviceName", "System audio loopback"},
                   {"audioSourceKind", "wasapi-loopback"},
                   {"nativeAudioDeviceId", "default-render"},
                   {"audioDriverName", "WASAPI"},
               },
           }},
      },
  });

  const auto* captureAudio = state.get("captureAudioSources");
  ASSERT_NE(captureAudio, nullptr);
  EXPECT_EQ(captureAudio->getString("status"), "warning");
  ASSERT_TRUE(captureAudio->get("sources")->asArray().size() == 1u);
  const auto& source = captureAudio->get("sources")->asArray()[0];
  EXPECT_TRUE(source.get("captureLastFrameAgeMs")->asNumber() > 1000);
  EXPECT_NE(source.getString("warning").find("PCM is stale"), std::string::npos);
  ASSERT_TRUE(captureAudio->get("warnings")->isArray());
  EXPECT_TRUE(std::any_of(
      captureAudio->get("warnings")->asArray().begin(),
      captureAudio->get("warnings")->asArray().end(),
      [](const corevideo::rpc::Json& warning) {
        return warning.asString().find("PCM is stale") != std::string::npos;
      }));
}

TEST(MediaCoreCommand, CaptureAudioSourceSyncDoesNotRestartUnchangedAdapter) {
  auto modules = corevideo::modules::createStubModules();
  auto* audioCapture = new RecordingAudioCaptureSource();
  modules.audioCapture.reset(audioCapture);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto command = corevideo::rpc::Json::Object{
      {"type", "sync-capture-audio-sources"},
      {"sources",
       corevideo::rpc::Json::Array{
           corevideo::rpc::Json::Object{
               {"captureDeviceId", "local-machine-audio"},
               {"audioDeviceId", "system-loopback"},
               {"audioDeviceName", "System audio loopback"},
               {"audioSourceKind", "wasapi-loopback"},
               {"nativeAudioDeviceId", "default-render"},
               {"audioDriverName", "WASAPI"},
               {"audioSyncOffsetMs", 0},
           },
       }},
  };

  const auto firstState = mediaCore.applyCommands(corevideo::rpc::Json::Array{command});
  const auto secondState = mediaCore.applyCommands(corevideo::rpc::Json::Array{command});
  (void)firstState;
  (void)secondState;

  EXPECT_EQ(audioCapture->configureCount, 1);
  EXPECT_TRUE(audioCapture->lastSources.size() == 1u);
  if (audioCapture->lastSources.empty()) {
    return;
  }
  EXPECT_EQ(audioCapture->lastSources[0].captureDeviceId, "local-machine-audio");
  EXPECT_EQ(audioCapture->lastSources[0].audioSourceKind, "wasapi-loopback");

  const auto changedState = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-capture-audio-sources"},
          {"sources",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{
                   {"captureDeviceId", "local-machine-audio"},
                   {"audioDeviceId", "system-loopback"},
                   {"audioDeviceName", "System audio loopback"},
                   {"audioSourceKind", "wasapi-loopback"},
                   {"nativeAudioDeviceId", "default-render"},
                   {"audioDriverName", "WASAPI"},
                   {"audioSyncOffsetMs", 25},
               },
           }},
      },
  });
  (void)changedState;

  EXPECT_EQ(audioCapture->configureCount, 2);
  EXPECT_TRUE(audioCapture->lastSources.size() == 1u);
  if (audioCapture->lastSources.empty()) {
    return;
  }
  EXPECT_EQ(audioCapture->lastSources[0].audioSyncOffsetMs, 25);
}

TEST(MediaCoreCommand, CaptureAudioSourcesProducePcmIntoNativeMixer) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-monitor"},
          {"enabled", true},
          {"deviceId", "stub-render"},
          {"deviceName", "Stub Render"},
          {"volume", 0.75},
      },
      corevideo::rpc::Json::Object{
          {"type", "sync-capture-audio-sources"},
          {"sources",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{
                   {"captureDeviceId", "decklink-1"},
                   {"audioDeviceId", "embedded-decklink-1"},
                   {"audioDeviceName", "DeckLink Mini Recorder embedded audio"},
                   {"audioSourceKind", "embedded-capture-audio"},
                   {"nativeAudioDeviceId", "decklink-native-1"},
                   {"audioDriverName", "Blackmagic DeckLink"},
                   {"embedded", true},
               },
               corevideo::rpc::Json::Object{
                   {"captureDeviceId", "local-machine-audio"},
                   {"audioDeviceId", "system-loopback"},
                   {"audioDeviceName", "System audio loopback"},
                   {"audioSourceKind", "wasapi-loopback"},
                   {"nativeAudioDeviceId", "default-render"},
                   {"audioDriverName", "WASAPI"},
               },
           }},
      },
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-routing-matrix"},
          {"sends",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{{"sourceId", "capture:decklink-1"}, {"busId", "master"}, {"gainDb", 0}},
               corevideo::rpc::Json::Object{{"sourceId", "capture:decklink-1"}, {"busId", "stream"}, {"gainDb", 0}},
               corevideo::rpc::Json::Object{{"sourceId", "capture:decklink-1"}, {"busId", "mon"}, {"gainDb", 0}},
               corevideo::rpc::Json::Object{{"sourceId", "local-machine-audio"}, {"busId", "master"}, {"gainDb", 0}},
               corevideo::rpc::Json::Object{{"sourceId", "local-machine-audio"}, {"busId", "stream"}, {"gainDb", 0}},
               corevideo::rpc::Json::Object{{"sourceId", "local-machine-audio"}, {"busId", "mon"}, {"gainDb", 0}},
           }},
      },
  });

  const auto* mix = state.get("audioMixSession");
  ASSERT_NE(mix, nullptr);
  EXPECT_TRUE(mix->get("mixedFrameCount")->asNumber() >= 2);
  EXPECT_EQ(mix->getString("monitorStatus"), "stub-monitor");
  EXPECT_TRUE(mix->get("monitorFramesPlayed")->asNumber() > 0);

  const auto* capture = findParticipantMix(*mix, "capture:decklink-1");
  ASSERT_NE(capture, nullptr);
  EXPECT_TRUE(capture->get("outputLevel")->asNumber() > 0);
  EXPECT_TRUE(capture->get("rmsDbfs")->asNumber() > -60);

  const auto* local = findParticipantMix(*mix, "local-machine-audio");
  ASSERT_NE(local, nullptr);
  EXPECT_TRUE(local->get("outputLevel")->asNumber() > 0);
  EXPECT_TRUE(local->get("rmsDbfs")->asNumber() > -60);

  const auto* captureSources = state.get("captureAudioSources");
  ASSERT_NE(captureSources, nullptr);
  const auto* matrix = state.get("audioRoutingMatrix");
  ASSERT_NE(matrix, nullptr);
  const auto* busTaps = matrix->get("busTaps");
  ASSERT_NE(busTaps, nullptr);
  bool sawMasterTap = false;
  bool sawStreamTap = false;
  bool sawMonitorTap = false;
  for (const auto& tap : busTaps->asArray()) {
    if (tap.getString("busId") == "master") {
      sawMasterTap = true;
      EXPECT_TRUE(tap.get("frames")->asNumber() > 0);
    }
    if (tap.getString("busId") == "stream") {
      sawStreamTap = true;
      EXPECT_TRUE(tap.get("frames")->asNumber() > 0);
    }
    if (tap.getString("busId") == "mon") {
      sawMonitorTap = true;
      EXPECT_TRUE(tap.get("frames")->asNumber() > 0);
    }
  }
  EXPECT_TRUE(sawMasterTap);
  EXPECT_TRUE(sawStreamTap);
  EXPECT_TRUE(sawMonitorTap);
  EXPECT_EQ(captureSources->get("sourceCount")->asNumber(), 2);
  EXPECT_EQ(captureSources->get("pairedCount")->asNumber(), 2);
  EXPECT_EQ(captureSources->get("streamingCount")->asNumber(), 2);
  EXPECT_TRUE(captureSources->get("captureFramesReceived")->asNumber() > 0);
  EXPECT_TRUE(captureSources->get("routedMasterFrames")->asNumber() > 0);
  EXPECT_TRUE(captureSources->get("routedStreamFrames")->asNumber() > 0);
  EXPECT_TRUE(captureSources->get("routedMonitorFrames")->asNumber() > 0);
  EXPECT_EQ(captureSources->get("fallbackMonitorFrames")->asNumber(), 0);
  EXPECT_TRUE(captureSources->get("monitorFramesPlayed")->asNumber() > 0);
  const auto* sources = captureSources->get("sources");
  ASSERT_NE(sources, nullptr);
  ASSERT_TRUE(sources->isArray());
  ASSERT_FALSE(sources->asArray().empty());
  const auto& firstSource = sources->asArray().front();
  ASSERT_NE(firstSource.get("emptyPacketPolls"), nullptr);
  ASSERT_NE(firstSource.get("captureStartedAtMs"), nullptr);
  ASSERT_NE(firstSource.get("captureLastFrameAtMs"), nullptr);
  ASSERT_NE(firstSource.get("captureLastFrameAgeMs"), nullptr);
  ASSERT_NE(firstSource.get("captureStoppedAtMs"), nullptr);
  ASSERT_NE(firstSource.get("endpointId"), nullptr);
  ASSERT_NE(firstSource.get("endpointName"), nullptr);
  ASSERT_NE(firstSource.get("lastError"), nullptr);
  ASSERT_NE(firstSource.get("peakDbfs"), nullptr);
  ASSERT_NE(firstSource.get("rmsDbfs"), nullptr);
  ASSERT_NE(firstSource.get("signalPresent"), nullptr);
  EXPECT_TRUE(firstSource.get("signalPresent")->asBool());
  EXPECT_TRUE(firstSource.get("captureStartedAtMs")->asNumber() >= 0);
  EXPECT_TRUE(firstSource.get("captureLastFrameAtMs")->asNumber() >= 0);
  EXPECT_TRUE(firstSource.get("captureLastFrameAgeMs")->asNumber() >= 0);
  EXPECT_TRUE(firstSource.get("captureStoppedAtMs")->asNumber() >= 0);
}

TEST(MediaCoreCommand, ConfiguresSrtIngestSourcesAsCaptureInputs) {
  corevideo::core::MediaCore mediaCore;

  const auto snapshot = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "configure-srt-ingest-sources"},
          {"sources",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{
                   {"id", "srt-source-01"},
                   {"deviceId", "srt-ingest-01"},
                   {"name", "SRT 1"},
                   {"mode", "listener"},
                   {"host", "0.0.0.0"},
                   {"port", 10000},
                   {"latencyMs", 120},
                   {"streamId", "ingest/main"},
               },
           }},
      },
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "srt-scene"},
          {"routes",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{
                   {"routeId", "route-srt-1"},
                   {"mode", "capture-input"},
                   {"audioRole", "mix"},
                   {"captureDeviceId", "srt-ingest-01"},
               },
           }},
      },
  });

  const auto devices = snapshot.get("captureDevices")->asArray();
  const auto srtDevice = std::find_if(devices.begin(), devices.end(), [](const corevideo::rpc::Json& device) {
    return device.getString("id") == "srt-ingest-01";
  });
  ASSERT_NE(srtDevice, devices.end());
  EXPECT_EQ(srtDevice->getString("vendor"), "srt");
  EXPECT_EQ(srtDevice->get("resolution")->get("width")->asNumber(), 1920);
  EXPECT_EQ(srtDevice->get("frameRate")->asNumber(), 60);

  const auto connected = mediaCore.connectCaptureDevice("srt-ingest-01");
  const auto connectedSrt = std::find_if(connected.asArray().begin(), connected.asArray().end(), [](const corevideo::rpc::Json& device) {
    return device.getString("id") == "srt-ingest-01";
  });
  ASSERT_NE(connectedSrt, connected.asArray().end());
  EXPECT_EQ(connectedSrt->getString("connectionState"), "connecting");

  const auto rendered = mediaCore.applyCommands(corevideo::rpc::Json::Array{}, 33);
  EXPECT_TRUE(rendered.get("programFrame")->get("layerCount")->asNumber() > 0);
  EXPECT_NE(rendered.get("programFrame")->get("renderPlanSignature")->asNumber(), 0);
}

TEST(MediaCoreCommand, AppliesOutputProfileToRenderCadence) {
  corevideo::core::MediaCore mediaCore;
  const auto snapshot = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-output-profile"},
          {"profileId", "canvas-4k60"},
          {"resolution", "3840x2160"},
          {"width", 3840},
          {"height", 2160},
          {"fps", 60},
          {"targetBitrateMbps", 28.0},
      },
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "profile-scene"},
          {"routes",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{{"routeId", "speaker"}, {"mode", "fixed"}, {"participantId", "synthetic-speaker-1"}},
           }},
      },
  });

  EXPECT_EQ(snapshot.get("outputProfile")->getString("profileId"), "canvas-4k60");
  EXPECT_EQ(snapshot.get("outputProfile")->get("width")->asNumber(), 3840);
  EXPECT_EQ(snapshot.get("outputProfile")->get("height")->asNumber(), 2160);
  EXPECT_EQ(snapshot.get("outputProfile")->get("fps")->asNumber(), 60);
  EXPECT_EQ(snapshot.get("programFrame")->get("width")->asNumber(), 3840);
  EXPECT_EQ(snapshot.get("programFrame")->get("height")->asNumber(), 2160);
  EXPECT_EQ(snapshot.get("programFrame")->get("fps")->asNumber(), 60);
}

TEST(GpuCompositorAdapter, FactoryIsDisabledUnlessD3D11GateIsEnabled) {
#if COREVIDEO_WITH_D3D11
  auto compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_NE(compositor, nullptr);
  corevideo::modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = "test-plan";
  renderPlan.sceneId = "interview";
  renderPlan.layers.push_back({"speaker", "participant-video", "zoom:123", "123", 0});

  const auto layout = corevideo::compositor::gridCell(1, 0);
  renderPlan.layers[0].rect = {layout.x, layout.y, layout.width, layout.height};

  const auto frame = compositor->render(renderPlan, {{"123", 1280, 720, 16}});
  EXPECT_EQ(frame.renderer, "d3d11");
  EXPECT_EQ(frame.renderPlanId, "test-plan");
  EXPECT_EQ(frame.layerCount, 1);
  EXPECT_TRUE(frame.gpuComposed);
  EXPECT_NE(frame.programPixelSignature, 0u);
#else
  EXPECT_EQ(corevideo::modules::createD3D11Compositor(), nullptr);
#endif
}

TEST(HardwareEncoderAdapter, FactoryIsDisabledUnlessMediaFoundationGateIsEnabled) {
#if COREVIDEO_WITH_MF_ENCODER
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  ASSERT_NE(encoder, nullptr);
  const auto session = encoder->start({"recording"}, {"participant-1"});
  EXPECT_EQ(session.encoderName, "media-foundation");
  EXPECT_EQ(session.codec, "h264");
  EXPECT_TRUE(session.hardwareAccelerated);
  encoder->submit({1920, 1080, 1, 1, "test-plan", "d3d11"});
  EXPECT_EQ(encoder->session().encodedFrameCount, 1);
#else
  EXPECT_EQ(corevideo::modules::createMediaFoundationEncoderSink(), nullptr);
#endif
}

TEST(HardwareEncoderAdapter, MediaFoundationWritesMp4ArtifactWhenRecordingIsArmed) {
#if COREVIDEO_WITH_MF_ENCODER
  // Pin an ample machine: this test is about the MP4 artifact, and the live
  // capacity probe is asynchronous and GPU-dependent.
  const corevideo::testing::ForcedEncoderCapacity ampleCapacity;
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  ASSERT_NE(encoder, nullptr);
  const auto started = encoder->start({"recording"}, {"participant-1"});
  ASSERT_FALSE(started.recordingArtifactPath.empty());
  EXPECT_EQ(std::filesystem::path(started.recordingArtifactPath).extension().string(), ".mp4");

  // recordingDurationMs is the WALL-CLOCK video timeline (RecordingPtsClock,
  // spec 4.3) â€” frames muxed 60ms apart advance it ~120ms, however many frames
  // that is. (The old frame-count x 1/fps model drifted recordings A/V apart.)
  encoder->submit(makeTestProgramFrame(42));
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  encoder->submit(makeTestProgramFrame(43));
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  encoder->submit(makeTestProgramFrame(44));
  const auto session = encoder->session();
  EXPECT_TRUE(session.recordingBytesWritten > 0);
  EXPECT_EQ(session.recordingVideoFrameCount, 3);
  EXPECT_GE(session.recordingDurationMs, 99);
  EXPECT_EQ(session.recordingContainerFormat, "mp4");
  EXPECT_EQ(session.recordingVideoCodec, "h264");
  EXPECT_EQ(session.recordingAudioCodec, "aac");
  EXPECT_EQ(session.recordingWidth, 1920);
  EXPECT_EQ(session.recordingHeight, 1080);
  EXPECT_EQ(session.recordingFps, 30);
  EXPECT_TRUE(session.recordingMetadataValid);
  EXPECT_TRUE(session.recordingWarning.empty()) << session.recordingWarning;
  const auto artifactPath = session.recordingArtifactPath;
  encoder.reset();

  ASSERT_TRUE(std::filesystem::exists(artifactPath));
  EXPECT_TRUE(std::filesystem::file_size(artifactPath) > 1024u);

  std::ifstream input(artifactPath, std::ios::binary);
  std::string header(32, '\0');
  input.read(header.data(), static_cast<std::streamsize>(header.size()));
  header.resize(static_cast<size_t>(input.gcount()));
  EXPECT_NE(header.find("ftyp"), std::string::npos);
  input.close();
  std::filesystem::remove(artifactPath);
#else
  EXPECT_TRUE(true);
#endif
}

TEST(HardwareEncoderAdapter, MediaFoundationWarnsWhenRequestedContainerIsNotMp4) {
#if COREVIDEO_WITH_MF_ENCODER
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  ASSERT_NE(encoder, nullptr);

  corevideo::modules::RecordingSessionRequest request;
  request.filenamePrefix = "container-request";
  request.format = "mkv";
  request.targetBitrateMbps = 12;
  encoder->configureRecording(request);

  const auto started = encoder->start({"recording"}, {});
  ASSERT_FALSE(started.recordingArtifactPath.empty());
  EXPECT_EQ(std::filesystem::path(started.recordingArtifactPath).extension().string(), ".mp4");
  EXPECT_EQ(started.recordingFormat, "mkv");
  EXPECT_EQ(started.recordingContainerFormat, "mp4");
  EXPECT_NE(started.recordingWarning.find("requested mkv will be recorded as MP4"), std::string::npos);

  const auto artifactPath = started.recordingArtifactPath;
  encoder.reset();
  if (std::filesystem::exists(artifactPath)) {
    std::filesystem::remove(artifactPath);
  }
#else
  EXPECT_TRUE(true);
#endif
}

TEST(OutputSenderAdapter, FactoryIsDisabledUnlessRtmpGateIsEnabled) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);
  const auto session = sender->sync({"rtmp"}, nullptr, 0);
  EXPECT_GE(session.activeSenderCount, 1);
  ASSERT_FALSE(session.senders.empty());
  EXPECT_EQ(session.senders[0].destination, "rtmp");
#else
  EXPECT_EQ(corevideo::modules::createRtmpOutputSender(), nullptr);
#endif
}

TEST(OutputSenderAdapter, RtmpRequiresCurrentDestinationSettings) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);

  corevideo::modules::ProgramFrame frame{1920, 1080, 2, 7, "rtmp-settings-plan", "d3d11"};
  const auto session = sender->sync({"rtmp"}, &frame, 33);
  ASSERT_FALSE(session.senders.empty());
  EXPECT_EQ(session.senders[0].status, "warning");
  EXPECT_EQ(session.senders[0].lastResultCode, "rtmp-settings-missing");
#else
  EXPECT_TRUE(true);
#endif
}

TEST(OutputSenderAdapter, RtmpRejectsInvalidDestinationSettingsBeforeLaunchingFfmpeg) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);

  corevideo::modules::ProgramFrame frame{1920, 1080, 2, 7, "rtmp-invalid-plan", "d3d11"};
  corevideo::modules::OutputDestinationSettings missingKey;
  missingKey.id = "rtmp";
  missingKey.label = "RTMP";
  missingKey.protocol = "rtmps";
  missingKey.url = "rtmps://live.example.com/app";

  auto session = sender->sync({"rtmp"}, &frame, 33, {missingKey});
  ASSERT_FALSE(session.senders.empty());
  EXPECT_EQ(session.senders[0].status, "warning");
  EXPECT_EQ(session.senders[0].lastResultCode, "rtmp-settings-invalid");
  EXPECT_NE(session.senders[0].warning.find("stream key"), std::string::npos);

  corevideo::modules::OutputDestinationSettings mismatchedScheme = missingKey;
  mismatchedScheme.protocol = "rtmp";
  mismatchedScheme.streamKey = "stream-key";

  session = sender->sync({"rtmp"}, &frame, 66, {mismatchedScheme});
  ASSERT_FALSE(session.senders.empty());
  EXPECT_EQ(session.senders[0].status, "warning");
  EXPECT_EQ(session.senders[0].lastResultCode, "rtmp-settings-invalid");
  EXPECT_NE(session.senders[0].warning.find("protocol"), std::string::npos);
#else
  EXPECT_TRUE(true);
#endif
}

// ---------------------------------------------------------------------------
// STREAM-START ADMISSION AT THE SENDER (2026-09-20).
//
// StreamStartAdmissionTest pins the POLICY; these pin the SENDER's call site,
// which is where the branch actually lives. Deleting
// `admission.codecKnownNotDeliverable = (... == "av1")` from
// RtmpOutputSenderAdapter::startFfmpegProcess left every other C++ and shell
// test green (CLAUDE.md #481: test the whole decision, not the leaf), and the
// only thing that caught it was the real-GPU gate — which no CI job runs.
//
// None of these launches FFmpeg: every case is refused BEFORE the process is
// created, and the frames carry full-resolution program BGRA with NO encoder
// shared texture, which pins chooseStreamEncodePath to the CPU fallback on
// every build so the verdict does not depend on this machine's hardware MFT.
#if COREVIDEO_WITH_RTMP_OUTPUT
namespace {

corevideo::modules::ProgramFrame startableProgramFrame(const char* planId) {
  corevideo::modules::ProgramFrame frame{320, 180, 2, 7, planId, "d3d11"};
  frame.programFullBgra.width = 320;
  frame.programFullBgra.height = 180;
  frame.programFullBgra.bgra.assign(320u * 180u * 4u, 0x10);
  return frame;
}

// The runtime probe gates the sender before it ever reaches the admission, so a
// machine with no FFmpeg answers "runtime-missing" and would pass these tests
// vacuously. Skip LOUDLY instead (the local gtest shim has no GTEST_SKIP).
bool senderAdmissionFfmpegPresent(const char* testName) {
  std::error_code missing;
  if (std::filesystem::exists(std::filesystem::path("C:\\ffmpeg\\bin") / "ffmpeg.exe", missing)) {
    return true;
  }
  std::fprintf(stderr, "[  SKIPPED ] OutputSenderAdapter.%s (ffmpeg absent at C:\\ffmpeg\\bin)"
                       " - this test did NOT run\n", testName);
  return false;
}

corevideo::modules::OutputDestinationSettings rtmpAdmissionSettings(const std::string& codec,
                                                                    bool allowEnhancedRtmp) {
  corevideo::modules::OutputDestinationSettings settings;
  settings.id = "rtmp";
  settings.label = "RTMP";
  settings.protocol = "rtmps";
  settings.url = "rtmps://live.example.com/app";
  settings.streamKey = "stream-key";
  settings.ffmpegBinDirectory = "C:\\ffmpeg\\bin";
  settings.videoCodec = codec;
  settings.allowEnhancedRtmp = allowEnhancedRtmp;
  return settings;
}

corevideo::modules::OutputDestinationSettings srtAdmissionSettings(const std::string& codec) {
  corevideo::modules::OutputDestinationSettings settings;
  settings.id = "srt";
  settings.label = "SRT";
  settings.protocol = "srt";
  settings.host = "127.0.0.1";
  settings.port = 9101;
  settings.mode = "caller";
  settings.ffmpegBinDirectory = "C:\\ffmpeg\\bin";
  settings.videoCodec = codec;
  // The operator did NOT tick "Enhanced RTMP (H.265 / AV1)" - that checkbox is
  // an RTMP concept and must not reach an SRT destination at all.
  settings.allowEnhancedRtmp = false;
  return settings;
}

}  // namespace
#endif

// THE DEFECT: one adapter serves RTMP/RTMPS and SRT, and the enhanced-RTMP
// refusal was applied with no protocol guard - so an SRT operator who picked
// H.265 got NO stream plus a sentence telling them to enable an RTMP setting.
// SRT carries MPEG-TS, which takes H.265 natively.
TEST(OutputSenderAdapter, SrtNeverRefusesH265ForTheEnhancedRtmpCheckbox) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  if (!senderAdmissionFfmpegPresent("SrtNeverRefusesH265ForTheEnhancedRtmpCheckbox")) return;
  auto sender = corevideo::modules::createFfmpegSrtOutputSender();
  ASSERT_NE(sender, nullptr);

  auto frame = startableProgramFrame("srt-h265-admission");
  const auto session = sender->sync({"srt"}, &frame, 33, {srtAdmissionSettings("h265")});
  ASSERT_FALSE(session.senders.empty());
  const auto& s = session.senders[0];
  EXPECT_NE(s.lastResultCode, "enhanced-rtmp-required");
  EXPECT_EQ(s.warning.find("Enhanced RTMP"), std::string::npos);
  EXPECT_EQ(s.lastError.find("Enhanced RTMP"), std::string::npos);
  // The compatibility ADVISORY must not claim an RTMP constraint either.
  EXPECT_EQ(s.runtimeDetail.find("enhanced-RTMP"), std::string::npos);
  EXPECT_EQ(s.runtimeDetail.find("Enhanced RTMP"), std::string::npos);
  // With no encoder shared texture the path is the CPU fallback on every build,
  // so H.265 lands on the hardware-encoder clause - which is protocol-neutral
  // and deliberately untouched by this fix.
  EXPECT_EQ(s.lastResultCode, "no-hardware-encoder");
#else
  EXPECT_TRUE(true);
#endif
}

TEST(OutputSenderAdapter, RtmpStillRefusesH265WithoutEnhancedRtmp) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  if (!senderAdmissionFfmpegPresent("RtmpStillRefusesH265WithoutEnhancedRtmp")) return;
  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);

  auto frame = startableProgramFrame("rtmp-h265-admission");
  const auto session = sender->sync({"rtmp"}, &frame, 33, {rtmpAdmissionSettings("h265", false)});
  ASSERT_FALSE(session.senders.empty());
  EXPECT_EQ(session.senders[0].lastResultCode, "enhanced-rtmp-required");
  EXPECT_NE(session.senders[0].warning.find("Enhanced RTMP"), std::string::npos);
#else
  EXPECT_TRUE(true);
#endif
}

// AV1's refusal is OUR ENCODER's defect (#565: near-empty access units), not a
// transport constraint, so it is protocol-independent: SRT refuses it too.
TEST(OutputSenderAdapter, SrtStillRefusesAv1AsNotDeliverable) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  if (!senderAdmissionFfmpegPresent("SrtStillRefusesAv1AsNotDeliverable")) return;
  auto sender = corevideo::modules::createFfmpegSrtOutputSender();
  ASSERT_NE(sender, nullptr);

  auto frame = startableProgramFrame("srt-av1-admission");
  const auto session = sender->sync({"srt"}, &frame, 33, {srtAdmissionSettings("av1")});
  ASSERT_FALSE(session.senders.empty());
  EXPECT_EQ(session.senders[0].lastResultCode, "codec-not-deliverable");
#else
  EXPECT_TRUE(true);
#endif
}

// The one that pins `admission.codecKnownNotDeliverable = (... == "av1")` in
// isolation: the checkbox is ON, so the compatibility clause cannot fire and
// the ONLY thing that can refuse this stream is the AV1 predicate.
TEST(OutputSenderAdapter, RtmpRefusesAv1AsNotDeliverableEvenWithEnhancedRtmpOn) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  if (!senderAdmissionFfmpegPresent("RtmpRefusesAv1AsNotDeliverableEvenWithEnhancedRtmpOn")) return;
  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);

  auto frame = startableProgramFrame("rtmp-av1-admission");
  const auto session = sender->sync({"rtmp"}, &frame, 33, {rtmpAdmissionSettings("av1", true)});
  ASSERT_FALSE(session.senders.empty());
  EXPECT_EQ(session.senders[0].lastResultCode, "codec-not-deliverable");
  EXPECT_NE(session.senders[0].warning.find("does not produce a usable stream"), std::string::npos);
#else
  EXPECT_TRUE(true);
#endif
}

// ---------------------------------------------------------------------------
// #597 Lever A: the input divisor, applied at the ENCODER-TEXTURE EXPORT.
//
// These two tests pin the decision where it is APPLIED, not in the leaf policy
// (StreamBackpressurePolicyTest already proves the policy in isolation, and a
// test that only re-proved it would stay green while the product did nothing).
// Between them, deleting EITHER the compositor's export gate OR the
// sender -> MediaCore -> compositor plumbing fails a test.
// ---------------------------------------------------------------------------

namespace {

// Records every setEncoderExportDivisor call and otherwise behaves exactly like
// the compositor MediaCore was built with, so the rest of the core tick is real.
class DivisorRecordingCompositor : public corevideo::modules::ICompositor {
 public:
  explicit DivisorRecordingCompositor(std::unique_ptr<corevideo::modules::ICompositor> inner)
      : inner_(std::move(inner)) {}

  std::vector<int> divisors;

  void setEncoderExportDivisor(int divisor) override { divisors.push_back(divisor); }

  std::string rendererName() const override { return inner_->rendererName(); }
  corevideo::modules::ProgramFrame render(const corevideo::modules::CompositorRenderPlan& plan,
                                          const std::vector<corevideo::modules::VideoFrame>& frames) override {
    return inner_->render(plan, frames);
  }
  void configureProgramBuffer(int frames) override { inner_->configureProgramBuffer(frames); }
  void prepareProgramBuffer(int width, int height) override { inner_->prepareProgramBuffer(width, height); }
  void setProgramProductionTiming(int64_t slot, int64_t anchorNs) override {
    inner_->setProgramProductionTiming(slot, anchorNs);
  }
  int programBufferFrames() const override { return inner_->programBufferFrames(); }
  bool latestDeliveredProgramFrame(corevideo::modules::ProgramFrame& out) const override {
    return inner_->latestDeliveredProgramFrame(out);
  }
  bool takeDeliveredProgramFrame(corevideo::modules::ProgramFrame& out, int timeoutMs) override {
    return inner_->takeDeliveredProgramFrame(out, timeoutMs);
  }
  corevideo::modules::ProgramBufferDiagnostics programBufferDiagnostics() const override {
    return inner_->programBufferDiagnostics();
  }
  bool takeVcamNv12(std::vector<uint8_t>& outNv12, int& width, int& height) override {
    return inner_->takeVcamNv12(outNv12, width, height);
  }
  corevideo::modules::ProgramFrameSharedTexture renderMultiview(
      const corevideo::modules::CompositorRenderPlan& plan,
      const std::vector<corevideo::modules::VideoFrame>& frames) override {
    return inner_->renderMultiview(plan, frames);
  }
  corevideo::modules::ProgramFrameSharedTexture renderPreview(
      const corevideo::modules::CompositorRenderPlan& plan,
      const std::vector<corevideo::modules::VideoFrame>& frames) override {
    return inner_->renderPreview(plan, frames);
  }
  corevideo::modules::CompositorSourceTexStats sourceTexStats() const override {
    return inner_->sourceTexStats();
  }
  bool wantsFullProgramReadbackForRecording() const override {
    return inner_->wantsFullProgramReadbackForRecording();
  }
  bool suppliesProgramNv12() const override { return inner_->suppliesProgramNv12(); }
  void setVcamFrameSink(VcamFrameSink sink) override { inner_->setVcamFrameSink(std::move(sink)); }
  bool publishesVcamFrames() const override { return inner_->publishesVcamFrames(); }

 private:
  std::unique_ptr<corevideo::modules::ICompositor> inner_;
};

// Two GPU-direct destinations, each publishing whatever divisor the test sets.
// Stands in for the real senders so the MAX-across-senders rule can be driven
// without a hardware encoder and a congested network.
class BackpressurePublishingSender : public corevideo::modules::IOutputSender {
 public:
  int rtmpDivisor = 1;
  int srtDivisor = 1;

  corevideo::modules::OutputSenderSession sync(
      const std::vector<std::string>& /*destinations*/,
      const corevideo::modules::ProgramFrame* /*frame*/,
      double /*elapsedMs*/,
      const std::vector<corevideo::modules::OutputDestinationSettings>& /*settings*/,
      const std::vector<float>* /*pcm*/,
      int /*channels*/,
      int /*sampleRate*/) override {
    return session();
  }
  corevideo::modules::OutputSenderSession fail(const std::string&, const std::string&, double) override {
    return session();
  }
  corevideo::modules::OutputSenderSession recover(const std::string&, double, const std::string&) override {
    return session();
  }
  corevideo::modules::OutputSenderSession session() const override {
    corevideo::modules::OutputSenderSession out;
    out.status = "live";
    out.activeSenderCount = 2;
    out.senders.push_back(makeSender("rtmp", rtmpDivisor));
    out.senders.push_back(makeSender("srt", srtDivisor));
    // An NDI destination on the raw path: it publishes NO backpressure at all.
    // Absent is NOT "healthy" and it is NOT divisor 1 evidence - it must simply
    // contribute nothing to the max.
    corevideo::modules::OutputSender ndi;
    ndi.senderId = ndi.destination = "ndi";
    ndi.status = "live";
    ndi.destinationHealth = "ok";
    out.senders.push_back(ndi);
    // A destination the operator already STOPPED, still carrying the divisor it
    // reached before it stopped. It must not hold the compositor throttled.
    auto stopped = makeSender("rtmp-previous", 4);
    stopped.status = "stopped";
    stopped.destinationHealth = "stopped";
    stopped.lastResultCode = "stopped";
    out.senders.push_back(stopped);
    return out;
  }

 private:
  static corevideo::modules::OutputSender makeSender(const char* id, int divisor) {
    corevideo::modules::OutputSender sender;
    sender.senderId = id;
    sender.destination = id;
    sender.status = "live";
    sender.destinationHealth = "ok";
    sender.lastResultCode = "encoder-input-accepted";
    sender.backpressure = corevideo::modules::OutputBackpressureState{divisor};
    return sender;
  }
};

}  // namespace

// #597: the divisor must gate the EXPORT. Task 1 proved gating submit() does
// nothing (ratio 0.998) - the keyed mutex paces the encoder. Deleting the export
// gate must fail this test.
TEST(RtmpOutputSenderBackpressure, TheDivisorGatesTheEncoderTextureExport) {
#if COREVIDEO_WITH_D3D11
  auto compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_NE(compositor, nullptr);
  compositor->setEncoderExportDivisor(2);

  corevideo::modules::CompositorRenderPlan plan;
  plan.renderPlanId = "backpressure-export-gate";
  plan.sceneId = "backpressure-export-gate";
  plan.width = 320;
  plan.height = 180;
  // The stream is up: this is exactly the flag that makes the compositor export
  // the dedicated encoder texture on every render.
  plan.fullProgramReadback = true;
  plan.skipCpuReadback = true;

  // An EXACT count, not a ratio: this leg has no timing in it at all.
  //
  // A SHED frame is one whose pixels were not submitted, which is visible as
  // encoderSharedTexture.frameNumber (the LAST ACTUALLY SUBMITTED number) not
  // having advanced to this frame's number. The handle itself must be published
  // on EVERY frame - see the companion assertion below and
  // AShedFrameNeverRestartsTheSendersEncodePath for why.
  int submittedEven = 0;
  int submittedOdd = 0;
  int shedEven = 0;
  int shedOdd = 0;
  int framesWithoutAHandle = 0;
  int staleNumberMismatches = 0;
  int64_t lastSubmitted = -1;
  for (int i = 0; i < 200; ++i) {
    const auto frame = compositor->render(plan, {});
    if (frame.encoderSharedTexture.sharedHandleHex.empty()) ++framesWithoutAHandle;
    const bool even = (frame.frameNumber % 2) == 0;
    const bool submitted = frame.encoderSharedTexture.frameNumber == frame.frameNumber;
    if (submitted) {
      lastSubmitted = frame.frameNumber;
      (even ? submittedEven : submittedOdd)++;
    } else {
      // A shed frame must report the last number that was really submitted,
      // never this frame's - a consumer keying freshness on it would otherwise
      // be told a frame arrived that the encoder never saw.
      if (frame.encoderSharedTexture.frameNumber != lastSubmitted) ++staleNumberMismatches;
      (even ? shedEven : shedOdd)++;
    }
  }

  // THE GATE PROPERTY, exact: at divisor 2 an odd frame number is NEVER
  // submitted. Nothing here depends on timing.
  EXPECT_EQ(submittedOdd, 0) << "an odd frame number was submitted at divisor 2";
  EXPECT_EQ(shedOdd, 100) << "every odd frame must be shed at divisor 2";
  // ...and the even ones ARE submitted, so the test cannot pass by shedding
  // everything. This is deliberately NOT an exact 100: D3DDecoupledExport has
  // its own bounded 3-slot refusal (`dropped_`/`producerBusy_`) which predates
  // Lever A and is not a shed, and conflating the two would make the assertion
  // a flake rather than a property.
  EXPECT_GT(submittedEven, 0) << "no frame was submitted at all";
  EXPECT_EQ(shedEven + submittedEven, 100);
  EXPECT_EQ(staleNumberMismatches, 0)
      << "a shed frame published a frame number the encoder was never given";
  // #597 fix round 1, finding 1: the ENCODER HANDLE IS NOT THE THROTTLE. The
  // sender reads the presence of this handle as "GPU-direct is available"
  // (resolveGpuEncodePath -> chooseStreamEncodePath), and a shed frame has not
  // changed that fact. Publishing an empty handle on a shed frame restarts
  // FFmpeg and the hardware encoder once per shed frame - #597 itself, amplified.
  EXPECT_EQ(framesWithoutAHandle, 0)
      << "a shed frame dropped the encoder handle; the sender reads that as the "
         "encoder texture disappearing and restarts FFmpeg";
#else
  EXPECT_TRUE(true) << "The encoder-texture export gate lives in the D3D11 compositor.";
#endif
}

// #597 Task 6 fix round 1 (declared coverage gap, secondary): the test above
// exercises the identical `!submitPixels` branch that increments
// encoderExportShedFrames_ byte-for-byte, but never reads the counter back -
// "the branch is covered" is not "the counter reads back the right number".
// This does, inside the same permitted filter as the test it extends.
TEST(RtmpOutputSenderBackpressure, EncoderExportShedFramesCountsExactlyWhatItSkipped) {
#if COREVIDEO_WITH_D3D11
  auto compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_NE(compositor, nullptr);
  compositor->setEncoderExportDivisor(4);

  corevideo::modules::CompositorRenderPlan plan;
  plan.renderPlanId = "backpressure-shed-counter";
  plan.sceneId = "backpressure-shed-counter";
  plan.width = 320;
  plan.height = 180;
  plan.fullProgramReadback = true;
  plan.skipCpuReadback = true;

  const auto before = compositor->encoderExportShedFrames();
  int expectedShed = 0;
  for (int i = 0; i < 40; ++i) {
    const auto frame = compositor->render(plan, {});
    // A Lever A shed is decided by the DIVISOR and nothing else. Do NOT infer it
    // from `encoderSharedTexture.frameNumber != frameNumber`: on a frame where
    // D3DDecoupledExport takes its OWN bounded 3-slot refusal the published
    // number also fails to advance, with no shed having happened, so that
    // heuristic counts a refusal as a shed and the assertion fails against a
    // counter that is correct. Measured: it failed about one run in three, which
    // is worse than no test - the Task 5 review warned about exactly this
    // conflation and the sibling test above already avoids it.
    if ((frame.frameNumber % 4) != 0) ++expectedShed;
  }
  const auto after = compositor->encoderExportShedFrames();

  EXPECT_GT(expectedShed, 0) << "the compositor shed nothing, so this proves nothing";
  EXPECT_EQ(after - before, expectedShed)
      << "encoderExportShedFrames() must count exactly the frames the DIVISOR shed, "
         "no more and no less - not D3DDecoupledExport's own unrelated bounded-slot refusal";
  EXPECT_TRUE(compositor->encoderExporting())
      << "the compositor is actively exporting on a fullProgramReadback plan; "
         "encoderExporting() must say so on the SAME tick, not lag";
  EXPECT_EQ(compositor->encoderExportDivisor(), 4);
#else
  EXPECT_TRUE(true) << "The encoder-texture export gate lives in the D3D11 compositor.";
#endif
}

// #597: a backed-up sender must PUBLISH a divisor, and MediaCore must carry the
// max across senders to the compositor. Deleting either half fails this.
TEST(RtmpOutputSenderBackpressure, TheSendersDivisorReachesTheCompositor) {
  // --- Half one: the SENDER observes its own bitstream queue and publishes. ---
#if COREVIDEO_WITH_RTMP_OUTPUT
  if (senderAdmissionFfmpegPresent("TheSendersDivisorReachesTheCompositor")) {
    auto sender = corevideo::modules::createRtmpOutputSender();
    ASSERT_NE(sender, nullptr);
    auto frame = startableProgramFrame("rtmp-backpressure");
    // H.265 without the enhanced-RTMP checkbox is refused BEFORE FFmpeg is
    // launched, so this drives the real sync() path with no child process.
    const auto settings = rtmpAdmissionSettings("h265", false);

    // ABSENT IS NOT HEALTHY. With no injection and no GPU-direct path (this
    // H.265 start is refused, so the sender stays on the raw CPU path) the
    // sender has no bitstream queue to observe and must publish NOTHING -
    // which is what makes applyEncoderExportDivisor's "skip senders with no
    // backpressure" rule meaningful rather than decorative.
    for (int i = 0; i < 3; ++i) {
      (void)sender->sync({"rtmp"}, &frame, 33.0 * i, {settings});
    }
    {
      const auto session = sender->session();
      ASSERT_FALSE(session.senders.empty());
      EXPECT_FALSE(session.senders[0].backpressure.has_value())
          << "a sender with no queue to observe must publish no backpressure state";
    }

    // A healthy queue never throttles, however long the stream runs.
    sender->setBackpressureObservationForTest(0, false);
    for (int i = 0; i < 120; ++i) {
      (void)sender->sync({"rtmp"}, &frame, 33.0 * i, {settings});
    }
    {
      const auto session = sender->session();
      ASSERT_FALSE(session.senders.empty());
      ASSERT_TRUE(session.senders[0].backpressure.has_value())
          << "a sender observing its queue must publish a backpressure state";
      EXPECT_EQ(session.senders[0].backpressure->divisor, 1);
    }

    // Now hold it above the throttle threshold for longer than the enter streak.
    sender->setBackpressureObservationForTest(
        corevideo::core::StreamBackpressurePolicy::kThrottleAboveBufferedMs + 10, false);
    const int ticks =
        static_cast<int>(corevideo::core::StreamBackpressurePolicy::kEnterAfterOverWaterTicks) + 2;
    for (int i = 0; i < ticks; ++i) {
      (void)sender->sync({"rtmp"}, &frame, 4000.0 + 33.0 * i, {settings});
    }
    {
      const auto session = sender->session();
      ASSERT_FALSE(session.senders.empty());
      ASSERT_TRUE(session.senders[0].backpressure.has_value());
      EXPECT_GT(session.senders[0].backpressure->divisor, 1)
          << "a sustained backlog must raise the published input divisor";
    }

    // #597 fix round 1, finding 3: STOPPING THE STREAM CLEARS THE THROTTLE.
    // Without this the stopped record keeps publishing divisor 4 forever, the
    // compositor stays throttled with nothing streaming, and the NEXT stream
    // opens at 15 fps on an empty queue.
    sender->setBackpressureObservationForTest(-1, false);  // stop injecting
    (void)sender->sync({}, &frame, 9000.0, {settings});
    {
      const auto session = sender->session();
      ASSERT_FALSE(session.senders.empty());
      EXPECT_EQ(session.senders[0].status, "stopped");
      EXPECT_FALSE(session.senders[0].backpressure.has_value())
          << "a stopped destination must not keep publishing an input divisor";
    }
    // ...and the policy itself is reset, so the next run starts at 1 rather
    // than resuming the ladder it left off at.
    sender->setBackpressureObservationForTest(0, false);
    for (int i = 0; i < 3; ++i) {
      (void)sender->sync({"rtmp"}, &frame, 10000.0 + 33.0 * i, {settings});
    }
    {
      const auto session = sender->session();
      ASSERT_FALSE(session.senders.empty());
      ASSERT_TRUE(session.senders[0].backpressure.has_value());
      EXPECT_EQ(session.senders[0].backpressure->divisor, 1)
          << "a restarted stream must not resume the previous run's divisor";
    }
  }
#endif

  // --- Half two: MediaCore carries the MAX across senders to the compositor,
  // once on the transition and never per tick. ---
  auto modules = corevideo::modules::createStubModules();
  auto compositor = std::make_unique<DivisorRecordingCompositor>(std::move(modules.compositor));
  auto* compositorPtr = compositor.get();
  modules.compositor = std::move(compositor);
  auto senders = std::make_unique<BackpressurePublishingSender>();
  auto* sendersPtr = senders.get();
  modules.outputSender = std::move(senders);

  corevideo::core::MediaCore mediaCore(std::move(modules));
  const corevideo::rpc::Json startOutputs = corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"rtmp", "srt"}},
  };

  // One struggling destination (divisor 3) and one healthy sibling (divisor 1).
  sendersPtr->rtmpDivisor = 3;
  sendersPtr->srtDivisor = 1;
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{startOutputs});
  ASSERT_EQ(compositorPtr->divisors.size(), 1u)
      << "the struggling destination's divisor never reached the compositor";
  EXPECT_EQ(compositorPtr->divisors.back(), 3) << "MediaCore must carry the MAX across senders";

  // Steady state at the same divisor is a control-plane no-op, never per tick.
  for (int i = 0; i < 5; ++i) {
    (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{startOutputs});
  }
  EXPECT_EQ(compositorPtr->divisors.size(), 1u)
      << "setEncoderExportDivisor must be called only when the value CHANGES";

  // The max is taken across ALL senders, not the first one: move the backlog
  // onto the SECOND destination and the compositor must follow it there.
  sendersPtr->rtmpDivisor = 1;
  sendersPtr->srtDivisor = 4;
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{startOutputs});
  ASSERT_EQ(compositorPtr->divisors.size(), 2u);
  EXPECT_EQ(compositorPtr->divisors.back(), 4)
      << "the MAX must be taken across every sender, not just the first";

  // Recovery travels the same way.
  sendersPtr->srtDivisor = 1;
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{startOutputs});
  ASSERT_EQ(compositorPtr->divisors.size(), 3u);
  EXPECT_EQ(compositorPtr->divisors.back(), 1);
}

// #597 fix round 2, item 4: THE FALSE-DEGRADED HALF, and the one the reviewers
// kept having to re-judge. Task 4 left the stop path alone because nothing read
// the divisor, so a latched value was inert. Task 6 published it, and the same
// residual became a node that reads "throttled" with nothing streaming at all -
// worse than no node, because `encoderExport.exporting` is fullProgramReadback
// (vcam OR output OR recording), so it is TRUE between shows whenever the
// virtual camera is on. Together they make an idle machine look degraded.
//
// The fix cannot come from asking the sender: `AsyncOutputSender::sync()`
// returns a CACHED pre-stop snapshot, so on the tick the last destination goes
// away the sender still reports the divisor it had while live. MediaCore's own
// `senderDestinations` list is the authoritative, synchronous answer. The fake
// sender below models the cache exactly - it keeps reporting divisor 4 forever,
// destinations or not - so this test fails if anyone ever "simplifies"
// renderVideoOutputTick back to trusting the sender's snapshot.
TEST(RtmpOutputSenderBackpressure, TheDivisorReturnsToOneWhenNothingIsStreaming) {
  auto modules = corevideo::modules::createStubModules();
  auto compositor = std::make_unique<DivisorRecordingCompositor>(std::move(modules.compositor));
  auto* compositorPtr = compositor.get();
  modules.compositor = std::move(compositor);
  auto senders = std::make_unique<BackpressurePublishingSender>();
  auto* sendersPtr = senders.get();
  modules.outputSender = std::move(senders);

  corevideo::core::MediaCore mediaCore(std::move(modules));
  std::mutex coreMutex;

  // A struggling destination, live: the compositor is throttled to 1-in-4.
  sendersPtr->rtmpDivisor = 4;
  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"rtmp"}},
  });
  mediaCore.renderVideoOutputTick(coreMutex);
  ASSERT_FALSE(compositorPtr->divisors.empty())
      << "the live divisor never reached the compositor, so this test proves nothing";
  EXPECT_EQ(compositorPtr->divisors.back(), 4);

  // The operator stops the stream. The sender STILL reports divisor 4 - that is
  // the cached snapshot, not a bug in the fake - but nothing is streaming, so
  // the compositor must be released back to every frame.
  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{}},
  });
  mediaCore.renderVideoOutputTick(coreMutex);
  ASSERT_GE(compositorPtr->divisors.size(), 2u)
      << "nothing was pushed to the compositor when the last destination went away";
  EXPECT_EQ(compositorPtr->divisors.back(), 1)
      << "with no destination streaming, the published divisor must be 1 - a "
         "latched 2-4 reads as a live throttle on an idle machine";
}

// #597 fix round 2, finding 1: the real red/green, no seam, no injection, no
// reorder - drives the ACTUAL production defect through the public sync() path.
//
// The mechanism: `ensureFfmpegProcess` sets `useGpuDirect_ = desiredGpuDirect`
// (the sender's own request) BEFORE it knows whether FFmpeg will start, then
// calls `startFfmpegProcess`. That function's FIRST admission block - the
// codec-compatibility refusal (H.265 over RTMP without Enhanced RTMP ticked) -
// returns false WITHOUT clearing `useGpuDirect_` (only the SECOND admission
// block, below the GPU-encoder start, clears it). `ensureFfmpegProcess` then
// sets `activeUseGpuDirect_ = false` on that same failed-start path. So a
// destination that WANTS the GPU path (a frame carrying a non-empty
// `encoderSharedTexture.sharedHandleHex` - `resolveGpuEncodePath`'s only input
// besides the codec/probe, no D3D11 compositor required to set it) and gets
// refused for an unrelated compatibility reason lands EXACTLY in finding 1's
// state: `useGpuDirect_ == true`, `activeUseGpuDirect_ == false`, no running
// FFmpeg, no queue. Before the fix, `observeStreamBackpressure()` gated on the
// former and published a pristine `divisor 1 / bufferedMs 0` node for that
// destination; after the fix it gates on the latter and correctly goes absent.
TEST(RtmpOutputSenderBackpressure, AGpuDirectRequestRefusedForCompatibilityPublishesNoBackpressure) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  if (!senderAdmissionFfmpegPresent(
          "AGpuDirectRequestRefusedForCompatibilityPublishesNoBackpressure")) {
    return;
  }
  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);
  auto frame = startableProgramFrame("rtmp-finding1-no-seam");
  // The one thing that makes this destination WANT the GPU path: a non-empty
  // encoder-texture handle. No compositor is needed to set this field.
  frame.encoderSharedTexture.sharedHandleHex = "0xDEADBEEF";
  // H.265 without Enhanced RTMP ticked: refused at startFfmpegProcess's FIRST
  // admission block (compatibility), before the GPU-encoder-start clause that
  // would otherwise clear useGpuDirect_ back down.
  const auto settings = rtmpAdmissionSettings("h265", /*allowEnhancedRtmp=*/false);

  // Two syncs: the first arms the desired state and attempts (and fails) the
  // start; the second observes the resulting steady state.
  (void)sender->sync({"rtmp"}, &frame, 0.0, {settings});
  const auto session = sender->sync({"rtmp"}, &frame, 33.0, {settings});

  ASSERT_FALSE(session.senders.empty());
  EXPECT_EQ(session.senders[0].lastResultCode, "enhanced-rtmp-required")
      << "this test proves nothing unless the refusal actually happened for "
         "the GPU-direct compatibility reason";
  EXPECT_FALSE(session.senders[0].backpressure.has_value())
      << "a destination that wanted the GPU path and has no running FFmpeg "
         "must read ABSENT, not a pristine divisor-1/bufferedMs-0 healthy node "
         "(#597 fix round 1, finding 1)";
#else
  EXPECT_TRUE(true) << "Needs the RTMP sender.";
#endif
}

// #597 fix round 1, finding 1/2 - THE COMBINATION NOTHING COVERED: a REAL
// RtmpOutputSender looking at REAL frames a throttled compositor has shed.
//
// The sender decides GPU-direct vs the raw CPU path from
// `!frame.encoderSharedTexture.sharedHandleHex.empty()`, and
// ensureFfmpegProcess tears down FFmpeg AND the hardware encoder and relaunches
// both whenever that decision changes. So if Lever A sheds by withholding the
// handle, every shed frame is an encoder restart - tens per second, where the
// incident that started this whole sub-project was EIGHT in twenty seconds. For
// H.265 it is worse still: the restart is admitted off the GPU path,
// StreamStartAdmission refuses it, startRefusedInadmissible_ latches, and the
// stream is dead for the rest of the show.
//
// Shedding must therefore skip the SUBMIT and keep publishing the handle. This
// test walks the exact comparison ensureFfmpegProcess makes over 40 real frames
// from a compositor at divisor 2 and asserts the restart count does not grow.
TEST(RtmpOutputSenderBackpressure, AShedFrameNeverRestartsTheSendersEncodePath) {
#if COREVIDEO_WITH_D3D11 && COREVIDEO_WITH_RTMP_OUTPUT
  if (!senderAdmissionFfmpegPresent("AShedFrameNeverRestartsTheSendersEncodePath")) return;
  auto compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_NE(compositor, nullptr);
  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);

  corevideo::modules::CompositorRenderPlan plan;
  plan.renderPlanId = "backpressure-no-restart";
  plan.sceneId = "backpressure-no-restart";
  plan.width = 320;
  plan.height = 180;
  plan.fullProgramReadback = true;
  plan.skipCpuReadback = true;

  // One sync to configure the sender (codec, endpoint, runtime). H.265 without
  // the enhanced-RTMP checkbox is refused before FFmpeg is launched, so no child
  // process and no hardware encoder session is created by this test.
  auto configuring = compositor->render(plan, {});
  (void)sender->sync({"rtmp"}, &configuring, 0, {rtmpAdmissionSettings("h265", false)});

  // Warm the path decision on an EXPORTED frame, so the first observed flip is
  // the ordinary one-time start rather than an artefact of the initial state.
  compositor->setEncoderExportDivisor(1);
  auto warm = compositor->render(plan, {});
  const bool warmIsGpuDirect = sender->wouldRestartForEncodePathForTest(warm);
  if (!warmIsGpuDirect) {
    // Already on the GPU path, or this machine has no hardware encoder at all.
    // The latter would make the whole test vacuous, so say so and stop.
    std::fprintf(stderr,
                 "[  SKIPPED  ] RtmpOutputSenderBackpressure."
                 "AShedFrameNeverRestartsTheSendersEncodePath (no GPU-direct path on this"
                 " machine) - this test did NOT run\n");
    return;
  }

  // Now throttle, and walk 40 real frames: 20 exported, 20 shed.
  compositor->setEncoderExportDivisor(2);
  int restarts = 0;
  int shed = 0;
  for (int i = 0; i < 40; ++i) {
    auto frame = compositor->render(plan, {});
    if (frame.encoderSharedTexture.frameNumber != frame.frameNumber) ++shed;
    if (sender->wouldRestartForEncodePathForTest(frame)) ++restarts;
  }

  EXPECT_GT(shed, 0) << "the compositor shed nothing, so this proves nothing";
  EXPECT_EQ(restarts, 0)
      << "the sender flipped its encode path on a shed frame: that is an FFmpeg + "
         "hardware-encoder teardown and relaunch per shed frame (#597 itself), and "
         "for H.265 a permanent start refusal";
#else
  EXPECT_TRUE(true) << "Needs both the D3D11 compositor and the RTMP sender.";
#endif
}

// #597 Lever B, fix round 1 (review finding 4): the discard-correctness
// boundary conditions (no keyframe queued, keyframe at the head, an
// all-keyframe queue, the exact GOP tail dropped, the buffered measure
// republished after a discard) are now `StreamBackpressurePolicy,
// DiscardableGopTailLength*` in StreamBackpressurePolicyTest.cpp, against the
// pure core::discardableGopTailLength() directly - no sender, no queue, no
// seam of any kind. What is NOT provable there is that
// observeStreamBackpressure()/sync() actually REACH
// discardBacklogToNextKeyframe() on a real sender: deleting the entire
// `if (decision.discardBacklog) { ... }` block left every discard-shaped test
// green as long as it only drove the pure function or an empty real queue.
// This test builds a REAL backlog on the real bitstream queue via the
// enqueueBitstreamChunkForTest seam, pushes the DECISION (not the queue) past
// the discard threshold via Task 4's setBackpressureObservationForTest, and
// asserts the real queue actually shrank - which only happens if the call
// site is wired.
TEST(RtmpOutputSenderBackpressure, DiscardBacklogReachesTheQueueThroughSyncAndObserveStreamBackpressure) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  if (senderAdmissionFfmpegPresent(
          "DiscardBacklogReachesTheQueueThroughSyncAndObserveStreamBackpressure")) {
    auto sender = corevideo::modules::createRtmpOutputSender();
    ASSERT_NE(sender, nullptr);
    auto frame = startableProgramFrame("rtmp-backpressure-discard");
    // H.265 without the enhanced-RTMP checkbox is refused BEFORE FFmpeg is
    // launched (and after observeStreamBackpressure(), which sits above every
    // admission refusal), so this drives the real path with no child process.
    const auto settings = rtmpAdmissionSettings("h265", false);

    // Lever B only ever fires once the divisor is already above 1 - get there
    // first, the same way TheSendersDivisorReachesTheCompositor does.
    sender->setBackpressureObservationForTest(
        corevideo::core::StreamBackpressurePolicy::kThrottleAboveBufferedMs + 10,
        /*keyframeInQueue=*/true);
    const int enterTicks =
        static_cast<int>(corevideo::core::StreamBackpressurePolicy::kEnterAfterOverWaterTicks) + 2;
    for (int i = 0; i < enterTicks; ++i) {
      (void)sender->sync({"rtmp"}, &frame, 33.0 * i, {settings});
    }
    {
      const auto session = sender->session();
      ASSERT_FALSE(session.senders.empty());
      ASSERT_TRUE(session.senders[0].backpressure.has_value());
      ASSERT_GT(session.senders[0].backpressure->divisor, 1)
          << "the divisor must be above 1 before a discard can fire";
    }

    // Build a REAL backlog on the REAL queue: two STALE reference frames
    // (aged by the sleep below, so the head's bufferedMs is provably old),
    // then a fresh keyframe, then one fresh reference frame that must
    // survive. This also lets the same test prove republishQueueTelemetryLocked()
    // fires: bufferedMs must fall once the stale head is discarded, or
    // bitstreamBufferedMs() would keep reporting the age of a chunk that no
    // longer exists (the risk named in the original brief).
    sender->enqueueBitstreamChunkForTest(1000, /*keyframe=*/false);
    sender->enqueueBitstreamChunkForTest(1000, /*keyframe=*/false);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    sender->enqueueBitstreamChunkForTest(2000, /*keyframe=*/true);
    sender->enqueueBitstreamChunkForTest(1000, /*keyframe=*/false);
    const auto before = sender->bitstreamQueueSnapshotForTest();
    ASSERT_EQ(before.depth, 4u);
    EXPECT_GE(before.bufferedMs, 50) << "the head is the STALE chunk before the discard";

    // Push the injected OBSERVATION (not the queue) above the discard
    // threshold and tick once more.
    sender->setBackpressureObservationForTest(
        corevideo::core::StreamBackpressurePolicy::kDiscardAboveBufferedMs + 10,
        /*keyframeInQueue=*/true);
    (void)sender->sync({"rtmp"}, &frame, 5000.0, {settings});

    const auto after = sender->bitstreamQueueSnapshotForTest();
    EXPECT_EQ(after.depth, 2u)
        << "decision.discardBacklog must have reached discardBacklogToNextKeyframe() "
           "through observeStreamBackpressure()/sync() and dropped the two stale chunks "
           "ahead of the keyframe - deleting that call site leaves this at 4";
    EXPECT_TRUE(after.hasKeyframe) << "the keyframe itself must never be dropped";
    EXPECT_LT(after.bufferedMs, before.bufferedMs)
        << "the buffered measure must republish against the NEW head (the keyframe), not "
           "keep reporting the age of the chunk the discard just dropped";
    EXPECT_LT(after.bufferedMs, 50) << "the new head (the keyframe) was enqueued moments ago";
  }
#else
  // The local gtest shim has no GTEST_SKIP; say so loudly rather than pass silently.
  std::fprintf(stderr, "[  SKIPPED ] RtmpOutputSenderBackpressure.DiscardBacklogReachesTheQueueThroughSyncAndObserveStreamBackpressure"
                       " (Needs the RTMP sender) - this test did NOT run\n");
  return;
#endif
}

// #597 Task 8b. THE DEFECT the acceptance gate (Task 8) measured: the outgoing
// queue's overflow path failed the sender ON PURPOSE so the supervisor would
// restart it - and a supervisor restart rebuilds the encoder, which is the one
// thing this whole sub-project exists to stop. A destination fault must never
// rebuild the encoder.
//
// The bound itself is not optional (an unbounded queue is unbounded latency),
// so the fix is to spend Lever B at the moment it matters most: on overflow,
// run the GOP-tail discard FIRST and accept the chunk if that freed room.
// Here the full queue CONTAINS a keyframe, so there is a safe unit to drop and
// the sender must survive.
//
// Drives the REAL enqueueBitstream() through offerBitstreamChunkForTest - the
// direct-push seam next to it deliberately bypasses the bound, so it cannot
// see this at all.
TEST(RtmpOutputSenderBackpressure, AFullQueueHoldingAKeyframeDiscardsItsGopTailInsteadOfFailingTheSender) {
#if COREVIDEO_WITH_RTMP_OUTPUT && defined(_WIN32)
  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);

  // Fill the real queue to its hard cap with a keyframe 30 chunks in - the
  // middle of the GOP, the ordinary case at 60 fps with a 1 s GOP.
  constexpr std::size_t kCap = 60;
  constexpr std::size_t kKeyframeIndex = 30;
  for (std::size_t i = 0; i < kCap; ++i) {
    sender->enqueueBitstreamChunkForTest(1000, /*keyframe=*/i == kKeyframeIndex);
  }
  const auto before = sender->bitstreamQueueSnapshotForTest();
  ASSERT_EQ(before.depth, kCap) << "the queue must actually be AT the cap, or this proves nothing";
  ASSERT_TRUE(before.hasKeyframe);
  ASSERT_FALSE(before.overflowFailed);

  // One more chunk arrives from the encoder with the queue already full.
  sender->offerBitstreamChunkForTest(1000, /*keyframe=*/false);

  const auto after = sender->bitstreamQueueSnapshotForTest();
  EXPECT_FALSE(after.overflowFailed)
      << "the overflow path failed the sender while a keyframe was queued: its supervisor "
         "will restart it and rebuild the encoder, which is #597 itself";
  EXPECT_LT(after.depth, before.depth)
      << "the queue must have SHRUNK - the discard is what makes room for the arriving chunk";
  EXPECT_EQ(after.depth, kCap - kKeyframeIndex + 1)
      << "exactly the GOP tail ahead of the keyframe is dropped, and the arriving chunk is "
         "then accepted";
  EXPECT_TRUE(after.hasKeyframe) << "the keyframe itself must never be dropped";
#else
  // The local gtest shim has no GTEST_SKIP; say so loudly rather than pass silently.
  std::fprintf(stderr, "[  SKIPPED ] RtmpOutputSenderBackpressure.AFullQueueHoldingAKeyframeDiscardsItsGopTailInsteadOfFailingTheSender"
                       " (Needs the Windows RTMP sender's bitstream queue) - this test did NOT run\n");
  return;
#endif
}

// #597 Task 8b fix round 1. The residual case the first cut still failed on,
// and the reason the ruling changed: the discard used to cut to the FIRST
// queued keyframe, so a full queue whose ONLY keyframe sat at the HEAD freed
// nothing and failed the sender anyway - and with the measured GOP (60 frames)
// about the size of the cap (60 chunks), where the keyframe sits is a rolling
// coin flip. The overflow path now cuts to the LAST queued keyframe. Same
// safety argument (every keyframe here is a self-contained IDR), strictly more
// room freed, and nothing about which preceding chunks are headers.
TEST(RtmpOutputSenderBackpressure, AFullQueueWhoseKeyframeIsAtTheHeadStillFreesRoomInsteadOfFailing) {
#if COREVIDEO_WITH_RTMP_OUTPUT && defined(_WIN32)
  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);

  // The cap, with a keyframe at the HEAD and one more mid-queue - the ordinary
  // steady state at 60 fps with a 1 s GOP and a 60-chunk cap.
  constexpr std::size_t kCap = 60;
  constexpr std::size_t kSecondKeyframeIndex = 45;
  for (std::size_t i = 0; i < kCap; ++i) {
    sender->enqueueBitstreamChunkForTest(1000, /*keyframe=*/i == 0 || i == kSecondKeyframeIndex);
  }
  const auto before = sender->bitstreamQueueSnapshotForTest();
  ASSERT_EQ(before.depth, kCap);
  ASSERT_TRUE(before.hasKeyframe);

  sender->offerBitstreamChunkForTest(1000, /*keyframe=*/false);

  const auto after = sender->bitstreamQueueSnapshotForTest();
  EXPECT_FALSE(after.overflowFailed)
      << "cutting to the FIRST keyframe frees 0 here and fails the sender - which restarts it "
         "and rebuilds the encoder, #597 itself";
  EXPECT_EQ(after.depth, kCap - kSecondKeyframeIndex + 1)
      << "the overflow path must cut to the LAST queued keyframe, freeing the most room a safe "
         "cut can free";
  EXPECT_TRUE(after.hasKeyframe) << "the keyframe cut TO must never be dropped";
#else
  // The local gtest shim has no GTEST_SKIP; say so loudly rather than pass silently.
  std::fprintf(stderr, "[  SKIPPED ] RtmpOutputSenderBackpressure.AFullQueueWhoseKeyframeIsAtTheHeadStillFreesRoomInsteadOfFailing"
                       " (Needs the Windows RTMP sender's bitstream queue) - this test did NOT run\n");
  return;
#endif
}

// #597 Task 8b fix round 1, the case the BURST gate measured (and the reason
// the arriving chunk became a cut point). A full queue of pure reference
// frames - no keyframe anywhere, because the window is one GOP wide and the
// last keyframe has already drained - while the chunk being refused is itself
// a keyframe. Cutting to a QUEUED keyframe frees nothing here and the sender
// fails; the decoder can resume at the ARRIVING IDR, so the whole backlog goes.
TEST(RtmpOutputSenderBackpressure, AKeyframeArrivingAtAFullAllReferenceQueueReplacesTheWholeBacklog) {
#if COREVIDEO_WITH_RTMP_OUTPUT && defined(_WIN32)
  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);

  constexpr std::size_t kCap = 60;
  for (std::size_t i = 0; i < kCap; ++i) {
    sender->enqueueBitstreamChunkForTest(1000, /*keyframe=*/false);
  }
  const auto before = sender->bitstreamQueueSnapshotForTest();
  ASSERT_EQ(before.depth, kCap);
  ASSERT_FALSE(before.hasKeyframe) << "the measured shape: 60 queued chunks, no keyframe among them";

  sender->offerBitstreamChunkForTest(2000, /*keyframe=*/true);

  const auto after = sender->bitstreamQueueSnapshotForTest();
  EXPECT_FALSE(after.overflowFailed)
      << "the sender failed with a usable cut point at the door - that restarts it and rebuilds "
         "the encoder, which is #597 itself";
  EXPECT_EQ(after.depth, 1u) << "the backlog is replaced by the arriving IDR";
  EXPECT_TRUE(after.hasKeyframe);
#else
  // The local gtest shim has no GTEST_SKIP; say so loudly rather than pass silently.
  std::fprintf(stderr, "[  SKIPPED ] RtmpOutputSenderBackpressure.AKeyframeArrivingAtAFullAllReferenceQueueReplacesTheWholeBacklog"
                       " (Needs the Windows RTMP sender's bitstream queue) - this test did NOT run\n");
  return;
#endif
}

// #597 Task 8b, the other half: a bounded queue is NOT optional. With NO
// keyframe queued there is nothing safe to drop - dropping an arbitrary chunk
// corrupts every frame until the next keyframe - so the overflow must still
// fail the sender. Without this the fix above would read as "never bound the
// queue", which is unbounded latency: the defect this sub-project removes.
TEST(RtmpOutputSenderBackpressure, AFullQueueWithNoKeyframeStillFailsTheSender) {
#if COREVIDEO_WITH_RTMP_OUTPUT && defined(_WIN32)
  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);

  constexpr std::size_t kCap = 60;
  for (std::size_t i = 0; i < kCap; ++i) {
    sender->enqueueBitstreamChunkForTest(1000, /*keyframe=*/false);
  }
  const auto before = sender->bitstreamQueueSnapshotForTest();
  ASSERT_EQ(before.depth, kCap);
  ASSERT_FALSE(before.hasKeyframe) << "this case is defined by there being nothing safe to drop";

  sender->offerBitstreamChunkForTest(1000, /*keyframe=*/false);

  const auto after = sender->bitstreamQueueSnapshotForTest();
  EXPECT_TRUE(after.overflowFailed)
      << "with no keyframe queued the discard frees nothing, and an unbounded queue is "
         "unbounded latency - the bound must still bite";
  EXPECT_EQ(after.depth, kCap) << "nothing may be dropped, and nothing may be accepted";
#else
  // The local gtest shim has no GTEST_SKIP; say so loudly rather than pass silently.
  std::fprintf(stderr, "[  SKIPPED ] RtmpOutputSenderBackpressure.AFullQueueWithNoKeyframeStillFailsTheSender"
                       " (Needs the Windows RTMP sender's bitstream queue) - this test did NOT run\n");
  return;
#endif
}

// #597 Task 6: the review finding this whole task exists to close. Publishing
// `backpressure->discardedChunks` made the counter reset OBSERVABLE for the
// first time - and therefore testable for the first time. Task 4/5 added
// `backpressureDiscardedChunks_ = 0` beside the `backpressure_ = {}` reset on
// the `!wantsRtmp` stop path, but nothing before this test could see whether
// that reset actually reached anything a consumer reads. A destination that
// discarded chunks, then stopped, must not report the previous run's discards
// on its NEXT run.
TEST(RtmpOutputSenderBackpressure, ADiscardedChunkCounterResetsOnTheNextStreamRun) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  if (!senderAdmissionFfmpegPresent("ADiscardedChunkCounterResetsOnTheNextStreamRun")) return;
  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);
  auto frame = startableProgramFrame("rtmp-backpressure-discard-reset");
  // H.265 without the enhanced-RTMP checkbox is refused before FFmpeg is
  // launched, so this drives the real sync()/observeStreamBackpressure() path
  // with no child process - exactly like the discard test above it.
  const auto settings = rtmpAdmissionSettings("h265", false);

  // --- Run 1: drive the divisor above 1, then force a real discard. ---
  sender->setBackpressureObservationForTest(
      corevideo::core::StreamBackpressurePolicy::kThrottleAboveBufferedMs + 10,
      /*keyframeInQueue=*/true);
  const int enterTicks =
      static_cast<int>(corevideo::core::StreamBackpressurePolicy::kEnterAfterOverWaterTicks) + 2;
  for (int i = 0; i < enterTicks; ++i) {
    (void)sender->sync({"rtmp"}, &frame, 33.0 * i, {settings});
  }
  sender->enqueueBitstreamChunkForTest(1000, /*keyframe=*/false);
  sender->enqueueBitstreamChunkForTest(1000, /*keyframe=*/false);
  sender->enqueueBitstreamChunkForTest(2000, /*keyframe=*/true);
  sender->setBackpressureObservationForTest(
      corevideo::core::StreamBackpressurePolicy::kDiscardAboveBufferedMs + 10,
      /*keyframeInQueue=*/true);
  (void)sender->sync({"rtmp"}, &frame, 5000.0, {settings});
  std::int64_t run1RunId = -1;
  {
    const auto session = sender->session();
    ASSERT_FALSE(session.senders.empty());
    ASSERT_TRUE(session.senders[0].backpressure.has_value());
    const auto& bp = *session.senders[0].backpressure;
    ASSERT_GT(bp.discardedChunks, 0)
        << "run 1 must have actually discarded something, or this test proves nothing";
    // The rest of run 1's per-run state, so the fix-round-1 extension below has
    // something real to compare against: a divisor above 1, at least one
    // engagement, and at least one discard event (distinct from discardedChunks
    // - see the divergence documented on OutputBackpressureState).
    ASSERT_GT(bp.divisor, 1);
    ASSERT_GT(bp.enteredCount, 0);
    ASSERT_GT(bp.discardEvents, 0);
    run1RunId = bp.runId;
  }

  // --- Stop the destination: the same `!wantsRtmp` path that resets
  // backpressure_ and backpressureDiscardedChunks_ together. ---
  sender->setBackpressureObservationForTest(-1, false);  // stop injecting
  (void)sender->sync({}, &frame, 9000.0, {settings});
  {
    const auto session = sender->session();
    ASSERT_FALSE(session.senders.empty());
    EXPECT_EQ(session.senders[0].status, "stopped");
    EXPECT_FALSE(session.senders[0].backpressure.has_value())
        << "a stopped destination must not keep publishing its last discard count";
  }

  // --- Run 2: a fresh stream, never throttled, no discard fired. ---
  sender->setBackpressureObservationForTest(0, false);
  for (int i = 0; i < 3; ++i) {
    (void)sender->sync({"rtmp"}, &frame, 10000.0 + 33.0 * i, {settings});
  }
  {
    const auto session = sender->session();
    ASSERT_FALSE(session.senders.empty());
    ASSERT_TRUE(session.senders[0].backpressure.has_value());
    const auto& bp = *session.senders[0].backpressure;
    EXPECT_EQ(bp.discardedChunks, 0)
        << "the next stream run must not report the PREVIOUS run's discards as its own";
    // Fix round 1, finding 7: the stop path resets the WHOLE policy object
    // (`backpressure_ = StreamBackpressurePolicy{}`), not just
    // backpressureDiscardedChunks_ beside it - pin all four other per-run
    // fields too. Deleting the policy reset alone (leaving only the discard
    // counter reset) would open the next stream at the PREVIOUS run's divisor
    // and pass every assertion above while failing these.
    EXPECT_EQ(bp.divisor, 1)
        << "a fresh, never-throttled run must not open at the previous run's divisor";
    EXPECT_EQ(bp.level, 0);
    EXPECT_EQ(bp.enteredCount, 0)
        << "a fresh run must not carry forward the previous run's engagement count";
    EXPECT_EQ(bp.discardEvents, 0)
        << "a fresh run must not carry forward the previous run's discard-event count";
    // Fix round 1, finding 11: the reset must mint a NEW run identity, so a
    // consumer that polled across the stop without observing the node's
    // momentary absence can still tell this is a reset, not the same run's
    // counters somehow decreasing.
    EXPECT_NE(bp.runId, run1RunId) << "a stop/restart must mint a new runId";
  }
#else
  // The local gtest shim has no GTEST_SKIP; say so loudly rather than pass silently.
  std::fprintf(stderr, "[  SKIPPED ] RtmpOutputSenderBackpressure.ADiscardedChunkCounterResetsOnTheNextStreamRun"
                       " (Needs the RTMP sender) - this test did NOT run\n");
  return;
#endif
}

// #597 FINAL REVIEW, FINDING 2 - RED/GREEN, and a CROSS-TASK defect no
// per-task review could see. Task 7 owned the restart path; Tasks 4 and 6
// owned the policy lifetime; only the operator-STOP path reconstructed the
// policy. So after any fault and reopen a destination republished its
// pre-failure divisor against an EMPTY queue and needed ~30 s of healthy
// streaming (kRecoverAfterHealthyTicks = 600, one step per 10 s) to return to
// full rate, while the compositor visibly snapped 4 -> 1 -> 4 across the
// outage with lastReason still reading the pre-fault value.
//
// Deleting `resetBackpressureForNewRun()` from reopen() turns this red on the
// divisor assertion alone.
TEST(RtmpOutputSenderBackpressure, AReopenedTransportStartsUnthrottledNotAtItsPreFailureDivisor) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  if (!senderAdmissionFfmpegPresent(
          "AReopenedTransportStartsUnthrottledNotAtItsPreFailureDivisor")) return;
  const auto settings = rtmpAdmissionSettings("h265", false);
  const int enterTicks =
      static_cast<int>(corevideo::core::StreamBackpressurePolicy::kEnterAfterOverWaterTicks) + 2;

  // Both reopen doors must reset: the SUPERVISOR's automatic restart and the
  // OPERATOR's recover(). They differ only in whether the restart floor is
  // cleared, and a fix applied to one of them would pass a test that only
  // drives the other.
  for (int door = 0; door < 2; ++door) {
    const bool viaSupervisor = door == 0;
    auto sender = corevideo::modules::createRtmpOutputSender();
    ASSERT_NE(sender, nullptr);
    auto frame = startableProgramFrame("rtmp-backpressure-reopen");

    // --- Climb to the floor of the ladder on a congested link. ---
    sender->setBackpressureObservationForTest(
        corevideo::core::StreamBackpressurePolicy::kThrottleAboveBufferedMs + 10,
        /*keyframeInQueue=*/true);
    double t = 0.0;
    for (int step = 0; step < corevideo::core::StreamBackpressurePolicy::kMaxDivisor; ++step) {
      for (int i = 0; i < enterTicks; ++i) {
        t += 33.0;
        (void)sender->sync({"rtmp"}, &frame, t, {settings});
      }
    }
    std::int64_t beforeRunId = -1;
    {
      const auto session = sender->session();
      ASSERT_FALSE(session.senders.empty());
      ASSERT_TRUE(session.senders[0].backpressure.has_value());
      const auto& bp = *session.senders[0].backpressure;
      ASSERT_GT(bp.divisor, 1)
          << "the precondition failed: nothing was throttled, so the reopen proves nothing";
      ASSERT_GT(bp.enteredCount, 0);
      beforeRunId = bp.runId;
    }

    // --- The transport faults and is reopened. The queue is now EMPTY. ---
    t += 33.0;
    if (viaSupervisor) {
      (void)sender->restartForSupervisor("rtmp", t, "transport fault");
    } else {
      (void)sender->recover("rtmp", t, "operator re-armed");
    }

    // --- The first observation after the reopen sees a healthy, empty queue. ---
    sender->setBackpressureObservationForTest(0, /*keyframeInQueue=*/false);
    t += 33.0;
    (void)sender->sync({"rtmp"}, &frame, t, {settings});

    const auto session = sender->session();
    ASSERT_FALSE(session.senders.empty());
    ASSERT_TRUE(session.senders[0].backpressure.has_value());
    const auto& bp = *session.senders[0].backpressure;
    EXPECT_EQ(bp.divisor, 1)
        << (viaSupervisor ? "supervisor restart" : "operator recover")
        << ": a reopened transport republished its PRE-FAILURE divisor against an empty queue, "
           "so the compositor stays throttled for ~30s of healthy streaming for nothing";
    EXPECT_EQ(bp.level, 0);
    EXPECT_EQ(bp.enteredCount, 0)
        << "a reopened transport is a new run; it must not carry the old run's engagement count";
    EXPECT_EQ(bp.discardEvents, 0);
    EXPECT_EQ(bp.discardedChunks, 0);
    EXPECT_STREQ(bp.lastReason, "none")
        << "lastReason must not still read the pre-fault reason after a reopen";
    EXPECT_NE(bp.runId, beforeRunId)
        << "a reopen resets the per-run counters, so it must mint a new run identity";
  }
#else
  // The local gtest shim has no GTEST_SKIP; say so loudly rather than pass silently.
  std::fprintf(stderr, "[  SKIPPED ] RtmpOutputSenderBackpressure.AReopenedTransportStartsUnthrottledNotAtItsPreFailureDivisor"
                       " (Needs the RTMP sender) - this test did NOT run\n");
  return;
#endif
}

TEST(OutputSenderAdapter, RtmpWritesSendProofArtifactWhenArmed) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);

  corevideo::modules::ProgramFrame frame{1920, 1080, 2, 7, "rtmp-proof-plan", "d3d11"};
  corevideo::modules::OutputDestinationSettings settings;
  settings.id = "rtmp";
  settings.label = "RTMP";
  settings.protocol = "rtmps";
  settings.url = "rtmps://live.example.com/app";
  settings.streamKey = "stream-key";
  const auto session = sender->sync({"rtmp"}, &frame, 33, {settings});
  ASSERT_FALSE(session.senders.empty());
  ASSERT_FALSE(session.senders[0].sendArtifactPath.empty());
  EXPECT_FALSE(session.senders[0].runtimeDetail.empty());
  EXPECT_TRUE(session.senders[0].sendBytesWritten > 0);
  ASSERT_TRUE(std::filesystem::exists(session.senders[0].sendArtifactPath));

  std::ifstream input(session.senders[0].sendArtifactPath);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const auto content = buffer.str();
  EXPECT_NE(content.find("rtmp-send-proof-start"), std::string::npos);
  EXPECT_NE(content.find("runtimeDetail"), std::string::npos);
  EXPECT_NE(content.find("runtimeCandidates"), std::string::npos);
  EXPECT_NE(content.find("endpointMode"), std::string::npos);
  EXPECT_NE(content.find("packagingSignal"), std::string::npos);
  EXPECT_NE(content.find("rtmp-send-attempt"), std::string::npos);
  input.close();
  const auto artifactPath = session.senders[0].sendArtifactPath;
  sender.reset();
  std::filesystem::remove(artifactPath);
#else
  EXPECT_TRUE(true);
#endif
}

TEST(CaptureDeviceAdapter, FactoriesAreDisabledUnlessHardwareGatesAreEnabled) {
#if COREVIDEO_WITH_DECKLINK
  auto deckLink = corevideo::modules::createDeckLinkCaptureDevice();
  ASSERT_NE(deckLink, nullptr);
  const auto deckLinkDevices = deckLink->enumerate();
  ASSERT_FALSE(deckLinkDevices.empty());
  EXPECT_EQ(deckLinkDevices[0].vendor, "blackmagic");
#else
  EXPECT_EQ(corevideo::modules::createDeckLinkCaptureDevice(), nullptr);
#endif

#if COREVIDEO_WITH_AJA
  auto aja = corevideo::modules::createAjaCaptureDevice();
  ASSERT_NE(aja, nullptr);
  const auto ajaDevices = aja->enumerate();
  ASSERT_FALSE(ajaDevices.empty());
  EXPECT_EQ(ajaDevices[0].vendor, "aja");
#else
  EXPECT_EQ(corevideo::modules::createAjaCaptureDevice(), nullptr);
#endif
}

TEST(ZoomMeetingSdkAdapter, FactoryIsDisabledInPortableStubBuild) {
#if COREVIDEO_WITH_ZOOM
  EXPECT_TRUE(true);
#else
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({});
  EXPECT_EQ(source, nullptr);
#endif
}

TEST(ZoomMeetingSdkAdapter, DevGateRejectsMissingJoinCredentials) {
#if COREVIDEO_WITH_ZOOM
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({
      "sdk-root",
      "7.0.5",
      "https://zoom.us",
      true,
      true,
      true,
      true,
      true,
      true,
  });
  ASSERT_NE(source, nullptr);

  const bool joined = source->join({
      "123456789",
      "CoreVideo Producer",
  });

  EXPECT_FALSE(joined);
  EXPECT_EQ(source->meetingState(), "join-ready");
  EXPECT_FALSE(source->warnings().empty());
  source->leave();
  EXPECT_EQ(source->meetingState(), "idle");
#else
  EXPECT_TRUE(true);
#endif
}

TEST(ZoomMeetingSdkAdapter, DevGateTracksDeferredRawSubscriptions) {
#if COREVIDEO_WITH_ZOOM
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({
      "sdk-root",
      "7.0.5",
      "https://zoom.us",
      true,
      true,
      true,
      true,
      true,
      true,
  });
  ASSERT_NE(source, nullptr);

  source->syncSubscriptions({
      {"12345", "participant-video", "program", 1},
      {"12345", "participant-audio", "mix", 2},
  });

  const auto states = source->subscriptionStates();
  ASSERT_TRUE(states.size() == 2);
  EXPECT_EQ(states[0].status, "failed");
  EXPECT_EQ(states[0].lastResultCode, "not-in-meeting");
  EXPECT_EQ(states[1].status, "failed");
  EXPECT_EQ(states[1].lastResultCode, "not-in-meeting");
#else
  EXPECT_TRUE(true);
#endif
}

TEST(ZoomMeetingSdkAdapter, DevGateReturnsEmptyRosterBeforeSdkJoin) {
#if COREVIDEO_WITH_ZOOM
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({
      "sdk-root",
      "7.0.5",
      "https://zoom.us",
      true,
      true,
      true,
      true,
      true,
      true,
  });
  ASSERT_NE(source, nullptr);

  EXPECT_TRUE(source->participants().empty());
#else
  EXPECT_TRUE(true);
#endif
}

TEST(ZoomMeetingSdkAdapter, DevGateRejectsRecordingProofBeforeMeeting) {
#if COREVIDEO_WITH_ZOOM
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({
      "sdk-root",
      "7.0.5",
      "https://zoom.us",
      true,
      true,
      true,
      true,
      true,
      true,
  });
  ASSERT_NE(source, nullptr);

  EXPECT_FALSE(source->startRecordingProof());
  const auto proof = source->recordingProof();
  EXPECT_FALSE(proof.active);
  EXPECT_EQ(proof.status, "failed");
  EXPECT_EQ(proof.lastResultCode, "not-in-meeting");
  EXPECT_FALSE(proof.warning.empty());
#else
  EXPECT_TRUE(true);
#endif
}

TEST(ZoomMeetingSdkAdapter, DevGateDoesNotEmitFramesForDeferredRawSubscriptions) {
#if COREVIDEO_WITH_ZOOM
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({
      "sdk-root",
      "7.0.5",
      "https://zoom.us",
      true,
      true,
      true,
      true,
      true,
      true,
  });
  ASSERT_NE(source, nullptr);

  source->syncSubscriptions({
      {"12345", "participant-video", "program", 1},
      {"12345", "participant-audio", "mix", 2},
      {"12345", "screen-share", "program", 3},
  });

  EXPECT_TRUE(source->pollVideoFrames().empty());
  EXPECT_TRUE(source->pollAudioFrames().empty());
#else
  EXPECT_TRUE(true);
#endif
}

TEST(MediaCoreCommand, EmitsDownscaledProgramFramePreviewInSnapshotAndEvent) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "preview-test"},
          {"routes", corevideo::rpc::Json::Array{
                         corevideo::rpc::Json::Object{{"routeId", "a"}, {"mode", "fixed"}, {"audioRole", "mix"}, {"participantId", "speaker-1"}},
                     }},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-program-output"},
          {"destinations", corevideo::rpc::Json::Array{"recording"}},
          {"isoParticipantIds", corevideo::rpc::Json::Array{}},
      },
  });

  const auto* preview = state.get("programFramePreview");
  ASSERT_NE(preview, nullptr);
  EXPECT_TRUE(preview->get("width")->asNumber() <= corevideo::modules::kProgramFramePreviewMaxWidth);
  EXPECT_TRUE(preview->get("height")->asNumber() <= corevideo::modules::kProgramFramePreviewMaxHeight);
  EXPECT_EQ(preview->getString("pixelFormat"), "bgra");
  EXPECT_FALSE(preview->getString("bgraBase64").empty());
  EXPECT_GE(preview->get("frameNumber")->asNumber(), 1);

  const auto events = mediaCore.drainProgramFramePreviewEvents();
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().getString("type"), "program-frame-preview");
  const auto* eventPreview = events.back().get("preview");
  ASSERT_NE(eventPreview, nullptr);
  EXPECT_EQ(eventPreview->getString("pixelFormat"), "bgra");
  EXPECT_FALSE(eventPreview->getString("bgraBase64").empty());
}

TEST(MediaCoreCommand, EmitsProgramSharedTextureHandleShape) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "shared-texture-test"},
          {"routes", corevideo::rpc::Json::Array{
                         corevideo::rpc::Json::Object{{"routeId", "a"}, {"mode", "fixed"}, {"audioRole", "mix"}, {"participantId", "speaker-1"}},
                     }},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-program-output"},
          {"destinations", corevideo::rpc::Json::Array{"recording"}},
          {"isoParticipantIds", corevideo::rpc::Json::Array{}},
      },
  });

#if COREVIDEO_STUB || COREVIDEO_WITH_D3D11
  const auto* snapshotTexture = state.get("programSharedTexture");
  ASSERT_NE(snapshotTexture, nullptr);
  EXPECT_FALSE(snapshotTexture->getString("sharedHandleHex").empty());
  EXPECT_GE(snapshotTexture->get("width")->asNumber(), 1);
  EXPECT_GE(snapshotTexture->get("height")->asNumber(), 1);
  EXPECT_EQ(snapshotTexture->getString("format"), "B8G8R8A8_UNORM");

  const auto events = mediaCore.drainProgramSharedTextureEvents();
  if (!events.empty()) {
    EXPECT_EQ(events.back().getString("type"), "program-shared-texture");
    const auto* texture = events.back().get("texture");
    ASSERT_NE(texture, nullptr);
    const auto handleHex = texture->getString("sharedHandleHex");
    EXPECT_FALSE(handleHex.empty());
    EXPECT_EQ(handleHex.rfind("0x", 0), 0u);
#if COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB
    EXPECT_TRUE(handleHex.size() > 4u);
#endif
    EXPECT_GE(texture->get("width")->asNumber(), 1);
    EXPECT_GE(texture->get("height")->asNumber(), 1);
    EXPECT_EQ(texture->getString("format"), "B8G8R8A8_UNORM");
  }
#else
  EXPECT_TRUE(true) << "Shared texture export requires COREVIDEO_STUB or COREVIDEO_WITH_D3D11.";
  return;
#endif
}

TEST(MediaCoreCommand, CompositesRealZoomPixelsIntoProgramPreview) {
  // Build the stub module set so the zoom source is a RealZoomCaptureSource
  // (synthetic fallback). Capture the raw pointer before moving the modules
  // into the media core so the test can ingest a real frame.
  auto modules = corevideo::modules::createStubModules();
  auto* zoom = dynamic_cast<corevideo::modules::RealZoomCaptureSource*>(modules.zoom.get());
  ASSERT_NE(zoom, nullptr);

  // A known solid BGRA color for the participant routed into the scene.
  constexpr uint8_t kBlue = 0x10;
  constexpr uint8_t kGreen = 0x9a;
  constexpr uint8_t kRed = 0xe4;
  constexpr int kWidth = 16;
  constexpr int kHeight = 16;
  std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4u);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i + 0] = kBlue;
    pixels[i + 1] = kGreen;
    pixels[i + 2] = kRed;
    pixels[i + 3] = 0xff;
  }
  zoom->ingestFrame("1234", pixels.data(), kWidth, kHeight, /*frameId=*/1, /*timestampMs=*/0);

  corevideo::core::MediaCore mediaCore(std::move(modules));
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "solo"},
          {"routes", corevideo::rpc::Json::Array{
                         corevideo::rpc::Json::Object{
                             {"routeId", "main"},
                             {"mode", "fixed"},
                             {"participantId", "1234"},
                             {"rect", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}},
                         },
                     }},
      },
  });

  const auto previews = mediaCore.drainProgramFramePreviewEvents();
  ASSERT_FALSE(previews.empty());
  const auto* preview = previews.back().get("preview");
  ASSERT_NE(preview, nullptr);
  EXPECT_EQ(preview->getString("pixelFormat"), "bgra");
  const int previewWidth = static_cast<int>(preview->get("width")->asNumber());
  const int previewHeight = static_cast<int>(preview->get("height")->asNumber());
  EXPECT_GE(previewWidth, 1);
  EXPECT_GE(previewHeight, 1);

  const auto decoded = corevideo::modules::base64Decode(preview->getString("bgraBase64"));
  EXPECT_EQ(decoded.size(), static_cast<size_t>(previewWidth) * static_cast<size_t>(previewHeight) * 4u);

  // The full-frame route should blit the participant's real pixels across the
  // preview; sample the center pixel and confirm the known color flowed
  // end-to-end through phases 1/2/4 instead of the synthetic slate color.
  const int centerX = previewWidth / 2;
  const int centerY = previewHeight / 2;
  const size_t offset = (static_cast<size_t>(centerY) * static_cast<size_t>(previewWidth) + static_cast<size_t>(centerX)) * 4u;
  ASSERT_TRUE(offset + 3 < decoded.size());
  EXPECT_EQ(decoded[offset + 0], kBlue);
  EXPECT_EQ(decoded[offset + 1], kGreen);
  EXPECT_EQ(decoded[offset + 2], kRed);
  EXPECT_EQ(decoded[offset + 3], 0xff);

  // The synthetic slate colour for this layer must NOT be what we see, proving
  // real pixels (not the bus-health slate — #535 slice 4a; this frame is
  // present, so a matched pixel would mean the placeholder painted over it)
  // were composited.
  const uint32_t syntheticColor = corevideo::compositor::kWarmingSlateRgba;
  const uint8_t syntheticBlue = static_cast<uint8_t>(syntheticColor & 0xff);
  const uint8_t syntheticGreen = static_cast<uint8_t>((syntheticColor >> 8) & 0xff);
  const uint8_t syntheticRed = static_cast<uint8_t>((syntheticColor >> 16) & 0xff);
  const bool matchesSynthetic =
      decoded[offset + 0] == syntheticBlue && decoded[offset + 1] == syntheticGreen && decoded[offset + 2] == syntheticRed;
  EXPECT_FALSE(matchesSynthetic);
}

TEST(MediaCoreCommand, KeepsSceneBackgroundAndProgramMediaRouteFrameSourcesDistinct) {
  auto modules = corevideo::modules::createStubModules();
  SolidMediaFrameSource::reset();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();

  corevideo::core::MediaCore mediaCore(std::move(modules));
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "media-background-program"},
          {"background",
           corevideo::rpc::Json::Object{
               {"mediaAssetId", "clip-intro"},
               {"mediaAssetName", "Intro Background"},
               {"mediaAssetKind", "video"},
               {"mediaAssetPath", "C:\\media\\intro.mp4"},
               {"playing", true},
           }},
          {"routes", corevideo::rpc::Json::Array{
                         corevideo::rpc::Json::Object{
                             {"routeId", "media-main"},
                             {"mode", "fixed"},
                             {"mediaAssetId", "clip-intro"},
                             {"mediaAssetName", "Intro"},
                             {"mediaAssetKind", "video"},
                             {"mediaAssetPath", "C:\\media\\intro.mp4"},
                             {"mediaPlaybackKey", "media:clip-intro"},
                             {"mediaAssetPlaying", true},
                             {"rect", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}},
                         },
                     }},
      },
  });

  // A loop source and a clip source over the same file are two sources by kind
  // (persistent-sources spec §2), so they stay distinct - two desired rows,
  // two transports, two decoder instances.
  ASSERT_TRUE(corevideo::testing::renderUntil(mediaCore, [](corevideo::core::MediaCore& core) {
    return corevideo::testing::busSourceProducing(core, "media:clip-intro") &&
           corevideo::testing::busSourceProducing(core, "background:clip-intro");
  }));
  const std::vector<std::string> expectedIds{"background:clip-intro", "media:clip-intro"};
  EXPECT_EQ(SolidMediaFrameSource::sortedSourceIds(), expectedIds);
  EXPECT_EQ(SolidMediaFrameSource::created.load(), 2);
  EXPECT_TRUE(SolidMediaFrameSource::loopFor("background:clip-intro"));
  EXPECT_FALSE(SolidMediaFrameSource::loopFor("media:clip-intro"));

  const auto previews = mediaCore.drainProgramFramePreviewEvents();
  ASSERT_FALSE(previews.empty());
  const auto* preview = previews.back().get("preview");
  ASSERT_NE(preview, nullptr);
  const auto decoded = corevideo::modules::base64Decode(preview->getString("bgraBase64"));
  EXPECT_FALSE(decoded.empty());
}

TEST(MediaCoreCommand, CompositesMediaRoutePixelsIntoProgramPreview) {
  auto modules = corevideo::modules::createStubModules();
  SolidMediaFrameSource::reset();
  const uint8_t expectedBlue = SolidMediaFrameSource::blue;
  const uint8_t expectedGreen = SolidMediaFrameSource::green;
  const uint8_t expectedRed = SolidMediaFrameSource::red;
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();

  corevideo::core::MediaCore mediaCore(std::move(modules));
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "media-program"},
          {"routes", corevideo::rpc::Json::Array{
                         corevideo::rpc::Json::Object{
                             {"routeId", "media-main"},
                             {"mode", "fixed"},
                             {"mediaAssetId", "clip-intro"},
                             {"mediaAssetName", "Intro"},
                             {"mediaAssetKind", "video"},
                             {"mediaAssetPath", "C:\\media\\intro.mp4"},
                             {"mediaPlaybackKey", "program-take:4:media:clip-intro"},
                             {"mediaAssetPlaying", true},
                             {"rect", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}},
                         },
                     }},
      },
  });

  // The playback KEY is gone (#535 slice 3b): a go-live is an in-place Resume
  // on the SAME transport entry, so what the decoder is handed is the source id
  // and its loop flag, served by exactly ONE decoder instance.
  ASSERT_TRUE(corevideo::testing::renderUntil(mediaCore, [](corevideo::core::MediaCore& core) {
    return corevideo::testing::busSourceProducing(core, "media:clip-intro");
  }));
  EXPECT_GE(SolidMediaFrameSource::pollCount.load(), 1);
  EXPECT_EQ(SolidMediaFrameSource::created.load(), 1);
  EXPECT_EQ(SolidMediaFrameSource::sourceIds(), std::vector<std::string>{"media:clip-intro"});
  EXPECT_FALSE(SolidMediaFrameSource::loopFor("media:clip-intro"));
  // A display tick sets skipCpuReadback (no outputs, no program buffer) AND the
  // base64 preview rides a ~30fps throttle, so the only event pending after the
  // pump is the COLD-START one. Discard it and pump FULL ticks until a fresh
  // preview - the one carrying the now-decoded clip - is emitted.
  (void)mediaCore.drainProgramFramePreviewEvents();
  std::vector<corevideo::rpc::Json> previews;
  ASSERT_TRUE(corevideo::testing::applyUntil(mediaCore, [&](corevideo::core::MediaCore& core) {
    for (auto& event : core.drainProgramFramePreviewEvents()) {
      if (event.get("preview") != nullptr) previews.push_back(std::move(event));
    }
    return !previews.empty();
  }));
  const auto* preview = previews.back().get("preview");
  ASSERT_NE(preview, nullptr);
  const int previewWidth = static_cast<int>(preview->get("width")->asNumber());
  const int previewHeight = static_cast<int>(preview->get("height")->asNumber());
  const auto decoded = corevideo::modules::base64Decode(preview->getString("bgraBase64"));
  EXPECT_EQ(decoded.size(), static_cast<size_t>(previewWidth) * static_cast<size_t>(previewHeight) * 4u);

  const size_t offset =
      (static_cast<size_t>(previewHeight / 2) * static_cast<size_t>(previewWidth) + static_cast<size_t>(previewWidth / 2)) * 4u;
  ASSERT_TRUE(offset + 3 < decoded.size());
  EXPECT_EQ(decoded[offset + 0], expectedBlue);
  EXPECT_EQ(decoded[offset + 1], expectedGreen);
  EXPECT_EQ(decoded[offset + 2], expectedRed);
  EXPECT_EQ(decoded[offset + 3], 0xff);

  // bus health on air (#535 slice 4a): the placeholder this real frame must
  // NOT match is the warming slate, not a per-id colour.
  const uint32_t syntheticColor = corevideo::compositor::kWarmingSlateRgba;
  const bool matchesSynthetic =
      decoded[offset + 0] == static_cast<uint8_t>(syntheticColor & 0xff) &&
      decoded[offset + 1] == static_cast<uint8_t>((syntheticColor >> 8) & 0xff) &&
      decoded[offset + 2] == static_cast<uint8_t>((syntheticColor >> 16) & 0xff);
  EXPECT_FALSE(matchesSynthetic);
}

// #535 slice 3a: a decoded media route appears on the bus as kind "media" after a
// tick (health "producing", framesIngested >= 1), Program still composites its
// pixels (the CompositesMediaRoutePixelsIntoProgramPreview assertion repeated
// here on the bus path), and a route that is removed disappears from the bus
// on the next tick.
TEST(MediaCoreCommand, MediaRouteAppearsOnTheSourceBusAndLeavesWhenUnrouted) {
  auto modules = corevideo::modules::createStubModules();
  SolidMediaFrameSource::reset();
  const uint8_t expectedBlue = SolidMediaFrameSource::blue;
  const uint8_t expectedGreen = SolidMediaFrameSource::green;
  const uint8_t expectedRed = SolidMediaFrameSource::red;
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();

  corevideo::core::MediaCore mediaCore(std::move(modules));
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "media-program"},
          {"routes", corevideo::rpc::Json::Array{
                         corevideo::rpc::Json::Object{
                             {"routeId", "media-main"},
                             {"mode", "fixed"},
                             {"mediaAssetId", "clip-intro"},
                             {"mediaAssetName", "Intro"},
                             {"mediaAssetKind", "video"},
                             {"mediaAssetPath", "C:\\media\\intro.mp4"},
                             {"mediaPlaybackKey", "program-take:4:media:clip-intro"},
                             {"mediaAssetPlaying", true},
                             {"rect", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}},
                         },
                     }},
      },
  });

  ASSERT_TRUE(corevideo::testing::renderUntil(mediaCore, [](corevideo::core::MediaCore& core) {
    return corevideo::testing::busSourceProducing(core, "media:clip-intro");
  }));
  auto state = mediaCore.sessionState();
  const auto* sources = state.get("sources");
  ASSERT_NE(sources, nullptr);
  bool found = false;
  for (const auto& s : sources->asArray()) {
    if (s.getString("sourceId") == "media:clip-intro") {
      found = true;
      EXPECT_EQ(s.getString("kind"), "media");
      EXPECT_EQ(s.getString("health"), "producing");
      EXPECT_GE(s.get("framesIngested")->asNumber(), 1.0);
    }
  }
  EXPECT_TRUE(found);

  // As above: discard the cold-start preview and pump full ticks until a fresh
  // one (past the ~30fps base64 throttle) carries the decoded clip.
  (void)mediaCore.drainProgramFramePreviewEvents();
  std::vector<corevideo::rpc::Json> previews;
  ASSERT_TRUE(corevideo::testing::applyUntil(mediaCore, [&](corevideo::core::MediaCore& core) {
    for (auto& event : core.drainProgramFramePreviewEvents()) {
      if (event.get("preview") != nullptr) previews.push_back(std::move(event));
    }
    return !previews.empty();
  }));
  const auto* preview = previews.back().get("preview");
  ASSERT_NE(preview, nullptr);
  const int previewWidth = static_cast<int>(preview->get("width")->asNumber());
  const int previewHeight = static_cast<int>(preview->get("height")->asNumber());
  const auto decoded = corevideo::modules::base64Decode(preview->getString("bgraBase64"));
  EXPECT_EQ(decoded.size(), static_cast<size_t>(previewWidth) * static_cast<size_t>(previewHeight) * 4u);

  const size_t offset =
      (static_cast<size_t>(previewHeight / 2) * static_cast<size_t>(previewWidth) + static_cast<size_t>(previewWidth / 2)) * 4u;
  ASSERT_TRUE(offset + 3 < decoded.size());
  EXPECT_EQ(decoded[offset + 0], expectedBlue);
  EXPECT_EQ(decoded[offset + 1], expectedGreen);
  EXPECT_EQ(decoded[offset + 2], expectedRed);
  EXPECT_EQ(decoded[offset + 3], 0xff);

  // Unroute: load a scene graph with no media route; the source is gone from
  // the bus on the next tick.
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "no-media"},
          {"routes", corevideo::rpc::Json::Array{}},
      },
  });

  // Release carries a short GRACE now (MediaTransports::kReleaseGraceMs) so one
  // interleaved preview-only spine tick cannot destroy a cued clip's warm
  // decoder, so the source leaves the bus a beat later rather than on the very
  // command - retired either by a later apply() or by the render tick's
  // collectExpiredReleases() sweep. It must still leave.
  ASSERT_TRUE(corevideo::testing::renderUntil(mediaCore, [](corevideo::core::MediaCore& core) {
    return corevideo::testing::busSourceRow(core.sessionState(), "media:clip-intro") == nullptr;
  }, 4000)) << "an unrouted media source never left the source bus";
}

// HEADLESS multiview validation: with the real GPU compositor wired in, a
// set-multiview-layout + one render tick must produce a non-empty multiview
// shared-texture handle and emit exactly one multiview-shared-texture event
// carrying the canvas dims + one tile per layout source. Skips when no D3D11
// device is available (e.g. the portable stub build / no GPU).
TEST(MediaCoreMultiview, ComposesGridIntoSharedTextureAndEmitsEvent) {
  auto gpuCompositor = corevideo::modules::createD3D11Compositor();
  if (!gpuCompositor) {
    std::fprintf(stderr, "[multiview-validation] skipped: no D3D11 GPU compositor in this environment.\n");
    return;
  }
  ASSERT_TRUE(gpuCompositor->rendererName() == "d3d11");

  auto modules = corevideo::modules::createStubModules();
  modules.compositor = std::move(gpuCompositor);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  corevideo::rpc::Json::Array sources;
  const std::vector<std::string> ids = {"alice", "bob", "carol", "dave"};
  for (size_t i = 0; i < ids.size(); ++i) {
    sources.emplace_back(corevideo::rpc::Json::Object{
        {"sourceId", "zoom:" + ids[i]},
        {"kind", "participant"},
        {"participantId", ids[i]},
        {"slot", static_cast<int>(i)},
        {"label", "Tile " + ids[i]},
    });
  }
  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-multiview-layout"},
      {"canvasWidth", 1920},
      {"canvasHeight", 1080},
      {"sources", sources},
  });

  // One light video-only render tick drives the second (multiview) GPU composite.
  mediaCore.renderDisplayTick();

  const auto events = mediaCore.drainMultiviewSharedTextureEvents();
  ASSERT_FALSE(events.empty()) << "expected a cold-start multiview-shared-texture event";
  const auto& event = events.back();
  EXPECT_TRUE(event.getString("type") == "multiview-shared-texture");
  EXPECT_EQ(static_cast<int>(event.getNumber("canvasWidth")), 1920);
  EXPECT_EQ(static_cast<int>(event.getNumber("canvasHeight")), 1080);

  const auto* texture = event.get("texture");
  ASSERT_NE(texture, nullptr);
  const std::string handleHex = texture->getString("sharedHandleHex");
  EXPECT_FALSE(handleHex.empty());
  const int texWidth = static_cast<int>(texture->getNumber("width"));
  const int texHeight = static_cast<int>(texture->getNumber("height"));
  EXPECT_EQ(texWidth, 1920);
  EXPECT_EQ(texHeight, 1080);

  const auto* tiles = event.get("tiles");
  ASSERT_NE(tiles, nullptr);
  ASSERT_TRUE(tiles->isArray());
  EXPECT_EQ(tiles->asArray().size(), ids.size());
  for (const auto& tile : tiles->asArray()) {
    EXPECT_TRUE(tile.getNumber("w") > 0.0);
    EXPECT_TRUE(tile.getNumber("h") > 0.0);
  }

  // Validation log for the headless report.
  std::fprintf(
      stderr,
      "[multiview-validation] handle=%s dims=%dx%d tiles=%zu\n",
      handleHex.c_str(), texWidth, texHeight, tiles->asArray().size());

  // Structural-change gating: a second render with the same layout must NOT
  // re-emit (the handle/geometry are unchanged).
  mediaCore.renderDisplayTick();
  EXPECT_TRUE(mediaCore.drainMultiviewSharedTextureEvents().empty())
      << "multiview event must only emit on structural change";

  // The cold-start snapshot must also carry the multiview shared texture.
  const auto snapshot = mediaCore.sessionState();
  const auto* snapshotMultiview = snapshot.get("multiviewSharedTexture");
  ASSERT_NE(snapshotMultiview, nullptr);
  EXPECT_FALSE(snapshotMultiview->get("texture")->getString("sharedHandleHex").empty());
}

// Regression (2026-08-08, "the multiviewer is broken"): the core must PUBLISH the
// multiviewer config it is actually running, so the shell can tell that a core it
// did not launch is sitting on the "grid" default.
//
// The bug this pins: `configure-multiviewer` is a one-shot the shell sends at app
// launch. When the CORE respawns under a live shell, nothing re-sent it, so the
// fresh core stayed at its `grid` default while the shell still believed
// `pgmPvwTop`. The PGM/PVW bus cells vanished off the top of the wall and it
// degraded to a bare source grid — with no way for the shell to notice, because
// the applied mode was not reported anywhere. Reproduced end-to-end by killing
// corevideo-native.exe under a running shell.
//
// Needs no GPU: this is command/snapshot state, not a composite.
TEST(MediaCoreMultiview, SnapshotReportsTheAppliedMultiviewerConfig) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());

  // A FRESH core — exactly what a respawn produces — reports the grid default.
  // This is the hazard: it is a perfectly valid mode, so it fails silently.
  const auto fresh = mediaCore.sessionState();
  const auto* freshMultiviewer = fresh.get("multiviewer");
  ASSERT_NE(freshMultiviewer, nullptr)
      << "sessionState must always surface the applied multiviewer config";
  EXPECT_EQ(freshMultiviewer->getString("layoutMode"), "grid")
      << "a fresh core defaults to grid — the shell must be able to SEE that";

  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "configure-multiviewer"},
      {"layoutMode", "pgmPvwTop"},
      {"tileCount", 8},
      {"showLabels", true},
      {"showTally", false},
  });

  const auto applied = mediaCore.sessionState();
  const auto* multiviewer = applied.get("multiviewer");
  ASSERT_NE(multiviewer, nullptr);
  EXPECT_EQ(multiviewer->getString("layoutMode"), "pgmPvwTop");
  EXPECT_EQ(multiviewer->get("tileCount")->asNumber(), 8);
  EXPECT_TRUE(multiviewer->get("showLabels")->asBool());
  EXPECT_FALSE(multiviewer->get("showTally")->asBool());
}

// Regression: in a pgmPvw layout with sources but NO scene cued in preview, the
// PVW cell must NOT be pinned to an arbitrary roster source (the old "v1" fallback
// drew multiviewSources_.front(), which never reflected the preview and never
// swapped on Take). The PVW tile carries the "pvw" role with an EMPTY sourceId â€”
// it renders the live preview composite (empty here) rather than a source. Skips
// without a D3D11 device (the multiview event needs the GPU composite).
TEST(MediaCoreMultiview, PgmPvwPreviewCellIsNotPinnedToARosterSourceWithoutAPreviewScene) {
  auto gpuCompositor = corevideo::modules::createD3D11Compositor();
  if (!gpuCompositor) {
    std::fprintf(stderr, "[multiview-validation] skipped: no D3D11 GPU compositor in this environment.\n");
    return;
  }

  auto modules = corevideo::modules::createStubModules();
  modules.compositor = std::move(gpuCompositor);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  // Program/Preview layout mode â€” this is the one with a dedicated PVW cell.
  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "configure-multiviewer"},
      {"layoutMode", "pgmPvwTop"},
      {"tileCount", 4},
  });

  corevideo::rpc::Json::Array sources;
  const std::vector<std::string> ids = {"alice", "bob", "carol"};
  for (size_t i = 0; i < ids.size(); ++i) {
    sources.emplace_back(corevideo::rpc::Json::Object{
        {"sourceId", "zoom:" + ids[i]},
        {"kind", "participant"},
        {"participantId", ids[i]},
        {"slot", static_cast<int>(i)},
        {"label", "Tile " + ids[i]},
    });
  }
  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-multiview-layout"},
      {"canvasWidth", 1920},
      {"canvasHeight", 1080},
      {"sources", sources},
  });
  // NOTE: deliberately no set-preview-scene â€” nothing is cued in preview.

  mediaCore.renderDisplayTick();

  const auto events = mediaCore.drainMultiviewSharedTextureEvents();
  ASSERT_FALSE(events.empty());
  const auto* tiles = events.back().get("tiles");
  ASSERT_NE(tiles, nullptr);
  ASSERT_TRUE(tiles->isArray());

  bool sawPvw = false;
  for (const auto& tile : tiles->asArray()) {
    if (tile.getString("role") == "pvw") {
      sawPvw = true;
      EXPECT_TRUE(tile.getString("sourceId").empty())
          << "PVW cell must not be pinned to a roster source; got '" << tile.getString("sourceId") << "'";
      EXPECT_TRUE(tile.getString("participantId").empty());
    }
  }
  EXPECT_TRUE(sawPvw) << "expected a pvw-role tile in a pgmPvw layout";
}

// #478 N4: a cued Preview (or Program) guest the shell's video budget left out has no
// wall tile to carry a label, so the shell sends the notice with the layout and the
// core puts it on the PVW / PGM cell. The overlay draws "PREVIEW · <notice>". An empty
// notice clears it. Skips without a D3D11 device (the event needs the GPU composite).
TEST(MediaCoreMultiview, TheBusCellsCarryTheShellsSubscriptionLimitNotice) {
  auto gpuCompositor = corevideo::modules::createD3D11Compositor();
  if (!gpuCompositor) {
    std::fprintf(stderr, "[multiview-validation] skipped: no D3D11 GPU compositor in this environment.\n");
    return;
  }
  auto modules = corevideo::modules::createStubModules();
  modules.compositor = std::move(gpuCompositor);
  corevideo::core::MediaCore mediaCore(std::move(modules));
  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "configure-multiviewer"},
      {"layoutMode", "pgmPvwTop"},
      {"tileCount", 4},
  });

  const auto layout = [](const std::string& previewNotice) {
    return corevideo::rpc::Json::Object{
        {"type", "set-multiview-layout"},
        {"canvasWidth", 1920},
        {"canvasHeight", 1080},
        {"programNotice", ""},
        {"previewNotice", previewNotice},
        {"sources", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
            {"sourceId", "zoom:alice"}, {"kind", "zoom"}, {"participantId", "alice"},
            {"slot", 0}, {"label", "Alice"}}}},
    };
  };
  // One drain per layout change: the tiles event is emitted on STRUCTURAL change only.
  const auto busLabels = [&]() {
    std::map<std::string, std::string> labels;
    mediaCore.renderDisplayTick();
    const auto events = mediaCore.drainMultiviewSharedTextureEvents();
    EXPECT_FALSE(events.empty());
    if (events.empty()) return labels;
    const auto* tiles = events.back().get("tiles");
    if (!tiles || !tiles->isArray()) return labels;
    for (const auto& tile : tiles->asArray()) {
      labels[tile.getString("role")] = tile.getString("label");
    }
    return labels;
  };

  (void)mediaCore.applyCommand(layout("no video: Cued guest (subscription limit 10)"));
  auto labels = busLabels();
  EXPECT_EQ(labels["pvw"], "Preview Â· no video: Cued guest (subscription limit 10)");
  EXPECT_EQ(labels["pgm"], "Program");

  (void)mediaCore.applyCommand(layout(""));
  labels = busLabels();
  EXPECT_EQ(labels["pvw"], "Preview");
}

TEST(MediaCoreCommand, PreviewSceneSyncBuildsMultiLayerCompositePlan) {
  corevideo::core::MediaCore mediaCore;

  // A multi-layer preview scene (two routes + a caption-free overlay) must build a
  // preview render plan with a layer per route/overlay and flag the dedicated composite.
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-preview-scene"},
          {"sceneId", "preview-multi"},
          {"routes", corevideo::rpc::Json::Array{
                         corevideo::rpc::Json::Object{{"routeId", "pa"}, {"mode", "fixed"}, {"participantId", "spk-1"}},
                         corevideo::rpc::Json::Object{{"routeId", "pb"}, {"mode", "fixed"}, {"participantId", "spk-2"}},
                     }},
          {"overlays", corevideo::rpc::Json::Array{
                           corevideo::rpc::Json::Object{{"overlayId", "lt"}, {"text", "Preview LT"}, {"position", "lower-third"}},
                       }},
          {"colorGrade", corevideo::rpc::Json::Object{{"exposure", 0.2}}},
      },
  });

  const auto* preview = state.get("previewScene");
  ASSERT_NE(preview, nullptr) << "sessionState must surface previewScene telemetry once set";
  EXPECT_EQ(preview->getString("sceneId"), "preview-multi");
  EXPECT_EQ(preview->get("routeCount")->asNumber(), 2);
  EXPECT_EQ(preview->get("overlayCount")->asNumber(), 1);
  // 2 routes + 1 overlay == 3 composited layers.
  EXPECT_EQ(preview->get("layerCount")->asNumber(), 3);
  // Multi-layer preview runs the dedicated third composite (not the single-source path).
  EXPECT_TRUE(preview->get("composite")->asBool());
}

TEST(MediaCoreCommand, PublishesAlwaysOnRealtimeWorkerEvidenceWithoutVerboseLogging) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  mediaCore.reportRenderDeadlineMisses(5);  // legacy aggregate also includes skipped slots
  mediaCore.reportRenderWorkerStarted();
  mediaCore.reportRenderWorkerProgress(7, 2, 3, 1'500'000, 20'000, 40'000, 5'000);
  mediaCore.reportAudioWorkerStarted();
  mediaCore.reportAudioWorkerProgress(900'000);
  mediaCore.reportAudioWorkerReanchor(510'000'000);
  mediaCore.reportVideoOutputWorkerStarted();
  mediaCore.reportVideoOutputWorkerProgress(2'000'000);

  const auto state = mediaCore.sessionState();
  const auto* evidence = state.get("realtimeEvidence");
  ASSERT_NE(evidence, nullptr);
  EXPECT_EQ(evidence->getString("metricVersion"), "realtime-worker-evidence-v1");
  const auto* render = evidence->get("render");
  ASSERT_NE(render, nullptr);
  EXPECT_TRUE(render->get("observed")->asBool());
  EXPECT_EQ(render->getNumber("completedSlots"), 7);
  EXPECT_EQ(render->getNumber("skippedSlots"), 2);
  EXPECT_EQ(render->getNumber("deadlineMisses"), 3);
  EXPECT_EQ(render->getNumber("workMaximumNs"), 40'000);
  EXPECT_FALSE(render->get("gpuCompletionVerified")->asBool());
  EXPECT_FALSE(render->get("deliveryVerified")->asBool());
  const auto* audio = evidence->get("audio");
  ASSERT_NE(audio, nullptr);
  EXPECT_EQ(audio->getNumber("completedTicks"), 1);
  EXPECT_EQ(audio->getNumber("pacerReanchors"), 1);
  EXPECT_EQ(audio->getNumber("discardedTimelineNs"), 510'000'000);
  const auto* videoOutput = evidence->get("videoOutput");
  ASSERT_NE(videoOutput, nullptr);
  EXPECT_EQ(videoOutput->getNumber("completedTicks"), 1);
  EXPECT_GE(render->getNumber("progressAgeMs"), 0);
  EXPECT_EQ(state.get("health")->getNumber("renderDeadlineMisses"), 5);
}

TEST(MediaCoreCommand, TakeTransitionTracksOneEdgeTriggeredOperationToCompletion) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"},
      {"sceneId", "outgoing"},
      {"routes", corevideo::rpc::Json::Array{
          corevideo::rpc::Json::Object{{"routeId", "old"}, {"mode", "fixed"}, {"participantId", "p1"}}
      }}
  });

  const auto active = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "begin-take-transition"}, {"operationId", "take-7"},
          {"revision", 7}, {"mode", "fade"}, {"durationMs", 300}},
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "incoming"},
          {"routes", corevideo::rpc::Json::Array{
              corevideo::rpc::Json::Object{{"routeId", "new"}, {"mode", "fixed"}, {"participantId", "p2"}}
          }}
  }});
  const auto* activeTransition = active.get("takeTransition");
  ASSERT_NE(activeTransition, nullptr);
  EXPECT_EQ(activeTransition->getString("operationId"), "take-7");
  EXPECT_EQ(activeTransition->getString("mode"), "fade");
  EXPECT_EQ(activeTransition->getString("status"), "active");
  EXPECT_EQ(active.getString("sceneId"), "incoming");

  const auto complete = mediaCore.applyCommands({}, 1000.0);
  const auto* completedTransition = complete.get("takeTransition");
  ASSERT_NE(completedTransition, nullptr);
  EXPECT_EQ(completedTransition->getString("status"), "completed");
  EXPECT_EQ(completedTransition->get("progress")->asNumber(), 1.0);
}

TEST(MediaCoreCommand, PreviewSceneSingleSourceStillCompositesAndDedups) {
  corevideo::core::MediaCore mediaCore;

  // A single-source preview scene still composites (the composite is always-on once a scene
  // is set, so a later multi->single switch never strands the consumer on a stale texture).
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-preview-scene"},
          {"sceneId", "preview-solo"},
          {"routes", corevideo::rpc::Json::Array{
                         corevideo::rpc::Json::Object{{"routeId", "solo"}, {"mode", "fixed"}, {"participantId", "spk-1"}},
                     }},
      },
  });

  const auto* preview = state.get("previewScene");
  ASSERT_NE(preview, nullptr);
  EXPECT_EQ(preview->get("routeCount")->asNumber(), 1);
  EXPECT_EQ(preview->get("layerCount")->asNumber(), 1);
  EXPECT_TRUE(preview->get("composite")->asBool());

  // Re-applying the identical preview scene must be a no-op (signature dedup), so the
  // preview telemetry stays stable rather than churning core state every sync tick.
  const auto second = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-preview-scene"},
          {"sceneId", "preview-solo"},
          {"routes", corevideo::rpc::Json::Array{
                         corevideo::rpc::Json::Object{{"routeId", "solo"}, {"mode", "fixed"}, {"participantId", "spk-1"}},
                     }},
      },
  });
  ASSERT_NE(second.get("previewScene"), nullptr);
  EXPECT_EQ(second.get("previewScene")->getString("sceneId"), "preview-solo");
}

TEST(MediaCoreCommand, PerRouteOpacityReachesTheCompositedPixels) {
  // Scenes redesign S1: per-layer opacity flows scene route -> render plan ->
  // composited output. The compositor always supported layer opacity; the
  // scene graph previously never carried it.
  const auto loadWithOpacity = [](double opacity) {
    return corevideo::rpc::Json::Array{
        corevideo::rpc::Json::Object{
            {"type", "load-scene-graph"},
            {"sceneId", "opacity-scene"},
            {"routes", corevideo::rpc::Json::Array{
                           corevideo::rpc::Json::Object{
                               {"routeId", "a"},
                               {"mode", "fixed"},
                               {"audioRole", "mix"},
                               {"participantId", "speaker-1"},
                               {"rect", corevideo::rpc::Json::Object{
                                            {"x", 0.0}, {"y", 0.0}, {"width", 1.0}, {"height", 1.0}}},
                               {"opacity", opacity},
                           },
                       }},
        },
    };
  };

  corevideo::core::MediaCore fullCore(corevideo::modules::createStubModules());
  const auto full = fullCore.applyCommands(loadWithOpacity(1.0));
  corevideo::core::MediaCore dimCore(corevideo::modules::createStubModules());
  const auto dim = dimCore.applyCommands(loadWithOpacity(0.4));

  const auto* fullFrame = full.get("programFrame");
  const auto* dimFrame = dim.get("programFrame");
  ASSERT_NE(fullFrame, nullptr);
  ASSERT_NE(dimFrame, nullptr);
  EXPECT_NE(fullFrame->get("programPixelSignature")->asNumber(),
            dimFrame->get("programPixelSignature")->asNumber());
}

namespace {

// Monitor fake that reports a fixed resolved endpoint id (the id of the device
// it "opened"), for the feedback-guard test below.
class EndpointMonitorOutput final : public corevideo::modules::IAudioMonitorOutput {
 public:
  explicit EndpointMonitorOutput(std::string endpointId) : endpointId_(std::move(endpointId)) {}
  bool start(const std::string&, int, int) override {
    active_ = true;
    return true;
  }
  void stop() override { active_ = false; }
  bool render(const float* interleaved, int frameCount, int, double) override {
    return active_ && interleaved != nullptr && frameCount > 0;
  }
  bool active() const override { return active_; }
  bool hardwareOutput() const override { return true; }
  std::string deviceName() const override { return "Endpoint Monitor"; }
  std::vector<std::string> warnings() const override { return {}; }
  std::string resolvedEndpointId() const override { return endpointId_; }

 private:
  std::string endpointId_;
  bool active_ = false;
};

}  // namespace

TEST(MediaCoreAudioMonitor, WarnsWhenMonitorPlaysIntoTheLoopbackCaptureEndpoint) {
  // Spec R6: the out-of-box config loopback-captures the default render
  // endpoint; a monitor playing into that SAME endpoint re-enters the mix.
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  modules.monitorOutput = std::make_unique<EndpointMonitorOutput>("{0.0.0.00000000}.{ABCD-1234}");
  auto* capture = new RecordingAudioCaptureSource();
  corevideo::modules::CaptureAudioSourceMetrics loopback;
  loopback.captureDeviceId = "local-machine-audio";
  loopback.sourceId = "local-machine-audio";
  loopback.audioSourceKind = "wasapi-loopback";
  loopback.streaming = true;
  loopback.endpointId = "{0.0.0.00000000}.{abcd-1234}";  // case differs on purpose
  capture->reportedMetrics.push_back(loopback);
  modules.audioCapture.reset(capture);
  corevideo::core::MediaCore mediaCore{std::move(modules)};

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-monitor"},
          {"enabled", true},
          {"deviceId", ""},
          {"deviceName", ""},
          {"volume", 0.5},
      },
  });

  const auto* audio = state.get("audioMixSession");
  ASSERT_NE(audio, nullptr);
  EXPECT_TRUE(audio->get("monitorFeedbackRisk")->asBool());
  bool foundFeedbackWarning = false;
  for (const auto& warning : audio->get("warnings")->asArray()) {
    if (warning.asString().find("feedback loop") != std::string::npos) {
      foundFeedbackWarning = true;
    }
  }
  EXPECT_TRUE(foundFeedbackWarning);
}

TEST(PluginHostScan, ParsesPluginLinesAndIgnoresNoise) {
  // VST host P1: the scan output parser is pure â€” the host exe never runs in
  // tests. Noise lines and the scan-complete trailer must be ignored.
  const std::string output =
      "some tool banner\n"
      "{\"cmd\":\"plugin\",\"id\":\"C:/VST3/TDR Nova.vst3\",\"name\":\"TDR Nova\",\"vendor\":\"Tokyo Dawn Labs\",\"probe\":\"pending\"}\n"
      "{\"cmd\":\"plugin\",\"id\":\"C:/VST3/Span.vst3\",\"name\":\"Span\",\"vendor\":\"\",\"probe\":\"pending\"}\n"
      "{\"cmd\":\"error\",\"msg\":\"one root unreadable\"}\n"
      "{\"cmd\":\"scan-complete\",\"count\":2}\n";

  const auto plugins = corevideo::core::parsePluginScanOutput(output);
  ASSERT_EQ(plugins.size(), 2u);
  EXPECT_EQ(plugins[0].name, "TDR Nova");
  EXPECT_EQ(plugins[0].vendor, "Tokyo Dawn Labs");
  EXPECT_EQ(plugins[0].probe, "pending");
  EXPECT_EQ(plugins[1].name, "Span");
  EXPECT_TRUE(plugins[1].vendor.empty());
}

TEST(PluginHostScan, SnapshotCarriesPluginHostStateInEveryShape) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{});
  const auto* mix = state.get("audioMixSession");
  ASSERT_NE(mix, nullptr);
  const auto* pluginHost = mix->get("pluginHost");
  ASSERT_NE(pluginHost, nullptr);
  EXPECT_EQ(pluginHost->getString("status"), "absent");
  EXPECT_TRUE(pluginHost->get("plugins")->asArray().empty());
}

TEST(PluginHostScan, ParsesProbeResultsAndTreatsSilenceAsCrash) {
  // P2a: a pass with vendor + class metadata (P2c: every audio class rides
  // classNames so shell bundles are selectable per class).
  const auto pass = corevideo::core::parsePluginProbeResult(
      "{\"cmd\":\"probe-result\",\"id\":\"C:/VST3/Span.vst3\",\"pass\":true,"
      "\"vendor\":\"Voxengo\",\"audioClasses\":2,\"className\":\"SPAN\","
      "\"classNames\":[\"SPAN\",\"SPAN Plus\"],\"reasons\":[]}\n");
  EXPECT_TRUE(pass.parsed);
  EXPECT_TRUE(pass.pass);
  EXPECT_EQ(pass.vendor, "Voxengo");
  EXPECT_EQ(pass.className, "SPAN");
  ASSERT_EQ(pass.classNames.size(), 2u);
  EXPECT_EQ(pass.classNames[0], "SPAN");
  EXPECT_EQ(pass.classNames[1], "SPAN Plus");
  EXPECT_TRUE(pass.reason.empty());

  // A structured failure carries its reason.
  const auto fail = corevideo::core::parsePluginProbeResult(
      "{\"cmd\":\"probe-result\",\"id\":\"x\",\"pass\":false,\"reasons\":[\"no GetPluginFactory export - not a VST3 module\"]}\n");
  EXPECT_TRUE(fail.parsed);
  EXPECT_FALSE(fail.pass);
  EXPECT_NE(fail.reason.find("GetPluginFactory"), std::string::npos);

  // NO probe-result line = the host process died loading the plugin. That is
  // the isolation verdict, not a parse error.
  const auto crashed = corevideo::core::parsePluginProbeResult("");
  EXPECT_FALSE(crashed.parsed);
  EXPECT_FALSE(crashed.pass);
  EXPECT_NE(crashed.reason.find("crashed"), std::string::npos);
}

#ifdef _WIN32
#include "modules/PluginHostClient.h"

TEST(PluginHostTransport, ExchangesBlocksWithTheRealHostAndBypassesOnDeath) {
  // VST P2b: the SHM/event transport against the REAL host executable (sits
  // next to this test binary). Proves the live path end to end: block goes
  // over, comes back processed (-6 dB test processor) inside the deadline â€”
  // and killing the host mid-show BYPASSES instead of hanging or corrupting.
  char modulePath[MAX_PATH] = {};
  ASSERT_TRUE(::GetModuleFileNameA(nullptr, modulePath, MAX_PATH) > 0);
  std::string hostPath(modulePath);
  hostPath = hostPath.substr(0, hostPath.find_last_of("\\/") + 1) + "corevideo-plugin-host.exe";
  {
    std::ifstream hostExists(hostPath, std::ios::binary);
    if (!hostExists) {
      // Stub/CI configurations may not build the host exe - the transport e2e
      // only means something where it exists (dev/full builds).
      std::fprintf(stderr, "[ SKIPPED ] corevideo-plugin-host.exe not built in this configuration\n");
      return;
    }
  }

  corevideo::modules::PluginHostClient client;
  ASSERT_TRUE(client.start(hostPath, "transport-test-1"));

  std::vector<float> pcm(1920, 0.5f);  // one 20ms stereo tick at 48k
  bool processed = false;
  for (int attempt = 0; attempt < 50 && !processed; ++attempt) {  // first block races spawn
    std::fill(pcm.begin(), pcm.end(), 0.5f);
    processed = client.exchange(pcm.data(), pcm.size(), 2, 48000, 100);
    if (!processed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  ASSERT_TRUE(processed);
  EXPECT_TRUE(std::fabs(pcm[0] - 0.5f * 0.5012f) < 1e-4);
  EXPECT_TRUE(std::fabs(pcm.back() - 0.5f * 0.5012f) < 1e-4);

  // Steady state: a tight (spec-level) deadline is met.
  std::fill(pcm.begin(), pcm.end(), 0.25f);
  ASSERT_TRUE(client.exchange(pcm.data(), pcm.size(), 2, 48000, 20));
  EXPECT_TRUE(std::fabs(pcm[0] - 0.25f * 0.5012f) < 1e-4);

  // P2c selection plumbing through the REAL serve loop: a selection whose
  // bundle cannot load must come back BYPASSED (audio untouched) with the
  // load error surfaced through the status back-channel — never faked.
  std::fill(pcm.begin(), pcm.end(), 0.25f);
  ASSERT_TRUE(client.exchange(pcm.data(), pcm.size(), 2, 48000, 200,
                              "C:/definitely/not/here.vst3", "Ghost Plugin"));
  EXPECT_TRUE(std::fabs(pcm[0] - 0.25f) < 1e-6);  // untouched = honest bypass
  EXPECT_FALSE(client.lastError().empty());
  EXPECT_EQ(client.statusCode(), corevideo::pluginhost::kHostStatusPluginFailed);

  // Dropping the selection returns to the test processor (and clean status).
  std::fill(pcm.begin(), pcm.end(), 0.25f);
  ASSERT_TRUE(client.exchange(pcm.data(), pcm.size(), 2, 48000, 200));
  EXPECT_TRUE(std::fabs(pcm[0] - 0.25f * 0.5012f) < 1e-4);
  EXPECT_TRUE(client.lastError().empty());
  EXPECT_EQ(client.statusCode(), corevideo::pluginhost::kHostStatusTestProcessor);

  // Kill the host mid-show: bypass, audio untouched, counter ticks, no hang.
  const auto missesBefore = client.deadlineMisses();
  client.terminateHostForTest();
  std::fill(pcm.begin(), pcm.end(), 0.25f);
  EXPECT_FALSE(client.exchange(pcm.data(), pcm.size(), 2, 48000, 30));
  EXPECT_TRUE(std::fabs(pcm[0] - 0.25f) < 1e-6);
  EXPECT_TRUE(client.deadlineMisses() > missesBefore);
  EXPECT_FALSE(client.ready());

  // A dead host is recoverable without restarting CoreVideo.
  ASSERT_TRUE(client.start(hostPath, "transport-test-2"));
  processed = false;
  for (int attempt = 0; attempt < 50 && !processed; ++attempt) {
    std::fill(pcm.begin(), pcm.end(), 0.5f);
    processed = client.exchange(pcm.data(), pcm.size(), 2, 48000, 100);
    if (!processed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  ASSERT_TRUE(processed);
  EXPECT_TRUE(std::fabs(pcm[0] - 0.5f * 0.5012f) < 1e-4);
  client.stop();
}
#endif

TEST(PluginHostScan, HostHandledInsertNamesMatchVstAndHostOnly) {
  using corevideo::core::isHostHandledInsertName;
  EXPECT_TRUE(isHostHandledInsertName("VST3 Bridge Slot"));
  EXPECT_TRUE(isHostHandledInsertName("vst3:TDR Nova"));
  EXPECT_TRUE(isHostHandledInsertName("Host Test Gain"));
  EXPECT_FALSE(isHostHandledInsertName("Noise Gate"));
  EXPECT_FALSE(isHostHandledInsertName("Built-in EQ"));
  EXPECT_FALSE(isHostHandledInsertName("Compressor"));
  EXPECT_FALSE(isHostHandledInsertName("Limiter"));
}

TEST(MediaCoreCommand, TransientEmptyRoutingSyncHoldsLiveRoutes) {
  // Click-hunt 2026-07-05 (rig-measured): the shell transiently syncs an
  // EMPTY routing matrix / channel list ~1x per second during row-rebuild
  // windows, which unrouted live audio for a tick - an audible click per
  // episode. The core must HOLD the last non-empty config through transient
  // blanks and adopt an empty sync only when it persists (a real clear-all).
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  corevideo::core::MediaCore mediaCore{std::move(modules)};

  const auto routedSends = corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"sourceId", "pcm-speaker"}, {"busId", "master"}, {"gainDb", 0}},
  };
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "sync-audio-routing-matrix"}, {"sends", routedSends}},
      corevideo::rpc::Json::Object{
          {"type", "sync-participant-audio-mix"},
          {"channels", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
                           {"participantId", "pcm-speaker"}, {"inputLevel", 80}}}},
      },
  });

  // One transient EMPTY sync of both: routes and channels must survive.
  auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "sync-audio-routing-matrix"},
                                   {"sends", corevideo::rpc::Json::Array{}}},
      corevideo::rpc::Json::Object{{"type", "sync-participant-audio-mix"},
                                   {"channels", corevideo::rpc::Json::Array{}}},
  });
  {
    const auto* matrix = state.get("audioRoutingMatrix");
    ASSERT_NE(matrix, nullptr);
    const auto* busTaps = matrix->get("busTaps");
    ASSERT_NE(busTaps, nullptr);
    bool masterStillRouted = false;
    for (const auto& tap : busTaps->asArray()) {
      const auto* frames = tap.get("frames");
      if (tap.getString("busId") == "master" && frames != nullptr && frames->asNumber() > 0) {
        masterStillRouted = true;
      }
    }
    EXPECT_TRUE(masterStillRouted);
  }

  // A PERSISTENT clear-all (25+ consecutive empties) is adopted.
  for (int repeat = 0; repeat < 30; ++repeat) {
    state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
        corevideo::rpc::Json::Object{{"type", "sync-audio-routing-matrix"},
                                     {"sends", corevideo::rpc::Json::Array{}}},
    });
  }
  {
    const auto* matrix = state.get("audioRoutingMatrix");
    if (matrix != nullptr) {
      const auto* busTaps = matrix->get("busTaps");
      bool anyRouted = false;
      if (busTaps != nullptr) {
        for (const auto& tap : busTaps->asArray()) {
          const auto* frames = tap.get("frames");
          if (frames != nullptr && frames->asNumber() > 0) {
            anyRouted = true;
          }
        }
      }
      EXPECT_TRUE(!anyRouted);
    }
  }
}

TEST(MediaCoreCommand, PartialSyncMissingASourceHoldsItsRoutesAndChannel) {
  // Rig-measured follow-up: PARTIAL syncs (media present, mic absent) still
  // punctured audio after the empty-sync guard. A source missing from one
  // sync keeps its sends AND channel strip until the absence persists.
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<PcmTestZoomSource>();
  corevideo::core::MediaCore mediaCore{std::move(modules)};

  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-routing-matrix"},
          {"sends",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{{"sourceId", "pcm-speaker"}, {"busId", "master"}, {"gainDb", 0}},
               corevideo::rpc::Json::Object{{"sourceId", "media-playback"}, {"busId", "master"}, {"gainDb", 0}},
           }},
      },
  });

  // Partial sync: media-playback present, pcm-speaker MISSING.
  auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "sync-audio-routing-matrix"},
          {"sends",
           corevideo::rpc::Json::Array{
               corevideo::rpc::Json::Object{{"sourceId", "media-playback"}, {"busId", "master"}, {"gainDb", 0}},
           }},
      },
  });
  {
    const auto* matrix = state.get("audioRoutingMatrix");
    ASSERT_NE(matrix, nullptr);
    const auto* busTaps = matrix->get("busTaps");
    ASSERT_NE(busTaps, nullptr);
    bool masterHasAudio = false;
    for (const auto& tap : busTaps->asArray()) {
      const auto* frames = tap.get("frames");
      if (tap.getString("busId") == "master" && frames != nullptr && frames->asNumber() > 0) {
        masterHasAudio = true;  // pcm-speaker still routed through the hold
      }
    }
    EXPECT_TRUE(masterHasAudio);
  }

  // Persistent absence (30 syncs) adopts the removal.
  for (int repeat = 0; repeat < 30; ++repeat) {
    state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
        corevideo::rpc::Json::Object{
            {"type", "sync-audio-routing-matrix"},
            {"sends",
             corevideo::rpc::Json::Array{
                 corevideo::rpc::Json::Object{{"sourceId", "media-playback"}, {"busId", "master"}, {"gainDb", 0}},
             }},
        },
    });
  }
  {
    const auto* matrix = state.get("audioRoutingMatrix");
    ASSERT_NE(matrix, nullptr);
    const auto* busTaps = matrix->get("busTaps");
    ASSERT_NE(busTaps, nullptr);
    bool masterHasAudio = false;
    for (const auto& tap : busTaps->asArray()) {
      const auto* frames = tap.get("frames");
      if (tap.getString("busId") == "master" && frames != nullptr && frames->asNumber() > 0) {
        masterHasAudio = true;
      }
    }
    EXPECT_TRUE(!masterHasAudio);  // media-playback has no PCM; pcm-speaker adopted-removed
  }
}

// PERSISTENT SOURCES (spec 2026-09-10 §2): Preview and Program address the SAME
// media source, so a Take cannot cold-start the background it cuts to.
TEST(MediaCoreCommand, ALoopingBackgroundHasOneFrameSourceIdOnBothBuses) {
  auto modules = corevideo::modules::createStubModules();
  SolidMediaFrameSource::reset();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto background = corevideo::rpc::Json::Object{
      {"mediaAssetId", "bg-loop"}, {"mediaAssetName", "Loop"}, {"mediaAssetKind", "video"},
      {"mediaAssetPath", "C:\\media\\loop.mp4"}, {"playing", true}};
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "pgm"},
                                   {"background", background}, {"routes", corevideo::rpc::Json::Array{}}},
      corevideo::rpc::Json::Object{{"type", "set-preview-scene"}, {"sceneId", "pvw"},
                                   {"background", background}, {"routes", corevideo::rpc::Json::Array{}}},
  });
  ASSERT_TRUE(corevideo::testing::renderUntil(mediaCore, [](corevideo::core::MediaCore& core) {
    return corevideo::testing::busSourceProducing(core, "background:bg-loop");
  }));

  // Program and Preview both asked for the SAME source id: one decoder, one clock.
  EXPECT_EQ(SolidMediaFrameSource::sourceIds(), std::vector<std::string>{"background:bg-loop"});
  EXPECT_EQ(SolidMediaFrameSource::created.load(), 1);
}

// #535 slice 3b: the paused clip cue no longer keeps a "preview:" namespace of
// its own. A cue and its live clip are the SAME transport entry (one id, one
// decoder, one clock) whose Cued/Live state is decided at command time, so the
// Preview poster and the Program roll can never be two decoders again.
TEST(MediaCoreCommand, APausedClipCueInPreviewSharesTheProgramSource) {
  auto modules = corevideo::modules::createStubModules();
  SolidMediaFrameSource::reset();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto clipRoute = [](bool playing) {
    return corevideo::rpc::Json::Object{
        {"routeId", "clip"}, {"mode", "fixed"}, {"mediaAssetId", "clip-1"}, {"mediaAssetName", "Clip"},
        {"mediaAssetKind", "video"}, {"mediaAssetPath", "C:\\media\\clip.mp4"},
        {"mediaPlaybackKey", "media:clip-1"}, {"mediaAssetPlaying", playing},
        {"rect", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}};
  };
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "pgm"},
                                   {"routes", corevideo::rpc::Json::Array{clipRoute(true)}}},
      corevideo::rpc::Json::Object{{"type", "set-preview-scene"}, {"sceneId", "pvw"},
                                   {"routes", corevideo::rpc::Json::Array{clipRoute(false)}}},
  });
  ASSERT_TRUE(corevideo::testing::renderUntil(mediaCore, [](corevideo::core::MediaCore& core) {
    return corevideo::testing::busSourceProducing(core, "media:clip-1");
  }));

  EXPECT_EQ(SolidMediaFrameSource::sourceIds(), std::vector<std::string>{"media:clip-1"});
  EXPECT_EQ(SolidMediaFrameSource::created.load(), 1);
}

// The shell's route "loop" flag (MediaRoutePlaybackService.IsLoopingAsset)
// must reach the media source on BOTH buses — without it a looping route asset
// plays once and freezes on its last frame. A preview re-send that only flips
// the loop flag must be applied, not deduped away by the preview signature.
TEST(MediaCoreCommand, ARouteLoopFlagReachesTheMediaSourceOnBothBuses) {
  auto modules = corevideo::modules::createStubModules();
  SolidMediaFrameSource::reset();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto loopRoute = [](const char* assetId, bool loop) {
    return corevideo::rpc::Json::Object{
        {"routeId", std::string("r-") + assetId}, {"mode", "fixed"}, {"mediaAssetId", assetId},
        {"mediaAssetName", "Loop"}, {"mediaAssetKind", "background"},
        {"mediaAssetPath", std::string("C:\\media\\") + assetId + ".mp4"},
        {"mediaPlaybackKey", std::string("media:") + assetId}, {"mediaAssetPlaying", true},
        {"mediaAssetLoop", loop},
        {"rect", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}};
  };
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "pgm"},
                                   {"routes", corevideo::rpc::Json::Array{loopRoute("bg-loop", true)}}},
      corevideo::rpc::Json::Object{{"type", "set-preview-scene"}, {"sceneId", "pvw"},
                                   {"routes", corevideo::rpc::Json::Array{loopRoute("pv-loop", false)}}},
  });
  ASSERT_TRUE(corevideo::testing::renderUntil(mediaCore, [](corevideo::core::MediaCore& core) {
    return corevideo::testing::busSourceProducing(core, "media:bg-loop") &&
           corevideo::testing::busSourceProducing(core, "media:pv-loop");
  }));
  {
    const std::vector<std::string> expectedIds{"media:bg-loop", "media:pv-loop"};
    EXPECT_EQ(SolidMediaFrameSource::sortedSourceIds(), expectedIds);
    EXPECT_TRUE(SolidMediaFrameSource::loopFor("media:bg-loop")) << "Program route lost its loop flag";
    EXPECT_FALSE(SolidMediaFrameSource::loopFor("media:pv-loop"));
  }

  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "set-preview-scene"}, {"sceneId", "pvw"},
                                   {"routes", corevideo::rpc::Json::Array{loopRoute("pv-loop", true)}}},
  });
  const bool previewLoops = corevideo::testing::renderUntil(mediaCore, [](corevideo::core::MediaCore& core) {
    // Read through the CORE: the reopened preview transport must be back on the
    // bus and producing before its loop flag is evidence of anything.
    return corevideo::testing::busSourceProducing(core, "media:pv-loop") &&
           SolidMediaFrameSource::loopFor("media:pv-loop");
  });
  EXPECT_TRUE(previewLoops) << "a loop-only change to the preview scene was not applied";
}

// --- Task 4 (#535 slice 1): the Zoom decode tap must not run with no engine ---
// Regression: with STUB modules (no ZoomEngineRuntime configured), MediaCore's
// render tick must not populate the source bus with any zoom:-keyed source (the
// decode tap never runs without a configured engine), and Program must still
// composite the synthetic slate exactly as before this slice's wiring change.
TEST(MediaCoreCommand, WithNoEngineTheZoomTapNeverPopulatesTheSourceBusAndProgramStillComposites) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());

  mediaCore.renderDisplayTick();
  mediaCore.renderDisplayTick();

  const auto state = mediaCore.sessionState();

  // The decode tap never ran (no engine), so the bus carries no zoom-kind source.
  // (Production Zoom sources are keyed by the raw participant id, e.g. "16791552",
  // not a "zoom:"-prefixed id, so a "kind" == "zoom" check is what actually guards
  // against a leaked Zoom source here.)
  const auto* sources = state.get("sources");
  ASSERT_NE(sources, nullptr);
  for (const auto& entry : sources->asArray()) {
    EXPECT_NE(entry.getString("kind"), "zoom")
        << "unexpected zoom-kind source on the bus with no engine configured";
  }

  // Program still composited the synthetic slate — unchanged regression.
  EXPECT_GT(state.getNumber("programFrameCount"), 0.0);
  const auto* programFrame = state.get("programFrame");
  ASSERT_NE(programFrame, nullptr);
}

// #535 slice 2: the stub capture set has decklink-1 connected with signal, so
// after one render tick the bus must list it as a capture source that produced.
TEST(MediaCoreCommand, StubCaptureDeviceAppearsOnTheSourceBusAfterATick) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());

  mediaCore.renderDisplayTick();
  mediaCore.renderDisplayTick();

  const auto state = mediaCore.sessionState();
  const auto* sources = state.get("sources");
  ASSERT_NE(sources, nullptr);
  ASSERT_TRUE(sources->isArray());
  bool found = false;
  for (const auto& s : sources->asArray()) {
    if (s.getString("sourceId") == "capture:decklink-1") {
      found = true;
      EXPECT_EQ(s.getString("kind"), "capture");
      EXPECT_EQ(s.getString("health"), "producing");
      EXPECT_GE(s.get("framesIngested")->asNumber(), 1.0);
    }
    EXPECT_NE(s.getString("sourceId"), "capture:aja-io-1") << "a detected-only device emits nothing and must not be on the bus";
  }
  EXPECT_TRUE(found);
}

// #535 slice 2: pins the "capture frames gather before other bus kinds" order
// invariant through an observable — with no scene routes loaded, the grid
// fallback lays out videoFrames in order and sessionState()["programFrame"]
// ["videoSources"] publishes the layers in that same order.
TEST(MediaCoreCommand, CaptureBusFramesGatherBeforeOtherBusKinds) {
  // "aaa:pattern" is chosen deliberately: it sorts BEFORE "capture:decklink-1"
  // in SourceBus's std::map (a < c), so this test only passes if MediaCore
  // actually partitions bus output by kind (capture first) rather than
  // forwarding the bus's own sourceId-sorted order. "capture:" already sorts
  // before "test:pattern" alphabetically, which let the ORIGINAL version of
  // this test (registering only "test:pattern") pass with no partition at
  // all — it exercised nothing. Keep "test:pattern" too so both non-capture
  // orderings (before and after "capture:" alphabetically) are covered.
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  mediaCore.addSourceForTest(std::make_shared<corevideo::core::TestPatternSource>("aaa:pattern"));
  mediaCore.addSourceForTest(std::make_shared<corevideo::core::TestPatternSource>("test:pattern"));

  mediaCore.renderDisplayTick();
  mediaCore.renderDisplayTick();

  const auto state = mediaCore.sessionState();
  const auto* programFrame = state.get("programFrame");
  ASSERT_NE(programFrame, nullptr);
  const auto* videoSources = programFrame->get("videoSources");
  ASSERT_NE(videoSources, nullptr);
  ASSERT_TRUE(videoSources->isArray());

  int captureIndex = -1;
  int aaaIndex = -1;
  int testPatternIndex = -1;
  const auto& arr = videoSources->asArray();
  for (size_t i = 0; i < arr.size(); ++i) {
    const std::string participantId = arr[i].getString("participantId");
    if (participantId == "capture:decklink-1") captureIndex = static_cast<int>(i);
    if (participantId == "aaa:pattern") aaaIndex = static_cast<int>(i);
    if (participantId == "test:pattern") testPatternIndex = static_cast<int>(i);
  }
  ASSERT_NE(captureIndex, -1) << "capture:decklink-1 missing from programFrame.videoSources";
  ASSERT_NE(aaaIndex, -1) << "aaa:pattern missing from programFrame.videoSources";
  ASSERT_NE(testPatternIndex, -1) << "test:pattern missing from programFrame.videoSources";
  EXPECT_LT(captureIndex, aaaIndex)
      << "capture frames must gather before other bus kinds even when the other "
         "kind's sourceId sorts alphabetically before \"capture:\"";
  EXPECT_LT(captureIndex, testPatternIndex)
      << "capture frames must gather before other bus kinds (Zoom/test-pattern)";
}

// #535 slice 4a: set-source-policy round-trips, is echoed on sources[], and
// annotates render-plan layers with the bus's own health + the stored policy.
//
// `lastRenderPlanForTest()` is deliberately cached ONLY while a PROGRAM wall
// is configured (see TilesRenderPlanTest.cpp's RecordingCompositor comment);
// this scene has plain routes and no tiles wall, so it cannot answer "what
// does this plan look like" via that seam. Mirroring TilesRenderPlanTest's
// own workaround, a thin recording ICompositor wrapper around the stub
// captures the SAME plan the real render tick built and handed the
// compositor, without adding a test-only seam to MediaCore.
TEST(MediaCoreCommand, SourcePolicyCommandIsEchoedAndAnnotatesRouteLayers) {
  class RecordingCompositor final : public corevideo::modules::ICompositor {
   public:
    explicit RecordingCompositor(std::unique_ptr<corevideo::modules::ICompositor> inner) : inner_(std::move(inner)) {}
    std::string rendererName() const override { return inner_->rendererName(); }
    corevideo::modules::ProgramFrame render(const corevideo::modules::CompositorRenderPlan& plan,
        const std::vector<corevideo::modules::VideoFrame>& frames) override {
      lastPlan = plan;
      return inner_->render(plan, frames);
    }
    corevideo::modules::ProgramFrameSharedTexture renderMultiview(const corevideo::modules::CompositorRenderPlan& plan,
        const std::vector<corevideo::modules::VideoFrame>& frames) override {
      lastMultiviewPlan = plan;
      return inner_->renderMultiview(plan, frames);
    }
    corevideo::modules::CompositorRenderPlan lastPlan;
    corevideo::modules::CompositorRenderPlan lastMultiviewPlan;
   private:
    std::unique_ptr<corevideo::modules::ICompositor> inner_;
  };
  auto modules = corevideo::modules::createStubModules();
  auto ownedCompositor = std::make_unique<RecordingCompositor>(std::move(modules.compositor));
  auto* compositor = ownedCompositor.get();
  modules.compositor = std::move(ownedCompositor);
  corevideo::core::MediaCore mediaCore(std::move(modules));
  // capture:decklink-1 is connected with signal in the stub set (CaptureIngest tests).
  // set-source-policy runs AFTER load-scene-graph in this batch, and every
  // batch below keeps that order: loadSceneGraph clears sceneValidationWarnings_,
  // so a rejection warning pushed before the scene command would be wiped
  // before this test could ever see it (see the comment at the push site in
  // MediaCore::setSourcePolicy).
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "cam"},
          {"routes", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
              {"routeId", "cam-0"}, {"mode", "capture-input"}, {"captureDeviceId", "decklink-1"},
              {"rect", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}}}}},
      corevideo::rpc::Json::Object{{"type", "set-multiview-layout"},
          {"sources", corevideo::rpc::Json::Array{
              corevideo::rpc::Json::Object{
                  {"sourceId", "capture:decklink-1"}, {"kind", "capture"}, {"captureDeviceId", "decklink-1"}},
              // #535 slice 4a fix round 2 (finding 3): a multiview entry with
              // no participantId/captureDeviceId/mediaAssetId resolves to a
              // sourceId-only feed (resolveMultiviewFeed's final else
              // branch) — the OLD guard (participantId/mediaAssetId
              // non-empty) skipped it entirely; the new guard (!hasFillColor)
              // must still annotate it via the key rule's sourceId fallback.
              corevideo::rpc::Json::Object{{"sourceId", "orphan:test-1"}}}}},
      // R2 (final review): a dropout POLICY is Zoom-only this slice — a
      // capture:<id> id's policy is REFUSED (loudly), but its displayName is
      // still adopted (the failed slate needs names for capture too).
      corevideo::rpc::Json::Object{{"type", "set-source-policy"}, {"sourceId", "capture:decklink-1"},
                                   {"dropoutPolicy", "black"}, {"displayName", "Camera 1"}}});
  mediaCore.renderDisplayTick();
  const auto& warningsAfterCaptureReject = mediaCore.sceneValidationWarningsForTest();
  EXPECT_NE(std::find(warningsAfterCaptureReject.begin(), warningsAfterCaptureReject.end(),
                       "set-source-policy: only zoom:<pid> sources take a dropout policy this slice (capture:decklink-1)"),
            warningsAfterCaptureReject.end());
  const auto& plan = compositor->lastPlan;
  const auto layer = std::find_if(plan.layers.begin(), plan.layers.end(),
      [](const auto& l) { return l.participantId == "capture:decklink-1"; });
  ASSERT_NE(layer, plan.layers.end());
  EXPECT_EQ(layer->sourceHealth, "producing");
  EXPECT_EQ(layer->dropoutPolicy, "hold");  // policy refused for a non-zoom id: stays default
  EXPECT_EQ(layer->sourceDisplayName, "Camera 1");  // name IS adopted for a non-zoom id
  // #535 slice 4a fix round 1: multiview tiles draw through the SAME
  // compositor path (renderMultiview), so a dead source must be identifiable
  // there too — the same annotation must reach the MV tile layer.
  const auto& mvPlan = compositor->lastMultiviewPlan;
  const auto mvLayer = std::find_if(mvPlan.layers.begin(), mvPlan.layers.end(),
      [](const auto& l) { return l.participantId == "capture:decklink-1"; });
  ASSERT_NE(mvLayer, mvPlan.layers.end());
  EXPECT_EQ(mvLayer->sourceHealth, "producing");
  EXPECT_EQ(mvLayer->sourceDisplayName, "Camera 1");
  // The sourceId-only tile (no participantId/mediaAssetId) is annotated too —
  // "failed" since "orphan:test-1" is on nobody's bus and in no frame. With
  // the old participantId/mediaAssetId guard this layer's sourceHealth would
  // have stayed "" (skipped entirely).
  const auto orphanMvLayer = std::find_if(mvPlan.layers.begin(), mvPlan.layers.end(),
      [](const auto& l) { return l.sourceId == "orphan:test-1"; });
  ASSERT_NE(orphanMvLayer, mvPlan.layers.end());
  EXPECT_EQ(orphanMvLayer->sourceHealth, "failed");
  const auto state = mediaCore.sessionState();
  bool echoed = false;
  for (const auto& s : state.get("sources")->asArray()) {
    if (s.getString("sourceId") == "capture:decklink-1") { echoed = true; EXPECT_EQ(s.getString("dropoutPolicy"), "hold"); EXPECT_EQ(s.getString("displayName"), "Camera 1"); }
  }
  EXPECT_TRUE(echoed);

  // #535 slice 4a fix round 2 (finding 1): PRESENT-OR-KEEP. A re-sent
  // set-source-policy carrying only displayName (no dropoutPolicy field at
  // all) must never reset the policy back to the "hold" default. Still true
  // for a non-zoom id, whose policy is refused rather than accepted, but
  // whose name keeps updating normally.
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
      {"type", "set-source-policy"}, {"sourceId", "capture:decklink-1"}, {"displayName", "Cam 1"}}});
  mediaCore.renderDisplayTick();
  const auto& keepPlan = compositor->lastPlan;
  const auto keepLayer = std::find_if(keepPlan.layers.begin(), keepPlan.layers.end(),
      [](const auto& l) { return l.participantId == "capture:decklink-1"; });
  ASSERT_NE(keepLayer, keepPlan.layers.end());
  EXPECT_EQ(keepLayer->dropoutPolicy, "hold");  // still refused/default for a non-zoom id
  EXPECT_EQ(keepLayer->sourceDisplayName, "Cam 1");  // updated: displayName was present

  // #535 slice 4a fix round 2 (finding 2): the rejection path is observable
  // (the exact warning), the rejected policy leaves the layer at the "hold"
  // default, and a VALID zoom:<pid> policy strips the "zoom:" prefix onto the
  // raw pid — the same key annotateLayerSource resolves from a Zoom layer's
  // (unprefixed) participantId.
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "zoomcam"},
          {"routes", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
              {"routeId", "zoomcam-0"}, {"mode", "fixed"}, {"participantId", "16778240"}}}}},
      corevideo::rpc::Json::Object{{"type", "set-source-policy"}, {"sourceId", "zoom:16778240"},
                                   {"dropoutPolicy", "bogus"}}});  // rejected, warns, stays default
  mediaCore.renderDisplayTick();
  const auto& warnings = mediaCore.sceneValidationWarningsForTest();
  EXPECT_NE(std::find(warnings.begin(), warnings.end(),
                       "set-source-policy: unknown dropoutPolicy 'bogus' for zoom:16778240"),
            warnings.end());
  const auto& rejectedPlan = compositor->lastPlan;
  const auto rejectedLayer = std::find_if(rejectedPlan.layers.begin(), rejectedPlan.layers.end(),
      [](const auto& l) { return l.participantId == "16778240"; });
  ASSERT_NE(rejectedLayer, rejectedPlan.layers.end());
  EXPECT_EQ(rejectedLayer->dropoutPolicy, "hold");  // default when the policy is rejected

  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
      {"type", "set-source-policy"}, {"sourceId", "zoom:16778240"},
      {"dropoutPolicy", "black"}, {"displayName", "Jamal"}}});
  mediaCore.renderDisplayTick();
  const auto& zoomPlan = compositor->lastPlan;
  const auto zoomLayer = std::find_if(zoomPlan.layers.begin(), zoomPlan.layers.end(),
      [](const auto& l) { return l.participantId == "16778240"; });
  ASSERT_NE(zoomLayer, zoomPlan.layers.end());
  EXPECT_EQ(zoomLayer->dropoutPolicy, "black");
  EXPECT_EQ(zoomLayer->sourceDisplayName, "Jamal");

  // A layer whose key is on nobody's bus and in no frame reads "failed".
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"}, {"sceneId", "gone"},
      {"routes", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
          {"routeId", "gone-0"}, {"mode", "capture-input"}, {"captureDeviceId", "no-such-device"},
          {"rect", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}}}}}});
  mediaCore.renderDisplayTick();
  const auto& plan2 = compositor->lastPlan;
  ASSERT_FALSE(plan2.layers.empty());
  EXPECT_EQ(plan2.layers.front().sourceHealth, "failed");
  EXPECT_EQ(plan2.layers.front().dropoutPolicy, "hold");  // default when unset
}

// Round 2 (final review, #535 slice 4a): a set-source-policy for a capture id
// carrying ONLY displayName (no dropoutPolicy field at all) must produce NO
// scene warning at all — the shell's wire builder now omits the dropoutPolicy
// key entirely for a name-only entry, so this must never hit the "only
// zoom:<pid>..." refusal path. Warnings must stay empty across a FOLLOWING
// load-scene-graph + tick too (load-scene-graph clears warnings, so this also
// proves nothing re-pushes one on a later tick).
TEST(MediaCoreCommand, CaptureDisplayNameOnlyPolicyProducesNoSceneWarning) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
      {"type", "set-source-policy"}, {"sourceId", "capture:x"}, {"displayName", "Camera X"}}});
  EXPECT_TRUE(mediaCore.sceneValidationWarningsForTest().empty());

  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"}, {"sceneId", "after-name-only"}, {"routes", corevideo::rpc::Json::Array{}}}});
  mediaCore.renderDisplayTick();
  EXPECT_TRUE(mediaCore.sceneValidationWarningsForTest().empty());
}

namespace {
// #535 slice 4a final review, R1: a bus source with a settable `kind` and a
// frame that NEVER advances its frameId after the first poll — used to drive
// MediaCore::annotateLayerSource's on-air stall decision (kind + real elapsed
// wall time), which is deliberately distinct from SourceBus's own 200ms
// diagnostic health.
class FixedFrameKindSource final : public corevideo::core::ISource {
 public:
  FixedFrameKindSource(std::string sourceId, std::string kind) {
    descriptor_.sourceId = sourceId_ = std::move(sourceId);
    descriptor_.kind = std::move(kind);
    descriptor_.width = descriptor_.height = 64;
    descriptor_.hasVideo = true;
  }
  const corevideo::core::SourceDescriptor& descriptor() const override { return descriptor_; }
  corevideo::core::SourceTick poll(int64_t /*programTime100ns*/) override {
    corevideo::modules::VideoFrame frame;
    frame.participantId = sourceId_;
    frame.width = frame.pixelWidth = frame.naturalWidth = 64;
    frame.height = frame.pixelHeight = frame.naturalHeight = 64;
    frame.pixelStride = 64 * 4;
    frame.pixels = std::make_shared<const std::vector<uint8_t>>(64 * 64 * 4, 0x80);
    frame.frameId = 1;  // fixed: never advances, so only the FIRST poll counts as new.
    corevideo::core::SourceTick tick;
    tick.video.push_back(std::move(frame));
    tick.health = corevideo::core::SourceHealth::Producing;
    return tick;
  }
  corevideo::core::SourceIngestCounters counters() const override { return counters_; }

 private:
  std::string sourceId_;
  corevideo::core::SourceDescriptor descriptor_;
  corevideo::core::SourceIngestCounters counters_;
};
}  // namespace

namespace {
// Reused by the three R1 tests below: mirrors SourcePolicyCommandIsEchoedAndAnnotatesRouteLayers's
// local RecordingCompositor (a real render-plan capture seam, since
// annotateLayerSource's output never reaches RenderedProgramSources's wire —
// that node carries only layerId/sourceId/participantId/kind, no health).
class OnAirRecordingCompositor final : public corevideo::modules::ICompositor {
 public:
  explicit OnAirRecordingCompositor(std::unique_ptr<corevideo::modules::ICompositor> inner)
      : inner_(std::move(inner)) {}
  std::string rendererName() const override { return inner_->rendererName(); }
  corevideo::modules::ProgramFrame render(const corevideo::modules::CompositorRenderPlan& plan,
      const std::vector<corevideo::modules::VideoFrame>& frames) override {
    lastPlan = plan;
    return inner_->render(plan, frames);
  }
  corevideo::modules::ProgramFrameSharedTexture renderMultiview(const corevideo::modules::CompositorRenderPlan& plan,
      const std::vector<corevideo::modules::VideoFrame>& frames) override {
    lastMultiviewPlan = plan;
    return inner_->renderMultiview(plan, frames);
  }
  corevideo::modules::CompositorRenderPlan lastPlan;
  corevideo::modules::CompositorRenderPlan lastMultiviewPlan;
 private:
  std::unique_ptr<corevideo::modules::ICompositor> inner_;
};

std::string onAirHealthFor(corevideo::core::MediaCore& mediaCore, OnAirRecordingCompositor* compositor,
                           const std::string& participantId) {
  mediaCore.renderDisplayTick();
  const auto& plan = compositor->lastPlan;
  const auto layer = std::find_if(plan.layers.begin(), plan.layers.end(),
      [&](const auto& l) { return l.participantId == participantId; });
  return layer == plan.layers.end() ? std::string() : layer->sourceHealth;
}
}  // namespace

// #535 slice 4a final review, R1: a "zoom"-kind bus source with no new frame
// for >= 1.5s reads "stalled" on the render-plan layer (the on-air rule),
// even though SourceBus's own 200ms diagnostic health would already call it
// stalled well before that.
TEST(MediaCoreCommand, AZoomKindSourceStalledOver1500msReadsStalledOnAir) {
  auto modules = corevideo::modules::createStubModules();
  auto ownedCompositor = std::make_unique<OnAirRecordingCompositor>(std::move(modules.compositor));
  auto* compositor = ownedCompositor.get();
  modules.compositor = std::move(ownedCompositor);
  corevideo::core::MediaCore mediaCore(std::move(modules));
  mediaCore.addSourceForTest(std::make_shared<FixedFrameKindSource>("zoomstall1", "zoom"));

  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"}, {"sceneId", "zoomstall"},
      {"routes", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
          {"routeId", "zoomstall-0"}, {"mode", "fixed"}, {"participantId", "zoomstall1"}}}}}});
  onAirHealthFor(mediaCore, compositor, "zoomstall1");  // first frame ingested; lastNewFrameNs set here.

  std::this_thread::sleep_for(std::chrono::milliseconds(1600));

  EXPECT_EQ(onAirHealthFor(mediaCore, compositor, "zoomstall1"), "stalled");
}

// Same shape, but well under the 1.5s on-air window: reads "producing".
TEST(MediaCoreCommand, AZoomKindSourceAt500msStillReadsProducingOnAir) {
  auto modules = corevideo::modules::createStubModules();
  auto ownedCompositor = std::make_unique<OnAirRecordingCompositor>(std::move(modules.compositor));
  auto* compositor = ownedCompositor.get();
  modules.compositor = std::move(ownedCompositor);
  corevideo::core::MediaCore mediaCore(std::move(modules));
  mediaCore.addSourceForTest(std::make_shared<FixedFrameKindSource>("zoomfresh1", "zoom"));

  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"}, {"sceneId", "zoomfresh"},
      {"routes", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
          {"routeId", "zoomfresh-0"}, {"mode", "fixed"}, {"participantId", "zoomfresh1"}}}}}});
  onAirHealthFor(mediaCore, compositor, "zoomfresh1");

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  EXPECT_EQ(onAirHealthFor(mediaCore, compositor, "zoomfresh1"), "producing");
}

// A non-Zoom bus source NEVER reads "stalled" from frame cadence alone, no
// matter how long it goes without a new frameId — browser sources, WGC
// screen capture, stills and paused clips legitimately re-serve the same
// frameId forever while healthy. Uses kind "test" rather than the spec's
// illustrative "capture": a bus source ADDED DIRECTLY via addSourceForTest
// with kind "capture" is purged every tick by CaptureBusRoster::syncCaptureSources
// (it mirrors the REAL capture adapters' poll and removes any "capture"-kind
// entry absent from it — by design, #554/slice-2's capture parity rule,
// unrelated to this test), so it can never stay on the bus long enough to
// prove anything here. Kind "test" is untouched by any such roster sync and
// exercises the exact same "kind != zoom" gate in annotateLayerSource. This
// also waits past the SAME 1.6s window the Zoom test above uses (proving the
// KIND gate, not a duration difference) rather than a literal 10s, since the
// property under test does not depend on how far past kOnAirStallNs the wait
// goes, and a real 10s sleep would needlessly slow the suite for the same
// coverage.
TEST(MediaCoreCommand, ANonZoomKindSourceNeverReadsStalledOnAirFromCadenceAlone) {
  auto modules = corevideo::modules::createStubModules();
  auto ownedCompositor = std::make_unique<OnAirRecordingCompositor>(std::move(modules.compositor));
  auto* compositor = ownedCompositor.get();
  modules.compositor = std::move(ownedCompositor);
  corevideo::core::MediaCore mediaCore(std::move(modules));
  mediaCore.addSourceForTest(std::make_shared<FixedFrameKindSource>("capturestall1", "test"));

  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"}, {"sceneId", "capturestall"},
      {"routes", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
          {"routeId", "capturestall-0"}, {"mode", "fixed"}, {"participantId", "capturestall1"}}}}}});
  onAirHealthFor(mediaCore, compositor, "capturestall1");

  std::this_thread::sleep_for(std::chrono::milliseconds(1600));

  EXPECT_EQ(onAirHealthFor(mediaCore, compositor, "capturestall1"), "producing");
}

// #535 slice 4a final review, R3: black is a PROGRAM-only cut. With a
// Zoom-kind source stalled >= 1.5s and policy "black", the PROGRAM plan
// layer reads policy "black" while the MULTIVIEW plan layer for the SAME
// source reads "hold" — the operator can still watch the multiview tile for
// recovery even though Program has cut to black.
TEST(MediaCoreCommand, BlackPolicyAppliesToProgramOnlyNeverMultiview) {
  auto modules = corevideo::modules::createStubModules();
  auto ownedCompositor = std::make_unique<OnAirRecordingCompositor>(std::move(modules.compositor));
  auto* compositor = ownedCompositor.get();
  modules.compositor = std::move(ownedCompositor);
  corevideo::core::MediaCore mediaCore(std::move(modules));
  mediaCore.addSourceForTest(std::make_shared<FixedFrameKindSource>("zoomblack1", "zoom"));

  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "zoomblack"},
          {"routes", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
              {"routeId", "zoomblack-0"}, {"mode", "fixed"}, {"participantId", "zoomblack1"}}}}},
      corevideo::rpc::Json::Object{{"type", "set-multiview-layout"},
          {"sources", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
              {"sourceId", "zoom:zoomblack1"}, {"kind", "zoom"}, {"participantId", "zoomblack1"}}}}},
      corevideo::rpc::Json::Object{{"type", "set-source-policy"}, {"sourceId", "zoom:zoomblack1"},
                                   {"dropoutPolicy", "black"}}});
  mediaCore.renderDisplayTick();  // first frame ingested.

  std::this_thread::sleep_for(std::chrono::milliseconds(1600));
  mediaCore.renderDisplayTick();

  const auto& programPlan = compositor->lastPlan;
  const auto programLayer = std::find_if(programPlan.layers.begin(), programPlan.layers.end(),
      [](const auto& l) { return l.participantId == "zoomblack1"; });
  ASSERT_NE(programLayer, programPlan.layers.end());
  EXPECT_EQ(programLayer->sourceHealth, "stalled");
  EXPECT_EQ(programLayer->dropoutPolicy, "black");

  const auto& mvPlan = compositor->lastMultiviewPlan;
  const auto mvLayer = std::find_if(mvPlan.layers.begin(), mvPlan.layers.end(),
      [](const auto& l) { return l.participantId == "zoomblack1"; });
  ASSERT_NE(mvLayer, mvPlan.layers.end());
  EXPECT_EQ(mvLayer->sourceHealth, "stalled");
  EXPECT_EQ(mvLayer->dropoutPolicy, "hold");  // monitoring surface: never black
}

// ---------------------------------------------------------------------------
// #535 slice 3b Task 4: the wire, the snapshot and the plan fields.
//
// The per-route `mediaPlaybackKey`/`mediaAssetPlaying` and the background
// `playing` flag are DEAD on the wire - MediaTransports decides play state at
// command time now. These four tests pin the replacement: a `mediaSources[]`
// snapshot node fed from the transports, a one-shot `set-media-transport`
// operator command, a `mediaPlayback` node whose status comes from the
// selected asset's transport, and the rule that an older shell still SENDING
// the retired fields is silently ignored, never refused.
// ---------------------------------------------------------------------------

// ONE fixed route on asset "clip" - no playback key, no playing flag.
static corevideo::rpc::Json slice3bClipScene(const char* sceneId, const char* type) {
  return corevideo::rpc::Json::Object{
      {"type", type},
      {"sceneId", sceneId},
      {"routes", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
                     {"routeId", "clip-route"},
                     {"mode", "fixed"},
                     {"mediaAssetId", "clip"},
                     {"mediaAssetName", "Clip"},
                     {"mediaAssetKind", "video"},
                     {"mediaAssetPath", "C:\\media\\clip.mp4"},
                     {"rect", corevideo::rpc::Json::Object{
                                  {"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}}}}};
}

static corevideo::rpc::Json slice3bEmptyScene(const char* sceneId, const char* type) {
  return corevideo::rpc::Json::Object{
      {"type", type}, {"sceneId", sceneId}, {"routes", corevideo::rpc::Json::Array{}}};
}

// The row for `sourceId` in the snapshot's mediaSources[] node, or nullptr.
static const corevideo::rpc::Json* slice3bMediaSource(const corevideo::rpc::Json& state,
                                                      const std::string& sourceId) {
  const auto* rows = state.get("mediaSources");
  if (rows == nullptr) return nullptr;
  for (const auto& row : rows->asArray()) {
    if (row.getString("sourceId") == sourceId) return &row;
  }
  return nullptr;
}

TEST(MediaCoreCommand, MediaSourcesNodeIsPublishedEmptyAndThenPerSource) {
  auto modules = corevideo::modules::createStubModules();
  SolidMediaFrameSource::reset();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
  corevideo::core::MediaCore core(std::move(modules));

  // The multiviewer-node rule: present and EMPTY before anything is routed.
  auto state = core.sessionState();
  ASSERT_NE(state.get("mediaSources"), nullptr);
  EXPECT_TRUE(state.get("mediaSources")->asArray().empty());

  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bEmptyScene("a", "load-scene-graph"), slice3bClipScene("b", "set-preview-scene")});
  state = core.sessionState();
  ASSERT_EQ(state.get("mediaSources")->asArray().size(), 1u);
  const auto* cued = slice3bMediaSource(state, "media:clip");
  ASSERT_NE(cued, nullptr);
  EXPECT_EQ(cued->getString("mediaAssetId"), "clip");
  EXPECT_EQ(cued->getString("state"), "cued");
  EXPECT_FALSE(cued->get("onProgram")->asBool());
  EXPECT_TRUE(cued->get("onPreview")->asBool());

  // The same asset taken to Program: the SAME source id, now live.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bClipScene("b", "load-scene-graph"), slice3bEmptyScene("a", "set-preview-scene")});
  state = core.sessionState();
  const auto* live = slice3bMediaSource(state, "media:clip");
  ASSERT_NE(live, nullptr);
  EXPECT_EQ(live->getString("state"), "live");
  EXPECT_TRUE(live->get("onProgram")->asBool());
}

TEST(MediaCoreCommand, SetMediaTransportPausesAndPlaysTheProgramClipOnly) {
  auto modules = corevideo::modules::createStubModules();
  SolidMediaFrameSource::reset();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
  corevideo::core::MediaCore core(std::move(modules));

  (void)core.applyCommands(corevideo::rpc::Json::Array{slice3bClipScene("b", "load-scene-graph")});

  auto state = core.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-media-transport"}, {"mediaAssetId", "clip"}, {"action", "pause"}});
  const auto* paused = slice3bMediaSource(state, "media:clip");
  ASSERT_NE(paused, nullptr);
  EXPECT_EQ(paused->getString("state"), "paused");

  state = core.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-media-transport"}, {"mediaAssetId", "clip"}, {"action", "play"}});
  const auto* playing = slice3bMediaSource(state, "media:clip");
  ASSERT_NE(playing, nullptr);
  EXPECT_EQ(playing->getString("state"), "live");

  // Refused: the clip is only CUED in Preview, and pause/play is a Program
  // gesture. The refusal is loud and leaves the transport exactly as it was.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bEmptyScene("a", "load-scene-graph"), slice3bClipScene("b", "set-preview-scene")});
  state = core.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-media-transport"}, {"mediaAssetId", "clip"}, {"action", "pause"}});
  const auto* refused = slice3bMediaSource(state, "media:clip");
  ASSERT_NE(refused, nullptr);
  EXPECT_EQ(refused->getString("state"), "cued");
  const auto& warnings = core.sceneValidationWarningsForTest();
  EXPECT_TRUE(std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) {
    return w.find("Program") != std::string::npos;
  })) << "a refused transport must say why";
}

TEST(MediaCoreCommand, TheSelectedAssetsPlaybackNodeReportsTheTransportState) {
  // set-media-playback is SELECTION ONLY now; playing/status come from the
  // transport, and the `playing` field on the wire is ignored outright.
  auto modules = corevideo::modules::createStubModules();
  SolidMediaFrameSource::reset();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
  corevideo::core::MediaCore core(std::move(modules));

  auto state = core.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-media-playback"},
      {"mediaAssetId", "clip"},
      {"mediaAssetName", "Clip"},
      {"mediaAssetPath", "C:\\media\\clip.mp4"},
      {"playing", true}});  // `playing` ignored
  ASSERT_NE(state.get("mediaPlayback"), nullptr);
  // Selected, but no scene names it: it is on no bus, so there is no transport
  // state to report and the node says so rather than guessing "paused".
  EXPECT_EQ(state.get("mediaPlayback")->getString("status"), "unavailable");
  EXPECT_FALSE(state.get("mediaPlayback")->get("playing")->asBool());

  (void)core.applyCommands(corevideo::rpc::Json::Array{slice3bClipScene("b", "load-scene-graph")});
  state = core.sessionState();
  EXPECT_EQ(state.get("mediaPlayback")->getString("status"), "live");
  EXPECT_TRUE(state.get("mediaPlayback")->get("playing")->asBool());
  EXPECT_EQ(state.get("mediaPlayback")->getString("mediaPlaybackKey"), "");
  EXPECT_EQ(state.get("mediaPlayback")->getString("summary"), "Clip live.");
}

TEST(MediaCoreCommand, LegacyPlaybackFieldsOnTheWireAreIgnoredNotRefused) {
  // Old shells and old cores overlap during rollout: a route still carrying
  // mediaPlaybackKey/mediaAssetPlaying must be read exactly as one that does
  // not - silently, with no warning and no effect on the transport.
  auto modules = corevideo::modules::createStubModules();
  SolidMediaFrameSource::reset();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
  corevideo::core::MediaCore core(std::move(modules));

  const auto state = core.applyCommands(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"},
      {"sceneId", "b"},
      {"routes", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
                     {"routeId", "clip-route"},
                     {"mode", "fixed"},
                     {"mediaAssetId", "clip"},
                     {"mediaAssetName", "Clip"},
                     {"mediaAssetKind", "video"},
                     {"mediaAssetPath", "C:\\media\\clip.mp4"},
                     {"mediaPlaybackKey", "media:clip:live:7"},
                     {"mediaAssetPlaying", false},
                     {"rect", corevideo::rpc::Json::Object{
                                  {"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}}}}}});

  const auto* row = slice3bMediaSource(state, "media:clip");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->getString("state"), "live") << "mediaAssetPlaying:false must not cue a Program clip";
  EXPECT_TRUE(core.sceneValidationWarningsForTest().empty());

  // Same rule on the one-shot command: the retired fields are ignored, and the
  // selection still lands.
  const auto after = core.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-media-playback"},
      {"mediaAssetId", "clip"},
      {"mediaAssetName", "Clip"},
      {"mediaAssetPath", "C:\\media\\clip.mp4"},
      {"mediaPlaybackKey", "media:clip:live:7"},
      {"playing", false}});
  EXPECT_EQ(after.get("mediaPlayback")->getString("status"), "live");
  EXPECT_TRUE(after.get("mediaPlayback")->get("warnings")->asArray().empty());
}

// ---------------------------------------------------------------------------
// #535 slice 3b Task 5: a behaviour test per operator rule in
// docs/superpowers/specs/2026-09-20-source-bus-slice3b-media-source-state-design.md
// section 2, plus the idempotent re-send.
//
// Every one of these drives the REAL core::MediaTransports through MediaCore's
// command surface - no policy-level stand-in - and every wait is a bounded
// predicate loop with a deadline and a failing assertion on timeout. The one
// deliberate exception is a fixed pump used to observe a NON-event ("the
// paused clip does not advance"), where a predicate wait is impossible and
// asserting on tick zero would pass vacuously.
//
// WHAT "THE FRAME ID ON PROGRAM" IS READ FROM, and why there are two answers:
//   * `sources[]`.framesIngested - the bus's DEDUPED per-frame count. It only
//     moves when the frameId selectVideo returned actually changed, so it is
//     the honest on-the-wire answer to "is the picture advancing on air?". It
//     cannot say WHICH id, and it cannot see a decoder that restarted and
//     kept producing.
//   * testing::CountingDecoder's own static book - the ids themselves, plus a
//     regression counter that names a NEW decoder instance. `lastFrameId` is
//     not on the wire and this task deliberately does not widen it.
// ---------------------------------------------------------------------------

namespace {

using corevideo::testing::CountingDecoder;

// A scene whose BACKGROUND is the asset "bg". A background always loops (see
// MediaCore::syncMediaTransportsDesired), so it is live on either bus and the
// `playing` flag on the wire is dead.
corevideo::rpc::Json slice3bBackgroundScene(const char* sceneId, const char* type) {
  return corevideo::rpc::Json::Object{
      {"type", type},
      {"sceneId", sceneId},
      {"background", corevideo::rpc::Json::Object{
                         {"mediaAssetId", "bg"},
                         {"mediaAssetName", "bg"},
                         {"mediaAssetKind", "video"},
                         {"mediaAssetPath", "C:\\media\\bg.mp4"},
                         {"playing", true}}},
      {"routes", corevideo::rpc::Json::Array{}}};
}

// Pumps display ticks (the production render cadence for media) until `done`,
// with a deadline. Bounded, and every caller ASSERTs on the return.
bool slice3bPumpUntil(corevideo::core::MediaCore& core, const std::function<bool()>& done,
                      int timeoutMs = 3000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    core.renderDisplayTick();
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  core.renderDisplayTick();
  return done();
}

// Pumps display ticks for a fixed span. ONLY for observing a non-event.
void slice3bPumpFor(corevideo::core::MediaCore& core, int ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    core.renderDisplayTick();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

std::int64_t slice3bSteadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Drains the transports' audio directly (the seam the audio worker uses), with
// the core's own audio worker latched ON so no full tick competes for the
// windows. True once a NON-SILENT window for `sourceId` arrives.
bool slice3bWaitForAudio(corevideo::core::MediaCore& core, const std::string& sourceId,
                         int timeoutMs = 2000) {
  auto* transports = core.mediaTransportsForTest();
  if (transports == nullptr) return false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    core.renderDisplayTick();
    for (const auto& frame : transports->popAudio(slice3bSteadyNowMs())) {
      if (frame.participantId != sourceId) continue;
      for (const auto sample : frame.pcm) {
        if (sample != 0.f) return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

// Every audio window `sourceId` produced over a fixed span. For the NON-event
// "a paused clip emits nothing, not even silence".
std::size_t slice3bAudioWindowsOver(corevideo::core::MediaCore& core, const std::string& sourceId,
                                    int ms) {
  auto* transports = core.mediaTransportsForTest();
  if (transports == nullptr) return 0;
  std::size_t windows = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    core.renderDisplayTick();
    for (const auto& frame : transports->popAudio(slice3bSteadyNowMs())) {
      if (frame.participantId == sourceId) ++windows;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return windows;
}

// Mean luma of the program thumbnail the stub compositor fills every tick.
// CountingDecoder paints 0x40 on every colour channel, so a full-canvas clip
// reads ~64 - far from either bus-health slate (~28-31), which is what a lost
// picture would draw instead.
double slice3bProgramLuma(corevideo::core::MediaCore& core) {
  const auto& px = core.lastProgramFrameForTest().preview.bgra;
  if (px.empty()) return -1.0;
  double sum = 0;
  std::size_t n = 0;
  for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
    sum += 0.114 * px[i] + 0.587 * px[i + 1] + 0.299 * px[i + 2];
    ++n;
  }
  return n ? sum / n : -1.0;
}

std::string slice3bTransportState(corevideo::core::MediaCore& core, const std::string& sourceId) {
  const auto state = core.sessionState();
  const auto* row = slice3bMediaSource(state, sourceId);
  return row == nullptr ? std::string() : row->getString("state");
}

}  // namespace

// RULE 1: a clip CUED in Preview and then TAKEN rolls from zero, on the
// decoder that was already warm - one decoder, audio on at the take.
TEST(MediaCoreCommand, ACuedClipTakenToProgramRollsFromZeroWithOneDecoder) {
  auto modules = corevideo::modules::createStubModules();
  CountingDecoder::resetAll();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<CountingDecoder>();
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();  // nothing but this test drains popAudio

  // Cue: Program is empty, Preview carries the clip.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bEmptyScene("a", "load-scene-graph"), slice3bClipScene("b", "set-preview-scene")});
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return corevideo::testing::busSourceProducing(core, "media:clip");
  })) << "the cued clip never reached a poster";
  EXPECT_EQ(slice3bTransportState(core, "media:clip"), "cued");
  EXPECT_EQ(CountingDecoder::created.load(), 1);

  const auto beforeTake = corevideo::testing::busFramesIngested(core, "media:clip");
  ASSERT_GT(beforeTake, 0);

  // The Take.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bClipScene("b", "load-scene-graph"), slice3bEmptyScene("a", "set-preview-scene")});
  EXPECT_EQ(slice3bTransportState(core, "media:clip"), "live");

  // The picture ADVANCES on Program (a poster that stayed frozen would not
  // move the bus's deduped count), from the SAME decoder and with no id
  // regression - i.e. it resumed rather than cold-starting on air.
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return corevideo::testing::busFramesIngested(core, "media:clip") > beforeTake + 2;
  })) << "the taken clip never rolled";
  EXPECT_EQ(CountingDecoder::created.load(), 1)
      << "the take opened a second decoder instead of resuming the warm one";
  EXPECT_EQ(CountingDecoder::restartsFor("media:clip"), 0)
      << "the frame ids regressed, so the clip cold-started on air";
  EXPECT_TRUE(slice3bWaitForAudio(core, "media:clip")) << "the taken clip never turned its audio on";
}

// RULE 2: the same clip on BOTH buses is ONE rolling picture. Program and
// Preview address the identical source id - there is no `preview:` namespace
// any more - so the preview composite is handed the very frame Program drew.
TEST(MediaCoreCommand, TheSameClipOnBothBusesShowsOneRollingPictureOnPreview) {
  class PlanRecordingCompositor final : public corevideo::modules::ICompositor {
   public:
    explicit PlanRecordingCompositor(std::unique_ptr<corevideo::modules::ICompositor> inner)
        : inner_(std::move(inner)) {}
    std::string rendererName() const override { return inner_->rendererName(); }
    corevideo::modules::ProgramFrame render(
        const corevideo::modules::CompositorRenderPlan& plan,
        const std::vector<corevideo::modules::VideoFrame>& frames) override {
      lastProgramPlan = plan;
      lastProgramFrames = frames;
      return inner_->render(plan, frames);
    }
    corevideo::modules::ProgramFrameSharedTexture renderPreview(
        const corevideo::modules::CompositorRenderPlan& plan,
        const std::vector<corevideo::modules::VideoFrame>& frames) override {
      lastPreviewPlan = plan;
      lastPreviewFrames = frames;
      ++previewRenders;
      return inner_->renderPreview(plan, frames);
    }
    corevideo::modules::CompositorRenderPlan lastProgramPlan, lastPreviewPlan;
    std::vector<corevideo::modules::VideoFrame> lastProgramFrames, lastPreviewFrames;
    int previewRenders = 0;
   private:
    std::unique_ptr<corevideo::modules::ICompositor> inner_;
  };

  auto modules = corevideo::modules::createStubModules();
  CountingDecoder::resetAll();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<CountingDecoder>();
  auto ownedCompositor = std::make_unique<PlanRecordingCompositor>(std::move(modules.compositor));
  auto* compositor = ownedCompositor.get();
  modules.compositor = std::move(ownedCompositor);
  corevideo::core::MediaCore core(std::move(modules));

  // The clip on Program AND on Preview, under two different scene ids.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bClipScene("pgm", "load-scene-graph"), slice3bClipScene("pvw", "set-preview-scene")});
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return compositor->previewRenders > 0 && corevideo::testing::busSourceProducing(core, "media:clip");
  })) << "the preview composite never ran with the clip producing";
  for (int tick = 0; tick < 10; ++tick) {
    core.renderDisplayTick();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  // ONE decoder for both buses.
  EXPECT_EQ(CountingDecoder::created.load(), 1);

  // No `preview:` id anywhere in the preview plan - that rename is what used
  // to make Preview a second playback identity.
  for (const auto& layer : compositor->lastPreviewPlan.layers) {
    EXPECT_NE(layer.sourceId.rfind("preview:", 0), 0u)
        << "preview plan layer '" << layer.sourceId << "' is still namespaced";
  }
  const auto mediaLayer = std::find_if(
      compositor->lastPreviewPlan.layers.begin(), compositor->lastPreviewPlan.layers.end(),
      [](const auto& l) { return l.sourceId == "media:clip"; });
  ASSERT_NE(mediaLayer, compositor->lastPreviewPlan.layers.end())
      << "the preview plan carries no media:clip layer";

  // ...and the frame set it resolves against holds exactly ONE media:clip
  // picture, the same one Program drew: one source, one frame id.
  const auto mediaFrameIdsIn = [](const std::vector<corevideo::modules::VideoFrame>& frames) {
    std::vector<std::int64_t> ids;
    for (const auto& frame : frames) {
      if (frame.participantId == "media:clip") ids.push_back(frame.frameId);
    }
    return ids;
  };
  const auto previewIds = mediaFrameIdsIn(compositor->lastPreviewFrames);
  const auto programIds = mediaFrameIdsIn(compositor->lastProgramFrames);
  ASSERT_EQ(previewIds.size(), 1u) << "preview saw " << previewIds.size() << " media:clip frames";
  ASSERT_EQ(programIds.size(), 1u) << "program saw " << programIds.size() << " media:clip frames";
  EXPECT_EQ(previewIds.front(), programIds.front())
      << "preview resolved a DIFFERENT picture from program";
  EXPECT_EQ(CountingDecoder::restartsFor("media:clip"), 0);
}

// RULE 3: a clip that LEAVES Program while still cued goes back to a poster at
// zero, and the return rolls it from zero again - never from where it was.
TEST(MediaCoreCommand, AClipThatLeavesProgramAndReturnsRollsFromZeroAgain) {
  auto modules = corevideo::modules::createStubModules();
  CountingDecoder::resetAll();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<CountingDecoder>();
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  // Live on Program, with the clip ALSO cued in Preview so leaving Program
  // leaves it cued rather than releasing it outright.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bClipScene("pgm", "load-scene-graph"), slice3bClipScene("pvw", "set-preview-scene")});
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return CountingDecoder::lastFrameIdFor("media:clip") > 4;
  })) << "the live clip never rolled past its fourth picture";
  const auto rolledTo = CountingDecoder::lastFrameIdFor("media:clip");
  ASSERT_GT(rolledTo, 4);
  ASSERT_EQ(CountingDecoder::created.load(), 1);

  // Take it AWAY from Program (Program empty, still cued in Preview):
  // RestartCued - a NEW decoder at zero behind the frame still on the monitor.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bEmptyScene("a", "load-scene-graph"), slice3bClipScene("pvw", "set-preview-scene")});
  EXPECT_EQ(slice3bTransportState(core, "media:clip"), "cued");
  ASSERT_TRUE(slice3bPumpUntil(core, [&] { return CountingDecoder::created.load() == 2; }))
      << "leaving Program did not restart the cue on a fresh decoder";

  // Take it BACK. The return is an in-place Resume on that restarted decoder,
  // so it opens NO third decoder and the picture it rolls from is at the head
  // of the clip, not where it left off.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bClipScene("pgm", "load-scene-graph"), slice3bClipScene("pvw", "set-preview-scene")});
  EXPECT_EQ(slice3bTransportState(core, "media:clip"), "live");
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return CountingDecoder::lastFrameIdFor("media:clip") > 2;
  })) << "the returned clip never rolled";
  EXPECT_EQ(CountingDecoder::created.load(), 2) << "the return cold-started a third decoder";
  ASSERT_EQ(CountingDecoder::restartsFor("media:clip"), 1)
      << "the ids never went back to the head of the clip";
  EXPECT_LE(CountingDecoder::firstFrameIdAfterRestartFor("media:clip"), 3)
      << "the restart did not begin at the head of the clip";
  EXPECT_LT(CountingDecoder::firstFrameIdAfterRestartFor("media:clip"), rolledTo);
}

// RULE 4: a Take between two scenes that BOTH hold the clip changes nothing -
// one decoder, ids strictly increasing straight through the cut.
TEST(MediaCoreCommand, ATakeBetweenTwoScenesBothHoldingTheClipDoesNotRestartIt) {
  auto modules = corevideo::modules::createStubModules();
  CountingDecoder::resetAll();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<CountingDecoder>();
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  // Scene A on Program, scene B cued - both routing the SAME clip.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bClipScene("scene-a", "load-scene-graph"),
      slice3bClipScene("scene-b", "set-preview-scene")});
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return CountingDecoder::lastFrameIdFor("media:clip") > 3;
  })) << "the clip never rolled before the take";
  const auto beforeId = CountingDecoder::lastFrameIdFor("media:clip");
  const auto beforeIngested = corevideo::testing::busFramesIngested(core, "media:clip");

  // The Take: A -> B.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bClipScene("scene-b", "load-scene-graph"),
      slice3bClipScene("scene-a", "set-preview-scene")});
  EXPECT_EQ(slice3bTransportState(core, "media:clip"), "live");

  for (int tick = 0; tick < 20; ++tick) {
    core.renderDisplayTick();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return CountingDecoder::lastFrameIdFor("media:clip") > beforeId;
  })) << "the clip stopped rolling across the take";
  EXPECT_EQ(CountingDecoder::created.load(), 1) << "the take opened a second decoder";
  EXPECT_EQ(CountingDecoder::restartsFor("media:clip"), 0)
      << "the frame ids regressed across the take, so the clip restarted";
  EXPECT_GT(corevideo::testing::busFramesIngested(core, "media:clip"), beforeIngested)
      << "the picture on Program stopped advancing across the take";
}

// RULE 5: operator Pause holds the on-air frame and emits NO audio (not even
// silence); Play resumes the same decoder from where it froze.
TEST(MediaCoreCommand, PauseHoldsTheFrameAndSilencesAudioPlayResumes) {
  auto modules = corevideo::modules::createStubModules();
  CountingDecoder::resetAll();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<CountingDecoder>();
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  (void)core.applyCommands(corevideo::rpc::Json::Array{slice3bClipScene("pgm", "load-scene-graph")});
  ASSERT_TRUE(slice3bWaitForAudio(core, "media:clip"))
      << "the live clip never produced audio, so silencing it would prove nothing";
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return CountingDecoder::lastFrameIdFor("media:clip") > 2;
  })) << "the live clip never rolled";

  (void)core.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-media-transport"}, {"mediaAssetId", "clip"}, {"action", "pause"}});
  EXPECT_EQ(slice3bTransportState(core, "media:clip"), "paused");

  // Let the worker observe the pause and settle onto its held frame, THEN
  // measure the non-event over a fixed span - the only case a fixed pump is
  // allowed, because "it does not advance" has no predicate to wait on.
  slice3bPumpFor(core, 120);
  const auto heldIngested = corevideo::testing::busFramesIngested(core, "media:clip");
  ASSERT_GT(heldIngested, 0);
  const auto heldSourceFrameId = CountingDecoder::lastFrameIdFor("media:clip");
  const auto pausedWindows = slice3bAudioWindowsOver(core, "media:clip", 150);
  EXPECT_EQ(corevideo::testing::busFramesIngested(core, "media:clip"), heldIngested)
      << "the paused clip kept advancing on Program";
  EXPECT_EQ(CountingDecoder::lastFrameIdFor("media:clip"), heldSourceFrameId)
      << "the paused clip kept decoding";
  EXPECT_EQ(pausedWindows, 0u) << "the paused clip emitted audio";

  (void)core.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-media-transport"}, {"mediaAssetId", "clip"}, {"action", "play"}});
  EXPECT_EQ(slice3bTransportState(core, "media:clip"), "live");
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return corevideo::testing::busFramesIngested(core, "media:clip") > heldIngested;
  })) << "the resumed clip never advanced again";
  EXPECT_EQ(CountingDecoder::created.load(), 1)
      << "pause/play is a clock state, not a new decoder";
  EXPECT_EQ(CountingDecoder::restartsFor("media:clip"), 0)
      << "the resumed clip restarted from the top instead of continuing";
  EXPECT_TRUE(slice3bWaitForAudio(core, "media:clip")) << "the resumed clip stayed silent";
}

// RULE 6: a LOOPING background is live on either bus, refuses pause out loud,
// and a Take does not disturb it.
TEST(MediaCoreCommand, ALoopBackgroundIgnoresPauseAndSurvivesATake) {
  auto modules = corevideo::modules::createStubModules();
  CountingDecoder::resetAll();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<CountingDecoder>();
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  // The same background behind BOTH scenes.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bBackgroundScene("scene-a", "load-scene-graph"),
      slice3bBackgroundScene("scene-b", "set-preview-scene")});
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return CountingDecoder::lastFrameIdFor("background:bg") > 3;
  })) << "the loop background never rolled";
  EXPECT_EQ(slice3bTransportState(core, "background:bg"), "live");

  // Pause is REFUSED, loudly, and changes nothing.
  (void)core.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-media-transport"}, {"mediaAssetId", "bg"}, {"action", "pause"}});
  EXPECT_EQ(slice3bTransportState(core, "background:bg"), "live");
  {
    const auto& warnings = core.sceneValidationWarningsForTest();
    EXPECT_TRUE(std::any_of(warnings.begin(), warnings.end(), [](const std::string& warning) {
      return warning.find("loop") != std::string::npos;
    })) << "a refused loop pause must say why";
  }

  const auto beforeId = CountingDecoder::lastFrameIdFor("background:bg");
  const auto beforeIngested = corevideo::testing::busFramesIngested(core, "background:bg");
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bBackgroundScene("scene-b", "load-scene-graph"),
      slice3bBackgroundScene("scene-a", "set-preview-scene")});
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return CountingDecoder::lastFrameIdFor("background:bg") > beforeId &&
           corevideo::testing::busFramesIngested(core, "background:bg") > beforeIngested;
  })) << "the loop background stopped across the take";
  EXPECT_EQ(slice3bTransportState(core, "background:bg"), "live");
  EXPECT_EQ(CountingDecoder::created.load(), 1) << "the take restarted the loop background";
  EXPECT_EQ(CountingDecoder::restartsFor("background:bg"), 0);
}

// IDEMPOTENCE: the repeating sync channel re-asserts the same scene graph over
// and over. Re-sending it must be a no-op - no new decoder, no state change,
// and the picture keeps advancing right through the re-sends.
TEST(MediaCoreCommand, AnIdenticalSceneGraphResentChangesNothing) {
  auto modules = corevideo::modules::createStubModules();
  CountingDecoder::resetAll();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<CountingDecoder>();
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  (void)core.applyCommands(corevideo::rpc::Json::Array{slice3bClipScene("pgm", "load-scene-graph")});
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return CountingDecoder::lastFrameIdFor("media:clip") > 2;
  })) << "the clip never rolled";

  std::int64_t previousIngested = corevideo::testing::busFramesIngested(core, "media:clip");
  ASSERT_GT(previousIngested, 0);
  for (int resend = 0; resend < 5; ++resend) {
    (void)core.applyCommands(corevideo::rpc::Json::Array{slice3bClipScene("pgm", "load-scene-graph")});
    EXPECT_EQ(slice3bTransportState(core, "media:clip"), "live") << "re-send " << resend;
    ASSERT_TRUE(slice3bPumpUntil(core, [&] {
      return corevideo::testing::busFramesIngested(core, "media:clip") > previousIngested;
    })) << "the picture stopped advancing after re-send " << resend;
    previousIngested = corevideo::testing::busFramesIngested(core, "media:clip");
  }

  EXPECT_EQ(CountingDecoder::created.load(), 1) << "a re-send opened another decoder";
  EXPECT_EQ(CountingDecoder::restartsFor("media:clip"), 0) << "a re-send restarted the clip";
  const auto state = core.sessionState();
  EXPECT_EQ(state.get("mediaSources")->asArray().size(), 1u)
      << "a re-send added a second media source";
}

// ENDED: a clip whose media runs out holds its last picture, reads "ended",
// and an operator Play restarts it from the top on a fresh decoder.
TEST(MediaCoreCommand, AFinishedClipHoldsItsLastFrameAndReadsEnded) {
  auto modules = corevideo::modules::createStubModules();
  CountingDecoder::resetAll();
  // Ten pictures and then a GENUINE end of stream - the decoder says so
  // (IMediaVideoPrefetch::mediaEnded), which is what "ended" is decided from
  // now; the frame-arrival backstop is reserved for a decoder that cannot say.
  // It is still alive and still holding its last picture.
  corevideo::testing::ScriptedDecoder::resetScript();
  corevideo::testing::ScriptedDecoder::stallAfterFrame.store(10);
  corevideo::testing::ScriptedDecoder::stallMs.store(-1);
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<corevideo::testing::ScriptedDecoder>();
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  (void)core.applyCommands(corevideo::rpc::Json::Array{slice3bClipScene("pgm", "load-scene-graph")});
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return slice3bTransportState(core, "media:clip") == "ended";
  }, 3000)) << "the finished clip never read ended";
  EXPECT_EQ(CountingDecoder::lastFrameIdFor("media:clip"), 10);

  // It still HOLDS its last picture: the bus removes a media source only when
  // selectVideo has nothing at all, so the source staying on the bus at a
  // FROZEN deduped count is the held frame. (The count is below 10 by design -
  // MediaVideoPresentation::select drains every frame already due and keeps
  // only the last, so ticks slower than the decoder collapse pictures. What
  // matters is that it stopped moving, not what it stopped at.)
  const auto heldIngested = corevideo::testing::busFramesIngested(core, "media:clip");
  ASSERT_GT(heldIngested, 0);
  const auto expectedClipLuma = 0.114 * 0x40 + 0.587 * 0x40 + 0.299 * 0x40;
  slice3bPumpFor(core, 120);
  EXPECT_EQ(corevideo::testing::busFramesIngested(core, "media:clip"), heldIngested)
      << "the ended clip advanced past the end of its media";
  EXPECT_NE(corevideo::testing::busSourceRow(core.sessionState(), "media:clip"), nullptr)
      << "the ended clip fell off the source bus instead of holding its last frame";
  EXPECT_NEAR(slice3bProgramLuma(core), expectedClipLuma, 2.0)
      << "Program lost the ended clip's last picture (a slate reads ~28-31)";
  EXPECT_EQ(CountingDecoder::created.load(), 1);

  // Play on an ENDED clip is a restart from the top.
  (void)core.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-media-transport"}, {"mediaAssetId", "clip"}, {"action", "play"}});
  EXPECT_EQ(slice3bTransportState(core, "media:clip"), "live");
  ASSERT_TRUE(slice3bPumpUntil(core, [&] { return CountingDecoder::created.load() == 2; }))
      << "Play on an ended clip did not open a fresh decoder";
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return CountingDecoder::restartsFor("media:clip") == 1;
  })) << "the restarted clip did not begin at the head of its media";
  EXPECT_LE(CountingDecoder::firstFrameIdAfterRestartFor("media:clip"), 3);
  corevideo::testing::ScriptedDecoder::resetScript();
}

// ---------------------------------------------------------------------------
// #535 slice 3b final review, CRITICAL 1: `Ended` used to be a 500 ms
// frame-arrival timeout AND terminal. These tests are the bound on what may
// reach it and on what may get out of it. They drive the REAL
// core::MediaTransports through MediaCore's command surface.
// ---------------------------------------------------------------------------

using corevideo::testing::ScriptedDecoder;

// A clip cut COLD to Program opens its decoder on the worker's first
// iteration, and a Media Foundation open is 95-250 ms while the FFmpeg
// fallback spawns a whole process. The old guard flipped it to Ended after
// 500 ms of no new frameId with an empty queue - i.e. before the clip had
// ever produced a picture - and Ended was terminal: frozen and silent on air,
// with the only recovery gesture restarting it from 0.
TEST(MediaCoreCommand, AClipWhoseFirstFrameIsLateStillRollsAndNeverReadsEnded) {
  auto modules = corevideo::modules::createStubModules();
  CountingDecoder::resetAll();
  ScriptedDecoder::resetScript();
  // Longer than the OLD 500 ms window, and longer than a real cold open, so
  // the test is about the rule and not about this machine's speed.
  ScriptedDecoder::firstFrameDelayMs.store(900);
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<ScriptedDecoder>();
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  (void)core.applyCommands(corevideo::rpc::Json::Array{slice3bClipScene("pgm", "load-scene-graph")});
  bool everReadEnded = false;
  const bool rolled = slice3bPumpUntil(core, [&] {
    if (slice3bTransportState(core, "media:clip") == "ended") everReadEnded = true;
    return CountingDecoder::lastFrameIdFor("media:clip") > 2;
  }, 6000);
  ScriptedDecoder::resetScript();
  ASSERT_TRUE(rolled) << "the late-opening clip never rolled at all";
  EXPECT_FALSE(everReadEnded)
      << "a clip that had not yet produced a single picture was called finished";
  EXPECT_EQ(slice3bTransportState(core, "media:clip"), "live");
  EXPECT_EQ(CountingDecoder::created.load(), 1) << "the late open cold-started a second decoder";
  EXPECT_EQ(CountingDecoder::restartsFor("media:clip"), 0);
}

// A mid-roll stall past the ended window is NOT the end of the media: an
// FFmpeg resume attempt off its ladder, a NAS hiccup, a loaded box. When the
// decoder hands back a picture the transport must return to Live on its own -
// no operator gesture, which is OpenLive and restarts from 0 - and keep its
// position.
TEST(MediaCoreCommand, AClipThatStallsMidRollAndResumesReturnsToLiveWithoutRestarting) {
  auto modules = corevideo::modules::createStubModules();
  CountingDecoder::resetAll();
  ScriptedDecoder::resetScript();
  ScriptedDecoder::stallAfterFrame.store(3);
  ScriptedDecoder::stallMs.store(800);  // past the old 500 ms window, then back
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<ScriptedDecoder>();
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  (void)core.applyCommands(corevideo::rpc::Json::Array{slice3bClipScene("pgm", "load-scene-graph")});
  const bool reachedEnded = slice3bPumpUntil(core, [&] {
    return slice3bTransportState(core, "media:clip") == "ended";
  }, 4000);
  const auto stalledAt = CountingDecoder::lastFrameIdFor("media:clip");
  const bool recovered = reachedEnded && slice3bPumpUntil(core, [&] {
    return slice3bTransportState(core, "media:clip") == "live";
  }, 5000);
  const bool rolledAgain = recovered && slice3bPumpUntil(core, [&] {
    return CountingDecoder::lastFrameIdFor("media:clip") > stalledAt;
  }, 4000);
  ScriptedDecoder::resetScript();

  ASSERT_TRUE(reachedEnded) << "the stalled clip never reported its decoder's end of stream";
  ASSERT_GE(stalledAt, 3);
  ASSERT_TRUE(recovered) << "the resumed clip never came back from ended - Ended is still terminal";
  ASSERT_TRUE(rolledAgain) << "the recovered clip never rolled again";
  EXPECT_EQ(CountingDecoder::created.load(), 1) << "the recovery opened a fresh decoder";
  EXPECT_EQ(CountingDecoder::restartsFor("media:clip"), 0)
      << "the recovered clip restarted from the top instead of keeping its position";
}

// ---------------------------------------------------------------------------
// #535 slice 3b final review, IMPORTANT 3: `applyPreviewScene` is reached from
// the shell's Take batch AND from the repeating spine sync. One interleaved
// spine tick carrying the post-swap Preview while Program still holds the
// outgoing scene leaves a cued clip absent from BOTH desired sets - and an
// immediate Release destroys the warm decoder the Take is about to claim.
// ---------------------------------------------------------------------------
TEST(MediaCoreCommand, ACuedClipSurvivesAPreviewOnlySpineTickBetweenTheCueAndTheTake) {
  auto modules = corevideo::modules::createStubModules();
  CountingDecoder::resetAll();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<CountingDecoder>();
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  // Cue: Program holds an empty scene, Preview holds the clip.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bEmptyScene("a", "load-scene-graph"), slice3bClipScene("pvw", "set-preview-scene")});
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return CountingDecoder::lastFrameIdFor("media:clip") > 0;
  })) << "the cue never produced its poster";
  EXPECT_EQ(slice3bTransportState(core, "media:clip"), "cued");
  ASSERT_EQ(CountingDecoder::created.load(), 1);

  // THE INTERLEAVED SPINE TICK: set-preview-scene ALONE, carrying the
  // post-swap Preview. For this one tick the clip is on neither bus.
  (void)core.applyCommand(slice3bEmptyScene("a", "set-preview-scene"));
  EXPECT_NE(slice3bMediaSource(core.sessionState(), "media:clip"), nullptr)
      << "one preview-only tick retired the cued clip's transport";

  // The Take lands right behind it.
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bClipScene("pgm", "load-scene-graph"), slice3bEmptyScene("a", "set-preview-scene")});
  EXPECT_EQ(slice3bTransportState(core, "media:clip"), "live");
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return corevideo::testing::busFramesIngested(core, "media:clip") > 0 &&
           CountingDecoder::lastFrameIdFor("media:clip") > 1;
  })) << "the taken clip never rolled on Program";
  EXPECT_EQ(CountingDecoder::created.load(), 1)
      << "the Take cold-started a second decoder: the interleaved preview tick "
         "destroyed the warm one";
}

// A source REALLY gone is still released - just one beat later. The grace is a
// delay, not an exemption.
TEST(MediaCoreCommand, AGenuinelyUnroutedClipIsStillReleasedAfterTheGrace) {
  auto modules = corevideo::modules::createStubModules();
  CountingDecoder::resetAll();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<CountingDecoder>();
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  (void)core.applyCommands(corevideo::rpc::Json::Array{slice3bClipScene("pgm", "load-scene-graph")});
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return CountingDecoder::lastFrameIdFor("media:clip") > 1;
  })) << "the clip never rolled";

  (void)core.applyCommands(corevideo::rpc::Json::Array{slice3bEmptyScene("a", "load-scene-graph")});
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return slice3bMediaSource(core.sessionState(), "media:clip") == nullptr &&
           corevideo::testing::busSourceRow(core.sessionState(), "media:clip") == nullptr;
  }, 4000)) << "an unrouted clip was never released: the grace became an exemption";
}

// ---------------------------------------------------------------------------
// #535 slice 3b final review, IMPORTANT 4: MediaTransports::operatorAction has
// a three-tier lookup (the exact `media:<assetId>` row, then the first
// non-loop source for the asset, then the first match at all). The shell
// mirror was tested; the CORE tiers were not. A background-only asset has no
// `media:` row, so the refusal comes from the LAST tier - and it must carry a
// real reason naming the loop, never silently do nothing.
// ---------------------------------------------------------------------------
TEST(MediaCoreCommand, ATransportOnABackgroundOnlyAssetIsRefusedAndNamesTheLoop) {
  auto modules = corevideo::modules::createStubModules();
  CountingDecoder::resetAll();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<CountingDecoder>();
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  (void)core.applyCommands(corevideo::rpc::Json::Array{
      slice3bBackgroundScene("scene-a", "load-scene-graph")});
  ASSERT_TRUE(slice3bPumpUntil(core, [&] {
    return slice3bTransportState(core, "background:bg") == "live";
  })) << "the loop background never went live";
  ASSERT_EQ(slice3bMediaSource(core.sessionState(), "media:bg"), nullptr)
      << "this asset must exist ONLY as background:bg for the test to exercise the last tier";

  for (const char* action : {"pause", "play"}) {
    (void)core.applyCommand(corevideo::rpc::Json::Object{
        {"type", "set-media-transport"}, {"mediaAssetId", "bg"}, {"action", action}});
    EXPECT_EQ(slice3bTransportState(core, "background:bg"), "live")
        << "a refused " << action << " changed the transport";
    const auto& warnings = core.sceneValidationWarningsForTest();
    EXPECT_TRUE(std::any_of(warnings.begin(), warnings.end(), [](const std::string& warning) {
      return warning.find("background:bg") != std::string::npos &&
             warning.find("loop") != std::string::npos;
    })) << "the refusal for " << action << " must name the source and say it is a loop";
  }

  // And an asset with no transport at all is refused by NAME, not silently.
  (void)core.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-media-transport"}, {"mediaAssetId", "no-such-asset"}, {"action", "pause"}});
  const auto& warnings = core.sceneValidationWarningsForTest();
  EXPECT_TRUE(std::any_of(warnings.begin(), warnings.end(), [](const std::string& warning) {
    return warning.find("no-such-asset") != std::string::npos;
  })) << "a transport command for an unknown asset must say so";
  EXPECT_EQ(CountingDecoder::created.load(), 1);
}
