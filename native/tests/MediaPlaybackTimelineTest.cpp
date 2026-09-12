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
// Rewritten for T1.2 (was RestartAndPauseResetSharedGeneration, which pinned
// the retired contract "a pause resets the clock"). A new IDENTITY (a new
// go-live generation) still resets; a pause is a clock state and does not.
TEST(MediaPlaybackTimeline, RestartResetsSharedGenerationButPauseDoesNot) {
  MediaPlaybackTimeline clock;
  EXPECT_TRUE(clock.configure("clip:play1", true, 100));
  const auto generation = clock.generation();
  EXPECT_FALSE(clock.configure("clip:play1", true, 10000000));
  EXPECT_EQ(clock.generation(), generation);
  EXPECT_FALSE(clock.configure("clip:play1", false, 10000000));
  EXPECT_EQ(clock.generation(), generation);
  EXPECT_TRUE(clock.paused());
  EXPECT_EQ(clock.elapsed100ns(20000000), 10000000 - 100);
  EXPECT_TRUE(clock.configure("clip:play2", true, 20000000));
  EXPECT_EQ(clock.generation(), generation + 1);
  EXPECT_FALSE(clock.paused());
  EXPECT_EQ(clock.elapsed100ns(20000000), 0);
}
TEST(MediaPlaybackTimeline, PauseFreezesElapsedAndResumeContinues) {
  MediaPlaybackTimeline clock;
  clock.configure("clip:live:1", true, 0);
  const auto generation = clock.generation();
  EXPECT_EQ(clock.elapsed100ns(10'000'000), 10'000'000);
  EXPECT_FALSE(clock.configure("clip:live:1", false, 10'000'000));
  EXPECT_TRUE(clock.paused());
  EXPECT_EQ(clock.elapsed100ns(50'000'000), 10'000'000); // Frozen while paused.
  EXPECT_FALSE(clock.configure("clip:live:1", false, 30'000'000)); // Repeat pause keeps the first instant.
  EXPECT_EQ(clock.elapsed100ns(50'000'000), 10'000'000);
  EXPECT_FALSE(clock.configure("clip:live:1", true, 50'000'000));
  EXPECT_FALSE(clock.paused());
  EXPECT_EQ(clock.elapsed100ns(50'000'000), 10'000'000); // No jump to 0, no skip of the pause.
  EXPECT_EQ(clock.elapsed100ns(60'000'000), 20'000'000);
  EXPECT_EQ(clock.generation(), generation);
  // Video due-times follow the resumed clock: the frame at media 1.5 s is due
  // 0.5 s after resume, not 4 s earlier (which a stale epoch would claim).
  EXPECT_FALSE(clock.videoDue(15'000'000, 54'999'999));
  EXPECT_TRUE(clock.videoDue(15'000'000, 55'000'000));
}
TEST(MediaPlaybackTimeline, ANewIdentityStillResets) {
  MediaPlaybackTimeline clock;
  clock.configure("clip:live:1", true, 0);
  clock.configure("clip:live:1", false, 10'000'000);
  const auto generation = clock.generation();
  EXPECT_TRUE(clock.configure("clip:live:2", true, 40'000'000));
  EXPECT_EQ(clock.generation(), generation + 1);
  EXPECT_FALSE(clock.paused());
  EXPECT_EQ(clock.elapsed100ns(40'000'000), 0);
  EXPECT_EQ(clock.elapsed100ns(45'000'000), 5'000'000);
  // A new identity that starts paused (a cue poster) sits at 0 until played.
  EXPECT_TRUE(clock.configure("clip:cue", false, 50'000'000));
  EXPECT_EQ(clock.elapsed100ns(90'000'000), 0);
  clock.configure("clip:cue", true, 90'000'000);
  EXPECT_EQ(clock.elapsed100ns(91'000'000), 1'000'000);
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

// Playing vs paused is no longer an identity (T1.2: pause is a clock state),
// so this uses a genuinely different identity for one source id: two
// different go-live playback keys.
TEST(OwnedMediaFrameSource, TwoPlaybackIdentitiesForOneSourceIdAreLoud) {
  auto gate = std::make_shared<DecodeGate>();
  OwnedMediaFrameSource source([gate] { return std::make_unique<TestDecoder>(gate); });
  auto first = workerLayer();
  first.mediaPlaybackKey = "media:test:live:1";
  auto second = workerLayer();
  second.mediaPlaybackKey = "media:test:live:2";
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  (void)source.pollMediaFrames({first, second}, now);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const auto warnings = source.warnings();
  const bool found = std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) {
    return w.find("two different playback identities") != std::string::npos && w.find("media:test") != std::string::npos;
  });
  EXPECT_TRUE(found);
}

namespace {
// A decoder that knows nothing about pause: every video poll yields a new,
// increasing frameId and every audio poll a non-silent window. Anything that
// holds or silences a paused clip has to be the owned source's doing.
class CountingDecoder final : public IMediaFrameSource {
 public:
  std::vector<VideoFrame> pollMediaFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t) override {
    VideoFrame frame;
    frame.participantId = layers.front().sourceId;
    frame.width = frame.pixelWidth = frame.height = frame.pixelHeight = 1;
    frame.pixelStride = 4; frame.frameId = ++frameId_;
    frame.pixels = std::make_shared<std::vector<uint8_t>>(4, 255);
    return {frame};
  }
  std::vector<AudioFrame> pollMediaAudioFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t) override {
    AudioFrame frame; frame.participantId = layers.front().sourceId;
    frame.sampleRate = 48000; frame.channels = 2; frame.sampleCount = 960; frame.pcm.resize(1920, 0.5f);
    return {frame};
  }
  std::vector<std::string> warnings() const override { return {}; }
 private:
  int64_t frameId_ = 0;
};
int64_t steadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Polls the video path until a frame for the layer arrives; returns its id or -1.
int64_t pollUntilFrame(OwnedMediaFrameSource& source, const CompositorRenderPlanLayer& layer, int64_t greaterThan = 0) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto frames = source.pollMediaFrames({layer}, steadyNowMs());
    if (!frames.empty() && frames.front().frameId > greaterThan) return frames.front().frameId;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return -1;
}
bool hasNonSilentPcm(const std::vector<AudioFrame>& frames) {
  for (const auto& frame : frames)
    for (const auto sample : frame.pcm) if (sample != 0.f) return true;
  return false;
}
}

TEST(OwnedMediaFrameSource, PauseAndResumeKeepOneDecoder) {
  std::atomic<int> created{0};
  OwnedMediaFrameSource source([&created] { ++created; return std::make_unique<CountingDecoder>(); });
  auto layer = workerLayer();
  layer.mediaPlaybackKey = "media:test:live:1";
  ASSERT_TRUE(pollUntilFrame(source, layer) > 0);
  EXPECT_EQ(created.load(), 1);
  layer.mediaAssetPlaying = false;
  const auto pauseEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (std::chrono::steady_clock::now() < pauseEnd) {
    (void)source.pollMediaFrames({layer}, steadyNowMs());
    (void)source.pollMediaAudioFrames({layer}, steadyNowMs());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(created.load(), 1);
  layer.mediaAssetPlaying = true;
  ASSERT_TRUE(pollUntilFrame(source, layer) > 0);
  // Give a would-be replacement worker the chance to start before counting.
  const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  while (std::chrono::steady_clock::now() < settle) {
    (void)source.pollMediaFrames({layer}, steadyNowMs());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(created.load(), 1);
}

TEST(OwnedMediaFrameSource, PauseHoldsTheOnAirFrame) {
  OwnedMediaFrameSource source([] { return std::make_unique<CountingDecoder>(); });
  auto layer = workerLayer();
  layer.mediaPlaybackKey = "media:test:live:1";
  int64_t held = pollUntilFrame(source, layer);
  ASSERT_TRUE(held > 0);
  held = pollUntilFrame(source, layer, held + 2); // Let it roll a few frames.
  ASSERT_TRUE(held > 0);
  layer.mediaAssetPlaying = false;
  // At least 250 ms AND at least 20 polls (Windows' 15.6 ms default timer can
  // stretch each 2 ms sleep), within a generous 3 s ceiling.
  int polls = 0;
  const auto start = std::chrono::steady_clock::now();
  const auto minEnd = start + std::chrono::milliseconds(250);
  const auto ceiling = start + std::chrono::seconds(3);
  while ((std::chrono::steady_clock::now() < minEnd || polls < 20) && std::chrono::steady_clock::now() < ceiling) {
    const auto frames = source.pollMediaFrames({layer}, steadyNowMs());
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames.front().frameId, held);
    ++polls;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(polls >= 20);
  layer.mediaAssetPlaying = true;
  EXPECT_TRUE(pollUntilFrame(source, layer, held) > held);
}

TEST(OwnedMediaFrameSource, NoAudioWhilePausedAndAudioResumes) {
  std::atomic<int> created{0};
  OwnedMediaFrameSource source([&created] { ++created; return std::make_unique<CountingDecoder>(); });
  auto layer = workerLayer();
  layer.mediaPlaybackKey = "media:test:live:1";
  bool sawAudio = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!sawAudio && std::chrono::steady_clock::now() < deadline) {
    (void)source.pollMediaFrames({layer}, steadyNowMs());
    sawAudio = hasNonSilentPcm(source.pollMediaAudioFrames({layer}, steadyNowMs()));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(sawAudio);
  layer.mediaAssetPlaying = false;
  const auto pauseEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (std::chrono::steady_clock::now() < pauseEnd) {
    (void)source.pollMediaFrames({layer}, steadyNowMs());
    EXPECT_TRUE(source.pollMediaAudioFrames({layer}, steadyNowMs()).empty()); // Not even silence.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  layer.mediaAssetPlaying = true;
  sawAudio = false;
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!sawAudio && std::chrono::steady_clock::now() < deadline) {
    (void)source.pollMediaFrames({layer}, steadyNowMs());
    sawAudio = hasNonSilentPcm(source.pollMediaAudioFrames({layer}, steadyNowMs()));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(sawAudio);
  EXPECT_EQ(created.load(), 1);
}

// Now that playing/paused share one request key, a paused copy of the same
// source (same key) in the same poll must not pause Program's roll. Program's
// layers come first in the media poll, and the first request for a key wins.
TEST(OwnedMediaFrameSource, APausedCopyOfTheSameSourceCannotPauseTheProgramRoll) {
  OwnedMediaFrameSource source([] { return std::make_unique<CountingDecoder>(); });
  auto program = workerLayer();
  program.mediaPlaybackKey = "media:test:live:1";
  auto copy = program;
  copy.mediaAssetPlaying = false;
  int64_t first = -1, latest = -1;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline && (first < 0 || latest <= first + 3)) {
    const auto frames = source.pollMediaFrames({program, copy}, steadyNowMs());
    if (!frames.empty()) { if (first < 0) first = frames.front().frameId; latest = frames.front().frameId; }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(first > 0);
  EXPECT_TRUE(latest > first + 3);
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

// A paused clip holds the frame on air; frames already prepared behind it are
// kept and re-timed by the paused duration on resume, so the next image is
// the next frame of the clip, due exactly as late as the pause lasted.
TEST(MediaVideoPresentation, HoldKeepsTheOnAirFrameAndShiftRetimesPreparedFrames) {
  MediaVideoPresentation queue;
  const auto sample = [](int64_t id, int64_t due) {
    VideoFrame frame; frame.frameId = id;
    frame.width = frame.pixelWidth = frame.height = frame.pixelHeight = 1; frame.pixelStride = 4;
    frame.pixels = std::make_shared<std::vector<uint8_t>>(4, static_cast<uint8_t>(id));
    return ScheduledMediaVideo{frame, due};
  };
  // An empty presentation's hold shows the first prepared frame (a poster).
  queue.push(sample(1, 5'000'000));
  EXPECT_EQ(queue.hold().frameId, 1);
  queue.push(sample(2, 1'333'333)); queue.push(sample(3, 1'666'666));
  EXPECT_EQ(queue.hold().frameId, 1); // Held regardless of the queued due times.
  EXPECT_EQ(queue.current().frameId, 1);
  EXPECT_EQ(queue.queued(), 2u);
  queue.shift(10'000'000);            // Paused for one second.
  EXPECT_EQ(queue.select(11'333'332).frameId, 1);
  EXPECT_EQ(queue.select(11'333'333).frameId, 2);
  EXPECT_EQ(queue.select(11'666'666).frameId, 3);
}

// Samples decoded ahead of the clock but never heard (dropped when the clip
// paused) must replay when the clock seeks back to the paused position,
// instead of turning into silence and skipping that media.
TEST(MediaAudioWindows, ABackwardSeekWithinRecentHistoryReplaysDecodedSamples) {
  MediaAudioWindows audio(48000, 1);
  for (int packet = 0; packet < 5; ++packet) { // 20 ms decoder packets.
    std::vector<float> pcm(960);
    for (int n = 0; n < 960; ++n) pcm[n] = static_cast<float>(packet * 960 + n + 1);
    audio.append(static_cast<int64_t>(packet) * 200000, std::move(pcm));
  }
  (void)audio.take(960); (void)audio.take(960); (void)audio.take(960); // Cursor 2880; packets 0-2 consumed.
  audio.seek(1920);                                                    // Back 20 ms.
  const auto replay = audio.take(960);
  EXPECT_EQ(replay.front(), 1921.f);
  EXPECT_EQ(replay.back(), 2880.f);
  EXPECT_EQ(audio.take(960).front(), 2881.f);
  // History is bounded: a seek far behind it is silent, never stale.
  MediaAudioWindows longClip(48000, 1);
  for (int second = 0; second < 3; ++second)
    longClip.append(static_cast<int64_t>(second) * 10'000'000, std::vector<float>(48000, 1.f + second));
  longClip.seek(48000 * 2 + 24000);
  EXPECT_EQ(longClip.take(960).front(), 3.f);
  longClip.seek(0);
  EXPECT_EQ(longClip.take(960).front(), 0.f);
}

// T1.11 step 2: a clip cued in Preview hands its WARM decoder to Program.
// Two things change on go-live — the `preview:` source-id namespace collapses
// and MediaGoLiveLedger advances the generation baked into the playback key —
// so the arriving request used to match no entry, a cold decoder opened, and
// Program painted colorFromParticipantId for the ticks before its first frame
// (the "placeholder flash", #449). The cue poster sits paused at frame 0, so
// resuming IT is exactly the "roll from 0" the go-live contract asks for.
TEST(OwnedMediaFrameSource, ACuedClipHandsItsWarmDecoderToProgram) {
  std::atomic<int> created{0};
  OwnedMediaFrameSource source([&created] { ++created; return std::make_unique<CountingDecoder>(); });
  auto cue = workerLayer();
  cue.sourceId = "preview:media:test";
  cue.mediaPlaybackKey = "media:test:live:1";
  cue.mediaAssetPlaying = false;
  ASSERT_TRUE(pollUntilFrame(source, cue) > 0) << "the cue poster never warmed";
  EXPECT_EQ(created.load(), 1);

  // The Take: same asset, same file, live namespace, generation +1, playing.
  auto live = workerLayer();
  live.sourceId = "media:test";
  live.mediaPlaybackKey = "media:test:live:2";
  live.mediaAssetPlaying = true;
  ASSERT_TRUE(pollUntilFrame(source, live) > 0);
  // Give a would-be replacement worker the chance to start before counting.
  const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  while (std::chrono::steady_clock::now() < settle) {
    (void)source.pollMediaFrames({live}, steadyNowMs());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(created.load(), 1) << "the take opened a second decoder instead of adopting the warm cue";
}

// The refusal that keeps the go-live contract honest. A cue that has already
// ROLLED is at an arbitrary position; adopting it would put the clip on air
// mid-roll while the take record still read `cut`. It must cold-start.
TEST(OwnedMediaFrameSource, ACueThatAlreadyRolledIsNeverHandedOver) {
  std::atomic<int> created{0};
  OwnedMediaFrameSource source([&created] { ++created; return std::make_unique<CountingDecoder>(); });
  auto cue = workerLayer();
  cue.sourceId = "preview:media:test";
  cue.mediaPlaybackKey = "media:test:live:1";
  cue.mediaAssetPlaying = true;  // auditioning in Preview: it has rolled
  ASSERT_TRUE(pollUntilFrame(source, cue) > 0);
  EXPECT_EQ(created.load(), 1);

  auto live = workerLayer();
  live.sourceId = "media:test";
  live.mediaPlaybackKey = "media:test:live:2";
  live.mediaAssetPlaying = true;
  ASSERT_TRUE(pollUntilFrame(source, live) > 0);
  const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  while (std::chrono::steady_clock::now() < settle) {
    (void)source.pollMediaFrames({live}, steadyNowMs());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(created.load(), 2) << "a rolled cue was adopted; the clip would go on air mid-roll";
}

namespace {
// A prefetching decoder, like the real MF adapter — which is what makes the
// SCHEDULE visible. While the clip is paused it prepares frames due at a far
// future instant (the cue's paused epoch); once playing it prepares frames due
// now, on a clock that RESETS whenever the playback key changes. So if a
// hand-over keeps the cue's queued frames, `MediaVideoPresentation::select`
// finds nothing due and Program sits on the poster forever.
class CueSchedulingDecoder final : public IMediaFrameSource, public IMediaVideoPrefetch {
 public:
  std::vector<VideoFrame> pollMediaFrames(const std::vector<CompositorRenderPlanLayer>&, int64_t) override { return {}; }
  std::vector<AudioFrame> pollMediaAudioFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t) override {
    AudioFrame frame; frame.participantId = layers.front().sourceId;
    frame.sampleRate = 48000; frame.channels = 2; frame.sampleCount = 960; frame.pcm.resize(1920, 0.5f);
    return {frame};
  }
  std::vector<std::string> warnings() const override { return {}; }
  std::vector<ScheduledMediaVideo> prefetchMediaVideo(
      const std::vector<CompositorRenderPlanLayer>& layers, int64_t nowMs) override {
    const auto& layer = layers.front();
    VideoFrame frame;
    frame.participantId = layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId;
    frame.width = frame.pixelWidth = frame.height = frame.pixelHeight = 1;
    frame.pixelStride = 4; frame.frameId = ++frameId_;
    frame.pixels = std::make_shared<std::vector<uint8_t>>(4, 255);
    // Paused: due an hour out, exactly like a frame scheduled against an epoch
    // that is not running. Playing: due now.
    const int64_t due = layer.mediaAssetPlaying ? nowMs * 10000 : (nowMs + 3600'000) * 10000;
    return {ScheduledMediaVideo{frame, due}};
  }
  void syncMediaClock(const std::vector<CompositorRenderPlanLayer>&, int64_t) override {}
 private:
  int64_t frameId_ = 0;
};
}  // namespace

// The hand-over must leave the clip ROLLING, not frozen on its poster. The
// adopted decoder's queued frames were scheduled against the cue's paused
// epoch; the go-live clock is a new epoch, so they can never come due. They are
// dropped on adoption and the decoder refills from the running clock — the
// poster stays on air meanwhile, which is what removes the placeholder.
TEST(OwnedMediaFrameSource, AnAdoptedCueRollsInsteadOfFreezingOnItsPoster) {
  OwnedMediaFrameSource source([] { return std::make_unique<CueSchedulingDecoder>(); });
  auto cue = workerLayer();
  cue.sourceId = "preview:media:test";
  cue.mediaPlaybackKey = "media:test:live:1";
  cue.mediaAssetPlaying = false;
  const int64_t poster = pollUntilFrame(source, cue);
  ASSERT_TRUE(poster > 0) << "the cue poster never warmed";
  // Let the cue queue up more far-future frames behind the poster.
  const auto warm = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
  while (std::chrono::steady_clock::now() < warm) {
    (void)source.pollMediaFrames({cue}, steadyNowMs());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  auto live = workerLayer();
  live.sourceId = "media:test";
  live.mediaPlaybackKey = "media:test:live:2";
  live.mediaAssetPlaying = true;
  EXPECT_TRUE(pollUntilFrame(source, live, poster) > poster)
      << "the adopted decoder never advanced past its poster";
}

// Go-live is "roll from 0, AUDIO ON". The cue was paused, so its entry carried
// wantsAudio=false and an audio clock anchored to a run that never happened;
// the hand-over has to arm the audio side as a fresh start, or the adoption
// buys picture continuity by silencing the clip.
TEST(OwnedMediaFrameSource, AnAdoptedCueTurnsItsAudioOn) {
  OwnedMediaFrameSource source([] { return std::make_unique<CountingDecoder>(); });
  auto cue = workerLayer();
  cue.sourceId = "preview:media:test";
  cue.mediaPlaybackKey = "media:test:live:1";
  cue.mediaAssetPlaying = false;
  ASSERT_TRUE(pollUntilFrame(source, cue) > 0);
  EXPECT_FALSE(hasNonSilentPcm(source.pollMediaAudioFrames({cue}, steadyNowMs())))
      << "a paused cue emitted audio";

  auto live = workerLayer();
  live.sourceId = "media:test";
  live.mediaPlaybackKey = "media:test:live:2";
  live.mediaAssetPlaying = true;
  bool heard = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!heard && std::chrono::steady_clock::now() < deadline) {
    (void)source.pollMediaFrames({live}, steadyNowMs());
    heard = hasNonSilentPcm(source.pollMediaAudioFrames({live}, steadyNowMs()));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(heard) << "the adopted clip went to Program silent";
}
