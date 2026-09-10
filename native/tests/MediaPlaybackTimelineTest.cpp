#include "modules/MediaPlaybackTimeline.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
using namespace corevideo::modules;
TEST(MediaPlaybackTimeline, VideoUsesPtsAcrossRepeatedAndIrregularPolls) {
  MediaPlaybackTimeline clock;
  clock.configure("clip:play1", true, 10000000);
  EXPECT_TRUE(clock.videoDue(0, 10000000));
  EXPECT_FALSE(clock.videoDue(333333, 10166667)); // 30fps sample held across a60Hz poll.
  EXPECT_TRUE(clock.videoDue(333333, 10333333));
  EXPECT_FALSE(clock.videoDue(10000000, 19999999));
  EXPECT_TRUE(clock.videoDue(10000000, 20000000)); // A source second stays a second.
  EXPECT_TRUE(clock.videoDue(7000000, 18000000)); // Stall does not move epoch.
}
TEST(MediaPlaybackTimeline, RestartAndPauseResetSharedGeneration) {
  MediaPlaybackTimeline clock;
  EXPECT_TRUE(clock.configure("clip:play1", true, 100));
  const auto generation = clock.generation();
  EXPECT_FALSE(clock.configure("clip:play1", true, 10000000));
  EXPECT_EQ(clock.generation(), generation);
  EXPECT_TRUE(clock.configure("clip:play1", false, 10000000));
  EXPECT_EQ(clock.elapsed100ns(20000000), 0);
  EXPECT_TRUE(clock.configure("clip:play2", true, 20000000));
  EXPECT_EQ(clock.elapsed100ns(20000000), 0);
}
TEST(MediaAudioWindows, DecoderPacketBoundariesDoNotChangeSampleDuration) {
  MediaAudioWindows audio(48000, 1);
  for (int packet = 0; packet < 15; ++packet) {
    std::vector<float> pcm(1024);
    for (int n = 0; n < 1024; ++n) pcm[n] = static_cast<float>(packet * 1024 + n);
    audio.append(static_cast<int64_t>(packet) * 1024 * 10000000 / 48000, std::move(pcm));
  }
  for (int tick = 0; tick < 16; ++tick) {
    const auto pcm = audio.take(960);
    EXPECT_EQ(pcm.size(), 960u);
    for (int n = 0; n < 960; ++n) EXPECT_EQ(pcm[n], static_cast<float>(tick * 960 + n));
  }
}
TEST(MediaAudioWindows, PreservesPtsGapAndClearsOldAudioOnRestart) {
  MediaAudioWindows audio(48000, 1);
  audio.append(100000, std::vector<float>(480, 0.5f)); // First10ms are absent.
  const auto pcm = audio.take(960);
  EXPECT_EQ(pcm[0], 0.f); EXPECT_EQ(pcm[479], 0.f);
  EXPECT_EQ(pcm[480], 0.5f); EXPECT_EQ(pcm[959], 0.5f);
  audio.append(200000, std::vector<float>(960, 0.9f));
  audio.reset(48000, 1);
  EXPECT_EQ(audio.cursor(), 0);
  EXPECT_EQ(audio.take(960).back(), 0.f);
}

#include "modules/OwnedMediaFrameSource.h"
#include <condition_variable>

namespace {
struct DecodeGate {
  std::mutex mutex;
  std::condition_variable changed;
  bool released = false;
  std::atomic<int> blocked{0}, audioReads{0}, destroyed{0};
};
class TestDecoder final : public IMediaFrameSource {
 public:
  explicit TestDecoder(std::shared_ptr<DecodeGate> gate) : gate_(std::move(gate)) {}
  ~TestDecoder() override { ++gate_->destroyed; }
  std::vector<VideoFrame> pollMediaFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t) override {
    if (layers.front().mediaPlaybackKey == "blocked") {
      ++gate_->blocked;
      std::unique_lock<std::mutex> lock(gate_->mutex);
      gate_->changed.wait(lock, [&] { return gate_->released; });
    }
    VideoFrame frame;
    frame.participantId = layers.front().sourceId;
    frame.width = frame.pixelWidth = frame.height = frame.pixelHeight = 1;
    frame.pixelStride = 4; frame.frameId = layers.front().mediaPlaybackKey == "blocked" ? 1 : 2;
    frame.pixels = std::make_shared<std::vector<uint8_t>>(4, 255);
    return {frame};
  }
  std::vector<AudioFrame> pollMediaAudioFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t) override {
    ++gate_->audioReads;
    AudioFrame frame; frame.participantId = layers.front().sourceId;
    frame.sampleRate = 48000; frame.channels = 2; frame.sampleCount = 960; frame.pcm.resize(1920, 0.5f);
    return {frame};
  }
  std::vector<std::string> warnings() const override { return {}; }
 private:
  std::shared_ptr<DecodeGate> gate_;
};
CompositorRenderPlanLayer workerLayer() {
  CompositorRenderPlanLayer layer;
  layer.kind = "media-video"; layer.mediaAssetId = "test"; layer.sourceId = "media:test";
  layer.mediaAssetPath = "test.wav"; layer.mediaAssetPlaying = true;
  return layer;
}
}
TEST(OwnedMediaFrameSource, SlowRetiredGenerationCannotBlockOrOverwriteNewPlayback) {
  auto gate = std::make_shared<DecodeGate>();
  OwnedMediaFrameSource source([gate] { return std::make_unique<TestDecoder>(gate); });
  struct ReleaseGate {
    std::shared_ptr<DecodeGate> gate;
    ~ReleaseGate() { { std::lock_guard<std::mutex> lock(gate->mutex); gate->released = true; } gate->changed.notify_all(); }
  } release{gate};
  auto layer = workerLayer(); layer.mediaPlaybackKey = "blocked";
  EXPECT_TRUE(source.pollMediaFrames({layer}, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()).empty());
  const auto blockedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!gate->blocked.load() && std::chrono::steady_clock::now() < blockedDeadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_TRUE(gate->blocked.load() > 0);
  layer.mediaPlaybackKey = "replacement";
  std::vector<VideoFrame> frames;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (frames.empty() && std::chrono::steady_clock::now() < deadline) {
    frames = source.pollMediaFrames({layer}, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(!frames.empty());
  if (!frames.empty()) EXPECT_EQ(frames.front().frameId, 2);
  { std::lock_guard<std::mutex> lock(gate->mutex); gate->released = true; } gate->changed.notify_all();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  frames = source.pollMediaFrames({layer}, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  ASSERT_TRUE(!frames.empty());
  if (!frames.empty()) EXPECT_EQ(frames.front().frameId, 2);
}
TEST(OwnedMediaFrameSource, AudioPrefetchIsBoundedAndDecoderStopsOnDestruction) {
  auto gate = std::make_shared<DecodeGate>();
  {
    OwnedMediaFrameSource source([gate] { return std::make_unique<TestDecoder>(gate); });
    auto layer = workerLayer();
    EXPECT_TRUE(source.pollMediaAudioFrames({layer}, 0).empty());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (gate->audioReads.load() < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(gate->audioReads.load(), 2);
    const auto frames = source.pollMediaAudioFrames({layer}, 20);
    ASSERT_TRUE(!frames.empty());
    if (!frames.empty()) EXPECT_EQ(frames.front().sampleCount, 960);
  }
  EXPECT_EQ(gate->destroyed.load(), 1);
}

TEST(OwnedMediaFrameSource, TheSameRequestFromTwoBusesStartsOneDecoder) {
  auto gate = std::make_shared<DecodeGate>();
  std::atomic<int> created{0};
  OwnedMediaFrameSource source([gate, &created] { ++created; return std::make_unique<TestDecoder>(gate); });
  auto program = workerLayer();      // sourceId media:test, playing, same path/key
  auto preview = workerLayer();
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  (void)source.pollMediaFrames({program, preview}, now);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (created.load() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(created.load(), 1);
  const auto warnings = source.warnings();
  EXPECT_TRUE(std::none_of(warnings.begin(), warnings.end(), [](const std::string& w) {
    return w.find("two different playback identities") != std::string::npos;
  }));
}

// A still routed on both buses arrives playing on Program and paused on
// Preview (the shell only rolls Program). Stills are served by
// StillMediaFrameCache, not by a decoder — the owned source must not start
// two dead decoder threads for them nor call the pair a collision.
TEST(OwnedMediaFrameSource, AStillRouteOnBothBusesStartsNoDecoderAndIsNotACollision) {
  auto gate = std::make_shared<DecodeGate>();
  std::atomic<int> created{0};
  OwnedMediaFrameSource source([gate, &created] { ++created; return std::make_unique<TestDecoder>(gate); });
  auto program = workerLayer();
  program.mediaAssetId = "logo"; program.sourceId = "media:logo";
  program.mediaAssetKind = "lower-third"; program.mediaAssetPath = "C:\\assets\\logo.PNG";
  program.mediaAssetPlaying = true;
  auto preview = program;
  preview.mediaAssetPlaying = false;
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  (void)source.pollMediaFrames({program, preview}, now);
  (void)source.pollMediaAudioFrames({program, preview}, now);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(created.load(), 0);
  const auto warnings = source.warnings();
  EXPECT_TRUE(std::none_of(warnings.begin(), warnings.end(), [](const std::string& w) {
    return w.find("two different playback identities") != std::string::npos;
  }));
}

TEST(OwnedMediaFrameSource, TheCapWarningNamesTheAssetItRefused) {
  auto gate = std::make_shared<DecodeGate>();
  OwnedMediaFrameSource source([gate] { return std::make_unique<TestDecoder>(gate); });
  std::vector<CompositorRenderPlanLayer> layers;
  // Zero-padded ids so the request map's lexicographic key order (it is a
  // std::map<std::string, ...>) matches admission order 0..16 — an
  // unpadded "asset-16" would sort ahead of "asset-9" and never be the one
  // refused, which is not the property this test is pinning.
  for (int i = 0; i < 17; ++i) {
    auto layer = workerLayer();
    char id[16]; std::snprintf(id, sizeof(id), "asset-%02d", i);
    layer.mediaAssetId = id;
    layer.sourceId = "media:" + layer.mediaAssetId;
    layers.push_back(layer);
  }
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  (void)source.pollMediaFrames(layers, now);
  // The manager admits asynchronously; poll to a generous deadline instead
  // of a fixed sleep so a busy box cannot flake this.
  const auto isNamed = [](const std::vector<std::string>& warnings) {
    return std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) {
      return w.find("Media decoder capacity reached") != std::string::npos && w.find("media:asset-16") != std::string::npos;
    });
  };
  bool named = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!(named = isNamed(source.warnings())) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_TRUE(named);
}

TEST(OwnedMediaFrameSource, TwoPlaybackIdentitiesForOneSourceIdAreLoud) {
  auto gate = std::make_shared<DecodeGate>();
  OwnedMediaFrameSource source([gate] { return std::make_unique<TestDecoder>(gate); });
  auto playing = workerLayer();
  auto paused = workerLayer();
  paused.mediaAssetPlaying = false;
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  (void)source.pollMediaFrames({playing, paused}, now);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const auto warnings = source.warnings();
  const bool found = std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) {
    return w.find("two different playback identities") != std::string::npos && w.find("media:test") != std::string::npos;
  });
  EXPECT_TRUE(found);
}

TEST(MediaAudioDemandClock, JitterKeepsAnchorAndInterruptedPollSkipsExpiredWindows) {
  MediaAudioDemandClock clock(100);
  EXPECT_EQ(*clock.takeDue(100), 100);
  EXPECT_FALSE(clock.takeDue(119).has_value());
  EXPECT_EQ(*clock.takeDue(121), 120);
  EXPECT_EQ(*clock.takeDue(142), 140);
  EXPECT_EQ(clock.skipped(), 0u);
  EXPECT_EQ(*clock.takeDue(241), 240);
  EXPECT_EQ(clock.skipped(), 4u);
  EXPECT_EQ(clock.nextTimeMs(), 260);
  EXPECT_FALSE(clock.takeDue(242).has_value());
}

TEST(MediaAudioWindows, LoopBoundaryWindowStartsNextReaderAtZeroAndKeepsBothHalves) {
  // A clip ends10ms into a20ms window. Its next loop contributes the rest;
  // the reader's seek must be zero even though this window began earlier.
  EXPECT_EQ(mediaLoopSeek100ns(200000, 300000), 0);
  EXPECT_EQ(mediaLoopSeek100ns(400000, 300000), 100000);
  MediaAudioWindows audio(48000, 1);
  audio.seek(960);
  audio.append(200000, std::vector<float>(480, 0.25f));
  audio.append(300000, std::vector<float>(480, 0.75f));
  const auto window = audio.take(960);
  EXPECT_EQ(window[0], 0.25f); EXPECT_EQ(window[479], 0.25f);
  EXPECT_EQ(window[480], 0.75f); EXPECT_EQ(window[959], 0.75f);
}

TEST(MediaVideoPresentation, RenderSelectsPreparedFrameAtDeadlineWithoutWorkerWake) {
  MediaVideoPresentation queue;
  const auto sample = [](int64_t id, int64_t due) {
    VideoFrame frame; frame.frameId = id;
    frame.width = frame.pixelWidth = frame.height = frame.pixelHeight = 1; frame.pixelStride = 4;
    frame.pixels = std::make_shared<std::vector<uint8_t>>(4, static_cast<uint8_t>(id));
    return ScheduledMediaVideo{frame, due};
  };
  queue.push(sample(1, 1000000)); queue.push(sample(2, 1166667)); queue.push(sample(3, 1333334));
  EXPECT_FALSE(queue.hasRoom());
  EXPECT_FALSE(queue.select(999999).hasPixels());
  EXPECT_EQ(queue.select(1000000).frameId, 1);
  EXPECT_EQ(queue.select(1166666).frameId, 1);
  // No worker call/publish happens between these render selections. The
  // already prepared next image becomes visible on its exact scheduled tick.
  EXPECT_EQ(queue.select(1166667).frameId, 2);
  EXPECT_EQ(queue.select(1400000).frameId, 3);
  EXPECT_EQ(queue.queued(), 0u);
  EXPECT_TRUE(queue.hasRoom());
}
