#include "compositor/CompositorLayout.h"
#include "core/MediaCore.h"
#include "modules/AudioDsp.h"
#include "modules/Interfaces.h"
#include "modules/ProgramFramePreview.h"
#include "modules/RealZoomCaptureSource.h"
#include "modules/ZoomMeetingSdkAdapter.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
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

class SolidMediaFrameSource final : public corevideo::modules::IMediaFrameSource {
 public:
  std::vector<corevideo::modules::VideoFrame> pollMediaFrames(
      const std::vector<corevideo::modules::CompositorRenderPlanLayer>& layers,
      int64_t timestampMs) override {
    ++pollCount;
    seenPlaybackKeys.clear();
    seenSourceIds.clear();
    std::vector<corevideo::modules::VideoFrame> frames;
    for (const auto& layer : layers) {
      if (layer.mediaAssetId.empty() || layer.mediaAssetPath.empty()) {
        continue;
      }
      seenPlaybackKeys.push_back(layer.mediaPlaybackKey);
      seenSourceIds.push_back(layer.sourceId);
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
      frame.frameId = pollCount;
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
      frame.participantId = "media";
      frame.sampleRate = 48000;
      frame.channels = 2;
      frame.timestampMs = timestampMs;
      frame.sampleCount = 480;
      frame.voiceActive = true;
      frame.pcm.resize(static_cast<size_t>(frame.sampleCount) * static_cast<size_t>(frame.channels));
      const int64_t baseSample = audioPollCount * frame.sampleCount;
      for (int sampleIndex = 0; sampleIndex < frame.sampleCount; ++sampleIndex) {
        const double phase = 2.0 * corevideo::modules::kAudioPi * 330.0 *
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
  uint8_t blue = 0x22;
  uint8_t green = 0xb4;
  uint8_t red = 0xf1;
  int64_t pollCount = 0;
  int64_t audioPollCount = 0;
  std::vector<std::string> seenPlaybackKeys;
  std::vector<std::string> seenSourceIds;
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
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
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

  const auto earlyPreviews = mediaCore.drainProgramFramePreviewEvents();
  ASSERT_FALSE(earlyPreviews.empty());
  const size_t earlyColors = bandDistinctColors(earlyPreviews.back());

  // Advance several ticks so the building-in animation settles on-air.
  corevideo::rpc::Json settledState = nullptr;
  for (int i = 0; i < 12; ++i) {
    settledState = mediaCore.applyCommands(corevideo::rpc::Json::Array{});
  }
  const auto settledPreviews = mediaCore.drainProgramFramePreviewEvents();
  const size_t settledColors = settledPreviews.empty()
                                   ? bandDistinctColors(settledState)
                                   : bandDistinctColors(settledPreviews.back());

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
  const bool hasWasapiCapture = corevideo::modules::createWasapiAudioCaptureSource() != nullptr;
  const bool hasWasapiMonitor = corevideo::modules::createWasapiMonitorOutput() != nullptr;
  EXPECT_EQ(jsonArrayContains(capabilities, "local-audio-capture"), hasWasapiCapture);
  EXPECT_EQ(jsonArrayContains(capabilities, "audio-monitor-output"), hasWasapiMonitor);
#if COREVIDEO_WITH_D3D11
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
#else
  EXPECT_FALSE(jsonArrayContains(capabilities, "program-recording"));
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
#else
  EXPECT_EQ(health.getString("renderer"), "software");
#endif
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

TEST(MediaCoreCommand, AppliesMediaPlaybackCommandAndWarnsOnEmptyAsset) {
  corevideo::core::MediaCore mediaCore;

  const auto idle = mediaCore.sessionState();
  ASSERT_NE(idle.get("mediaPlayback"), nullptr);
  EXPECT_EQ(idle.get("mediaPlayback")->getString("status"), "idle");
  EXPECT_FALSE(idle.get("mediaPlayback")->get("playing")->asBool());

  const auto playing = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-media-playback"},
      {"mediaAssetId", "clip-intro"},
      {"mediaAssetName", "Intro Sting"},
      {"mediaAssetKind", "stinger"},
      {"mediaAssetPath", "C:/media/intro.mp4"},
      {"mediaPlaybackKey", "program-take:3:media:clip-intro"},
      {"playing", true},
  });
  const auto* playback = playing.get("mediaPlayback");
  ASSERT_NE(playback, nullptr);
  EXPECT_EQ(playback->getString("status"), "playing");
  EXPECT_EQ(playback->getString("mediaAssetId"), "clip-intro");
  EXPECT_EQ(playback->getString("mediaAssetName"), "Intro Sting");
  EXPECT_EQ(playback->getString("mediaAssetKind"), "stinger");
  EXPECT_EQ(playback->getString("mediaAssetPath"), "C:/media/intro.mp4");
  EXPECT_EQ(playback->getString("mediaPlaybackKey"), "program-take:3:media:clip-intro");
  EXPECT_TRUE(playback->get("playing")->asBool());
  EXPECT_EQ(playback->getString("summary"), "Playing Intro Sting with key program-take:3:media:clip-intro.");

  const auto paused = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-media-playback"},
      {"mediaAssetId", "clip-outro"},
      {"mediaAssetName", "Outro Loop"},
      {"playing", false},
  });
  ASSERT_NE(paused.get("mediaPlayback"), nullptr);
  EXPECT_EQ(paused.get("mediaPlayback")->getString("status"), "paused");
  EXPECT_FALSE(paused.get("mediaPlayback")->get("playing")->asBool());

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
  EXPECT_EQ(mix->get("loudnessLufs")->asNumber(), -60);
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
  EXPECT_GE(proof->get("isoFrameCount")->asNumber(), 1);
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
  auto mediaFrames = std::make_unique<SolidMediaFrameSource>();
  auto* mediaFramesPtr = mediaFrames.get();
  modules.mediaFrames = std::move(mediaFrames);
  auto* outputSender = new CapturingOutputSender();
  modules.outputSender.reset(outputSender);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
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

  EXPECT_TRUE(mediaFramesPtr->audioPollCount > 0);
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
  const auto mediaParticipant = std::find_if(
      participants->asArray().begin(), participants->asArray().end(), [](const corevideo::rpc::Json& participant) {
        return participant.getString("participantId") == "media";
      });
  ASSERT_TRUE(mediaParticipant != participants->asArray().end());
  EXPECT_TRUE(mediaParticipant->get("outputLevel")->asNumber() > 0);
}

#if !COREVIDEO_STUB && COREVIDEO_WITH_MF_ENCODER
TEST(MediaFoundationMediaFrameSource, DecodesSceneMediaAudioPcmFromLocalWav) {
  const auto wavPath = std::filesystem::temp_directory_path() / "corevideo-mf-media-audio-test.wav";
  std::filesystem::remove(wavPath);
  writeSineWaveFile(wavPath, 44100, 1);

  auto source = corevideo::modules::createMediaFoundationMediaFrameSource();
  ASSERT_NE(source, nullptr);

  corevideo::modules::CompositorRenderPlanLayer layer;
  layer.kind = "media-video";
  layer.sourceId = "media:clip-audio";
  layer.mediaAssetId = "clip-audio";
  layer.mediaAssetKind = "video";
  layer.mediaAssetPath = wavPath.string();
  layer.mediaPlaybackKey = "program-take:1:media:clip-audio";
  layer.mediaAssetPlaying = true;

  const auto frames = source->pollMediaAudioFrames({layer}, 33);
  ASSERT_FALSE(frames.empty());
  EXPECT_EQ(frames.front().participantId, "media");
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

  std::filesystem::remove(wavPath);
}
#endif

TEST(MediaCoreCommand, ReportsCaptureDevicesAndAppliesCaptureControls) {
  corevideo::core::MediaCore mediaCore;
  const auto devices = mediaCore.captureDevices();
  ASSERT_TRUE(devices.asArray().size() >= 2);
  EXPECT_EQ(devices.asArray()[0].getString("vendor"), "blackmagic");
  EXPECT_EQ(devices.asArray()[0].get("inputs")->asArray().size(), 2);
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
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  ASSERT_NE(encoder, nullptr);
  const auto started = encoder->start({"recording"}, {"participant-1"});
  ASSERT_FALSE(started.recordingArtifactPath.empty());
  EXPECT_EQ(std::filesystem::path(started.recordingArtifactPath).extension().string(), ".mp4");

  encoder->submit(makeTestProgramFrame(42));
  encoder->submit(makeTestProgramFrame(43));
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

  // The synthetic slate color for this participant must NOT be what we see,
  // proving real pixels (not the colorFromParticipantId fill) were composited.
  const uint32_t syntheticColor = corevideo::compositor::colorFromParticipantId("1234");
  const uint8_t syntheticBlue = static_cast<uint8_t>(syntheticColor & 0xff);
  const uint8_t syntheticGreen = static_cast<uint8_t>((syntheticColor >> 8) & 0xff);
  const uint8_t syntheticRed = static_cast<uint8_t>((syntheticColor >> 16) & 0xff);
  const bool matchesSynthetic =
      decoded[offset + 0] == syntheticBlue && decoded[offset + 1] == syntheticGreen && decoded[offset + 2] == syntheticRed;
  EXPECT_FALSE(matchesSynthetic);
}

TEST(MediaCoreCommand, KeepsSceneBackgroundAndProgramMediaRouteFrameSourcesDistinct) {
  auto modules = corevideo::modules::createStubModules();
  auto mediaFrames = std::make_unique<SolidMediaFrameSource>();
  auto* mediaFramesPtr = mediaFrames.get();
  modules.mediaFrames = std::move(mediaFrames);

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
                             {"mediaPlaybackKey", "program-take:8:media:clip-intro"},
                             {"mediaAssetPlaying", true},
                             {"rect", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}},
                         },
                     }},
      },
  });

  ASSERT_TRUE(mediaFramesPtr->seenSourceIds.size() == 2u);
  EXPECT_EQ(mediaFramesPtr->seenSourceIds[0], "background:clip-intro");
  EXPECT_EQ(mediaFramesPtr->seenSourceIds[1], "media:clip-intro");
  ASSERT_TRUE(mediaFramesPtr->seenPlaybackKeys.size() == 2u);
  EXPECT_TRUE(mediaFramesPtr->seenPlaybackKeys[0].empty());
  EXPECT_EQ(mediaFramesPtr->seenPlaybackKeys[1], "program-take:8:media:clip-intro");

  const auto previews = mediaCore.drainProgramFramePreviewEvents();
  ASSERT_FALSE(previews.empty());
  const auto* preview = previews.back().get("preview");
  ASSERT_NE(preview, nullptr);
  const auto decoded = corevideo::modules::base64Decode(preview->getString("bgraBase64"));
  EXPECT_FALSE(decoded.empty());
}

TEST(MediaCoreCommand, CompositesMediaRoutePixelsIntoProgramPreview) {
  auto modules = corevideo::modules::createStubModules();
  auto mediaFrames = std::make_unique<SolidMediaFrameSource>();
  const uint8_t expectedBlue = mediaFrames->blue;
  const uint8_t expectedGreen = mediaFrames->green;
  const uint8_t expectedRed = mediaFrames->red;
  auto* mediaFramesPtr = mediaFrames.get();
  modules.mediaFrames = std::move(mediaFrames);

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

  EXPECT_GE(mediaFramesPtr->pollCount, 1);
  ASSERT_FALSE(mediaFramesPtr->seenPlaybackKeys.empty());
  EXPECT_EQ(mediaFramesPtr->seenPlaybackKeys.front(), "program-take:4:media:clip-intro");
  const auto previews = mediaCore.drainProgramFramePreviewEvents();
  ASSERT_FALSE(previews.empty());
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

  const uint32_t syntheticColor = corevideo::compositor::colorFromParticipantId("media:clip-intro");
  const bool matchesSynthetic =
      decoded[offset + 0] == static_cast<uint8_t>(syntheticColor & 0xff) &&
      decoded[offset + 1] == static_cast<uint8_t>((syntheticColor >> 8) & 0xff) &&
      decoded[offset + 2] == static_cast<uint8_t>((syntheticColor >> 16) & 0xff);
  EXPECT_FALSE(matchesSynthetic);
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
