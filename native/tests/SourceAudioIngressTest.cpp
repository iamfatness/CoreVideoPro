#include "core/SourceAudioIngress.h"
#include "core/CaptureBusRoster.h"
#include "core/MediaCore.h"
#include "modules/WinUiCaptureDeviceAdapter.h"
#include <gtest/gtest.h>

using namespace corevideo;
namespace {
modules::AudioFrame pcm(std::string id, int64_t stamp, float value) {
  modules::AudioFrame f;
  f.participantId = std::move(id); f.timestampMs = stamp;
  f.sampleRate = 48000; f.channels = 2; f.sampleCount = 480;
  f.pcm.assign(960, value);
  return f;
}
class Capture final : public modules::IAudioCaptureSource {
 public:
  void configure(const std::vector<modules::CaptureAudioSourceConfig>&) override {}
  std::vector<modules::AudioFrame> pollAudioFrames(int64_t) override {
    auto result = std::move(pending); pending.clear(); return result;
  }
  std::vector<modules::AudioFrame> pending;
};
class Transport final : public modules::ICaptureDevice {
 public:
  std::vector<modules::CaptureDeviceInfo> enumerate() const override { return {}; }
  std::vector<modules::CaptureDeviceInfo> selectInput(const std::string&, const std::string&) override { return {}; }
  std::vector<modules::CaptureDeviceInfo> setAudioSyncOffset(const std::string&, int) override { return {}; }
  std::vector<modules::CaptureDeviceInfo> connect(const std::string&) override { return {}; }
  std::vector<std::string> audioSourceIds() const override { return {"capture:srt-1"}; }
  void captureAudioTick(int64_t stamp) override { postAudio(pcm("capture:srt-1", stamp, .75f)); }
};
}

TEST(SourceAudioIngress, CapturePacketsKeepPayloadOrderTimingAndAudioOnlyIdentityThroughGaps) {
  core::SourceBus bus;
  core::stageCaptureAudioSources(bus, {pcm("capture:mic", 100, .25f), pcm("capture:mic", 110, -.5f)}, {"capture:mic"});
  auto* identity = bus.sourceFor("capture:mic");
  ASSERT_NE(identity, nullptr);
  bus.ingest(0, 1); // render cannot consume PCM
  auto out = bus.ingestAudio(0, 2);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].timestampMs, 100); EXPECT_EQ(out[1].timestampMs, 110);
  EXPECT_EQ(out[0].pcm, std::vector<float>(960, .25f));
  EXPECT_EQ(out[1].pcm, std::vector<float>(960, -.5f));
  EXPECT_EQ(out[0].sampleCount, 480); EXPECT_EQ(out[0].channels, 2);
  EXPECT_TRUE(bus.ingestAudio(0, 3).empty());
  core::syncCaptureSources(bus, {});
  core::stageCaptureAudioSources(bus, {}, {"capture:mic"});
  EXPECT_EQ(bus.sourceFor("capture:mic"), identity);
  EXPECT_TRUE(bus.ingestAudio(0, 300000003).empty());
  EXPECT_EQ(bus.snapshot(300000003).front().counters.audioPacketsIngested, 2u);
  EXPECT_EQ(bus.snapshot(300000003).front().counters.audioSamplesIngested, 960u);
  EXPECT_EQ(bus.healthFor("capture:mic", 300000003), core::SourceHealth::Stalled);
  core::stageCaptureAudioSources(bus, {}, {});
  EXPECT_FALSE(bus.contains("capture:mic"));
}

TEST(SourceAudioIngress, VideoDisconnectionClearsPictureButPreservesConfiguredAudio) {
  core::SourceBus bus;
  core::stageCaptureAudioSources(bus, {pcm("capture:camera", 12, .2f)}, {"capture:camera"});
  auto* identity = bus.sourceFor("capture:camera");
  modules::VideoFrame video;
  video.participantId = "capture:camera"; video.frameId = 1;
  video.pixelWidth = 640; video.pixelHeight = 360;
  core::syncCaptureSources(bus, {video});
  EXPECT_EQ(bus.sourceFor("capture:camera"), identity);
  EXPECT_TRUE(identity->descriptor().hasVideo);
  EXPECT_EQ(bus.ingest(0, 10).video.size(), 1u);
  core::syncCaptureSources(bus, {});
  EXPECT_FALSE(identity->descriptor().hasVideo);
  EXPECT_TRUE(bus.ingest(0, 20).video.empty());
  EXPECT_EQ(bus.ingestAudio(0, 20).size(), 1u);
  core::stageCaptureAudioSources(bus, {}, {});
  EXPECT_TRUE(bus.empty());
}

TEST(SourceAudioIngress, RemovingAudioKeepsVideoWithoutReplayingPcm) {
  core::SourceBus bus;
  modules::VideoFrame video;
  video.participantId = "capture:camera"; video.frameId = 8;
  video.pixelWidth = 640; video.pixelHeight = 360;
  core::syncCaptureSources(bus, {video});
  core::stageCaptureAudioSources(bus, {pcm(video.participantId, 12, .2f)}, {video.participantId});
  ASSERT_EQ(bus.ingestAudio(0, 1).size(), 1u);
  core::stageCaptureAudioSources(bus, {}, {});
  EXPECT_TRUE(bus.ingestAudio(0, 2).empty());
  EXPECT_EQ(bus.ingest(0, 2).video.size(), 1u);
  EXPECT_FALSE(bus.sourceFor(video.participantId)->descriptor().hasAudio);
}

TEST(SourceAudioIngress, MediaPauseClearsPcmAndRetiredSourcesAreNotResurrected) {
  core::SourceBus bus;
  bus.add(std::make_shared<core::MediaAssetSource>("media:clip", "media", 640, 360));
  auto* identity = bus.sourceFor("media:clip");
  core::stageMediaAudioSources(bus, {pcm("media:clip", 22, .4f)});
  bus.ingest(0, 1);
  auto out = bus.ingestAudio(0, 1);
  ASSERT_EQ(out.size(), 1u); EXPECT_EQ(out[0].timestampMs, 22);
  EXPECT_EQ(out[0].pcm, std::vector<float>(960, .4f));
  core::stageMediaAudioSources(bus, {}); // paused/cued transport emits nothing
  EXPECT_TRUE(bus.ingestAudio(0, 2).empty());
  EXPECT_EQ(bus.sourceFor("media:clip"), identity);
  core::stageMediaAudioSources(bus, {pcm("media:clip", 42, .7f)});
  EXPECT_EQ(bus.ingestAudio(0, 3).size(), 1u);
  EXPECT_EQ(bus.snapshot(3).front().counters.audioPacketsIngested, 2u);
  bus.remove("media:clip");
  core::stageMediaAudioSources(bus, {pcm("media:clip", 62, .9f)});
  EXPECT_TRUE(bus.empty()); EXPECT_TRUE(bus.ingestAudio(0, 4).empty());
}

TEST(SourceAudioIngress, MetadataAndConfiguredInputsDoNotCountAsReceivedPcm) {
  core::SourceBus bus;
  auto frame = pcm("capture:mic", 1, 0.f); frame.pcm.clear();
  core::stageCaptureAudioSources(bus, {frame}, {"capture:mic", "local-machine-audio"});
  bus.ingestAudio(0, 1);
  for (const auto& s : bus.snapshot(1)) {
    EXPECT_EQ(s.counters.audioPacketsIngested, 0u);
    EXPECT_EQ(s.health, core::SourceHealth::Warming);
  }
}

TEST(SourceAudioIngress, CaptureCannotOverwriteMediaOwnership) {
  core::SourceBus bus;
  bus.add(std::make_shared<core::MediaAssetSource>("media:clip", "media", 640, 360));
  auto* identity = bus.sourceFor("media:clip");
  core::stageCaptureAudioSources(bus, {pcm("media:clip", 1, .8f)}, {"media:clip"});
  EXPECT_EQ(bus.sourceFor("media:clip"), identity);
  EXPECT_TRUE(bus.ingestAudio(0, 1).empty());
}

TEST(SourceAudioIngress, ShellCaptureWrapperForwardsTransportMembershipAndPcm) {
  modules::WinUiCaptureDeviceAdapter wrapper(std::make_unique<Transport>());
  core::SourceBus bus;
  auto out = core::ingestSourceAudio(bus, {}, nullptr, &wrapper, nullptr, {}, 456, 0, 1);
  ASSERT_EQ(out.size(), 1u); EXPECT_EQ(out[0].timestampMs, 456);
  EXPECT_EQ(out[0].pcm, std::vector<float>(960, .75f));
  EXPECT_TRUE(bus.contains("capture:srt-1"));
}

TEST(SourceAudioIngress, MediaCoreMixerGatherPublishesCapturePcmCounters) {
  auto modules = modules::createStubModules();
  auto capture = std::make_unique<Capture>();
  capture->pending = {pcm("local-machine-audio", 789, .25f)};
  modules.audioCapture = std::move(capture);
  core::MediaCore core(std::move(modules)); std::mutex mutex;
  core.renderAudioOutputTick(mutex);
  const auto state = core.sessionState();
  bool found = false;
  for (const auto& s : state.get("sources")->asArray()) if (s.getString("sourceId") == "local-machine-audio") {
    found = true;
    EXPECT_EQ(s.get("audioPacketsIngested")->asNumber(), 1);
    EXPECT_EQ(s.get("audioSamplesIngested")->asNumber(), 480);
  }
  EXPECT_TRUE(found);
}
