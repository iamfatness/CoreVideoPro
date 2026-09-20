#include "MediaTestSupport.h"
#include "modules/Interfaces.h"
#include "modules/MediaPlaybackTimeline.h"
#include "modules/MediaVideoPresentation.h"
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
    if (layers.front().mediaAssetId == "blocked") {
      ++gate_->blocked;
      std::unique_lock<std::mutex> lock(gate_->mutex);
      gate_->changed.wait(lock, [&] { return gate_->released; });
    }
    VideoFrame frame;
    frame.participantId = layers.front().sourceId;
    frame.width = frame.pixelWidth = frame.height = frame.pixelHeight = 1;
    frame.pixelStride = 4; frame.frameId = layers.front().mediaAssetId == "blocked" ? 1 : 2;
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
}


namespace {
// The one copy lives in MediaTestSupport.h (#535 slice 3b Task 5) so this
// file and the MediaCore behaviour tests cannot drift apart.
using corevideo::testing::CountingDecoder;
int64_t steadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool hasNonSilentPcm(const std::vector<AudioFrame>& frames) {
  for (const auto& frame : frames)
    for (const auto sample : frame.pcm) if (sample != 0.f) return true;
  return false;
}
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


// ---------------------------------------------------------------------------
// core::MediaTransports (#535 slice 3b). The media decoder owner, driven by a
// command-time DESIRED SET instead of the render plan's layers. Task 3 swapped
// MediaCore onto it and deleted OwnedMediaFrameSource outright.
// ---------------------------------------------------------------------------
#include "core/MediaTransports.h"

using corevideo::core::MediaOperatorAction;
using corevideo::core::MediaTransportDesired;
using corevideo::core::MediaTransports;
using corevideo::core::MediaTransportState;

namespace {
MediaTransportDesired testDesired(bool onProgram, bool onPreview, bool loop = false) {
  MediaTransportDesired d;
  d.sourceId = loop ? "background:test" : "media:test";
  d.assetId = "test";
  d.path = "test.wav";
  d.kind = loop ? "background" : "video";
  d.loop = loop;
  d.onProgram = onProgram;
  d.onPreview = onPreview;
  return d;
}
// A bounded wait driving the static per-entry selector the bus source will call each tick.
int64_t pollUntilFrame(MediaTransports::Entry& entry, int64_t greaterThan = 0) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto frame = MediaTransports::selectVideo(entry, steadyNowMs() * 10000);
    if (frame && frame->frameId > greaterThan) return frame->frameId;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return -1;
}
// The playback identity no longer carries a playback key, so a retired
// generation is separated by its PATH (a Reopen) - which is exactly the
// transition this test now covers.
class PathGatedDecoder final : public IMediaFrameSource {
 public:
  explicit PathGatedDecoder(std::shared_ptr<DecodeGate> gate) : gate_(std::move(gate)) {}
  ~PathGatedDecoder() override { ++gate_->destroyed; }
  std::vector<VideoFrame> pollMediaFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t) override {
    const bool blocked = layers.front().mediaAssetPath == "blocked.wav";
    if (blocked) {
      ++gate_->blocked;
      std::unique_lock<std::mutex> lock(gate_->mutex);
      gate_->changed.wait(lock, [&] { return gate_->released; });
    }
    VideoFrame frame;
    frame.participantId = layers.front().sourceId;
    frame.width = frame.pixelWidth = frame.height = frame.pixelHeight = 1;
    frame.pixelStride = 4; frame.frameId = blocked ? 1 : 2;
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
}  // namespace

TEST(MediaTransports, ACuedClipEnteringProgramRollsTheSameDecoderWithAudio) {
  std::atomic<int> created{0};
  MediaTransports t([&created] { ++created; return std::make_unique<CountingDecoder>(); });
  auto ch = t.apply({testDesired(false, true)}, 0);
  ASSERT_EQ(ch.size(), 1u);
  if (ch.empty()) return;
  auto e = ch.front().entry;
  const auto poster = pollUntilFrame(*e);
  ASSERT_GT(poster, 0);
  // Cued: holds the poster (frameId does not advance) and emits no audio.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto held = MediaTransports::selectVideo(*e, steadyNowMs() * 10000);
  ASSERT_TRUE(held.has_value());
  if (held) EXPECT_EQ(held->frameId, poster);
  EXPECT_TRUE(t.popAudio(steadyNowMs()).empty());
  EXPECT_TRUE(t.apply({testDesired(true, true)}, 0).empty());  // membership unchanged: no Change rows
  ASSERT_GT(pollUntilFrame(*e, poster), 0);                    // rolls
  EXPECT_EQ(created.load(), 1);                                // the SAME decoder
  bool audio = false;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!audio && std::chrono::steady_clock::now() < until) {
    audio = hasNonSilentPcm(t.popAudio(steadyNowMs()));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(audio);
}

TEST(MediaTransports, LeavingProgramWhileCuedRestartsBehindTheHeldFrame) {
  std::atomic<int> created{0};
  MediaTransports t([&created] { ++created; return std::make_unique<CountingDecoder>(); });
  auto opened = t.apply({testDesired(true, true)}, 0);
  ASSERT_EQ(opened.size(), 1u);
  if (opened.empty()) return;
  auto e = opened.front().entry;
  auto id = pollUntilFrame(*e);
  id = pollUntilFrame(*e, id + 3);
  ASSERT_GT(id, 0);
  EXPECT_TRUE(t.apply({testDesired(false, true)}, 0).empty());
  // The very next select still returns a frame (the held one), never nothing.
  ASSERT_TRUE(MediaTransports::selectVideo(*e, steadyNowMs() * 10000).has_value());
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool restarted = false;
  while (!restarted && std::chrono::steady_clock::now() < until) {
    restarted = created.load() == 2;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(restarted);
  const auto status = t.snapshot();
  ASSERT_EQ(status.size(), 1u);
  if (!status.empty()) EXPECT_TRUE(status.front().state == MediaTransportState::Cued);
}

TEST(MediaTransports, ALoopNeverPausesAndNeverRestartsAcrossATake) {
  std::atomic<int> created{0};
  MediaTransports t([&created] { ++created; return std::make_unique<CountingDecoder>(); });
  auto opened = t.apply({testDesired(false, true, true)}, 0);
  ASSERT_EQ(opened.size(), 1u);
  if (opened.empty()) return;
  auto e = opened.front().entry;
  auto id = pollUntilFrame(*e);
  ASSERT_GT(id, 0);
  std::string reason;
  EXPECT_FALSE(t.operatorAction("test", MediaOperatorAction::Pause, reason));
  EXPECT_TRUE(!reason.empty());
  EXPECT_TRUE(t.apply({testDesired(true, false, true)}, 0).empty());
  ASSERT_GT(pollUntilFrame(*e, id), 0);
  EXPECT_EQ(created.load(), 1);
}

TEST(MediaTransports, AnIdenticalApplyIsANoOp) {
  std::atomic<int> created{0};
  MediaTransports t([&created] { ++created; return std::make_unique<CountingDecoder>(); });
  auto opened = t.apply({testDesired(true, false)}, 0);
  ASSERT_EQ(opened.size(), 1u);
  if (opened.empty()) return;
  auto e = opened.front().entry;
  auto id = pollUntilFrame(*e);
  ASSERT_GT(id, 0);
  for (int i = 0; i < 5; ++i) EXPECT_TRUE(t.apply({testDesired(true, false)}, 0).empty());
  ASSERT_GT(pollUntilFrame(*e, id), 0);
  EXPECT_EQ(created.load(), 1);
}

// Ported from the retired OwnedMediaFrameSource.SlowRetiredGenerationCannotBlockOrOverwrite
// NewPlayback. The replacement is now a Reopen (the path changed), which is a
// remove Change followed by an add Change: two SEPARATE entries, so the blocked
// generation cannot reach the new presentation even in principle.
TEST(MediaTransports, ASlowRetiredGenerationCannotBlockOrOverwriteNewPlayback) {
  auto gate = std::make_shared<DecodeGate>();
  MediaTransports transports([gate] { return std::make_unique<PathGatedDecoder>(gate); });
  struct ReleaseGate {
    std::shared_ptr<DecodeGate> gate;
    ~ReleaseGate() { { std::lock_guard<std::mutex> lock(gate->mutex); gate->released = true; } gate->changed.notify_all(); }
  } release{gate};
  auto blocked = testDesired(true, false);
  blocked.path = "blocked.wav";
  ASSERT_EQ(transports.apply({blocked}, 0).size(), 1u);
  const auto blockedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!gate->blocked.load() && std::chrono::steady_clock::now() < blockedDeadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_TRUE(gate->blocked.load() > 0);
  const auto changes = transports.apply({testDesired(true, false)}, 0);
  ASSERT_EQ(changes.size(), 2u);
  if (changes.size() != 2) return;
  EXPECT_FALSE(changes.front().added);
  EXPECT_TRUE(changes.back().added);
  auto fresh = changes.back().entry;
  ASSERT_TRUE(static_cast<bool>(fresh));
  if (!fresh) return;
  EXPECT_EQ(pollUntilFrame(*fresh), 2);
  { std::lock_guard<std::mutex> lock(gate->mutex); gate->released = true; }
  gate->changed.notify_all();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto after = MediaTransports::selectVideo(*fresh, steadyNowMs() * 10000);
  ASSERT_TRUE(after.has_value());
  if (after) EXPECT_EQ(after->frameId, 2);
}

TEST(MediaTransports, AudioPrefetchIsBoundedAndDecoderStopsOnDestruction) {
  auto gate = std::make_shared<DecodeGate>();
  {
    MediaTransports transports([gate] { return std::make_unique<TestDecoder>(gate); });
    const auto changes = transports.apply({testDesired(true, false)}, 0);
    ASSERT_EQ(changes.size(), 1u);
    if (changes.empty()) return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (gate->audioReads.load() < 2 && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(gate->audioReads.load(), 2);
    std::vector<AudioFrame> frames;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (frames.empty() && std::chrono::steady_clock::now() < until) {
      frames = transports.popAudio(steadyNowMs());
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(!frames.empty());
    if (!frames.empty()) EXPECT_EQ(frames.front().sampleCount, 960);
  }
  EXPECT_EQ(gate->destroyed.load(), 1);
}

TEST(MediaTransports, TheCapWarningNamesTheAssetItRefused) {
  auto gate = std::make_shared<DecodeGate>();
  MediaTransports transports([gate] { return std::make_unique<TestDecoder>(gate); });
  std::vector<MediaTransportDesired> desired;
  // Zero-padded ids so the desired map's lexicographic order matches admission
  // order 0..16 - an unpadded "asset-16" would sort ahead of "asset-9".
  for (int i = 0; i < 17; ++i) {
    auto row = testDesired(true, false);
    char id[16]; std::snprintf(id, sizeof(id), "asset-%02d", i);
    row.assetId = id;
    row.sourceId = "media:" + row.assetId;
    desired.push_back(row);
  }
  (void)transports.apply(desired, 0);
  const auto warnings = transports.warnings();
  const bool named = std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) {
    return w.find("Media decoder capacity reached") != std::string::npos &&
           w.find("media:asset-16") != std::string::npos;
  });
  EXPECT_TRUE(named);
}

TEST(MediaTransports, PauseAndResumeKeepOneDecoder) {
  std::atomic<int> created{0};
  MediaTransports t([&created] { ++created; return std::make_unique<CountingDecoder>(); });
  auto opened = t.apply({testDesired(true, false)}, 0);
  ASSERT_EQ(opened.size(), 1u);
  if (opened.empty()) return;
  auto e = opened.front().entry;
  ASSERT_GT(pollUntilFrame(*e), 0);
  EXPECT_EQ(created.load(), 1);
  std::string reason;
  EXPECT_TRUE(t.operatorAction("test", MediaOperatorAction::Pause, reason));
  const auto pauseEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (std::chrono::steady_clock::now() < pauseEnd) {
    (void)MediaTransports::selectVideo(*e, steadyNowMs() * 10000);
    (void)t.popAudio(steadyNowMs());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(created.load(), 1);
  EXPECT_TRUE(t.operatorAction("test", MediaOperatorAction::Play, reason));
  ASSERT_GT(pollUntilFrame(*e), 0);
  const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  while (std::chrono::steady_clock::now() < settle) {
    (void)MediaTransports::selectVideo(*e, steadyNowMs() * 10000);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(created.load(), 1);
}

TEST(MediaTransports, PauseHoldsTheOnAirFrame) {
  MediaTransports t([] { return std::make_unique<CountingDecoder>(); });
  auto opened = t.apply({testDesired(true, false)}, 0);
  ASSERT_EQ(opened.size(), 1u);
  if (opened.empty()) return;
  auto e = opened.front().entry;
  int64_t held = pollUntilFrame(*e);
  ASSERT_GT(held, 0);
  held = pollUntilFrame(*e, held + 2);  // Let it roll a few frames.
  ASSERT_GT(held, 0);
  std::string reason;
  EXPECT_TRUE(t.operatorAction("test", MediaOperatorAction::Pause, reason));
  int polls = 0;
  const auto start = std::chrono::steady_clock::now();
  const auto minEnd = start + std::chrono::milliseconds(250);
  const auto ceiling = start + std::chrono::seconds(3);
  while ((std::chrono::steady_clock::now() < minEnd || polls < 20) && std::chrono::steady_clock::now() < ceiling) {
    const auto frame = MediaTransports::selectVideo(*e, steadyNowMs() * 10000);
    ASSERT_TRUE(frame.has_value());
    if (frame) EXPECT_EQ(frame->frameId, held);
    ++polls;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(polls >= 20);
  EXPECT_TRUE(t.operatorAction("test", MediaOperatorAction::Play, reason));
  EXPECT_TRUE(pollUntilFrame(*e, held) > held);
}

TEST(MediaTransports, NoAudioWhilePausedAndAudioResumes) {
  std::atomic<int> created{0};
  MediaTransports t([&created] { ++created; return std::make_unique<CountingDecoder>(); });
  auto opened = t.apply({testDesired(true, false)}, 0);
  ASSERT_EQ(opened.size(), 1u);
  if (opened.empty()) return;
  auto e = opened.front().entry;
  bool sawAudio = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!sawAudio && std::chrono::steady_clock::now() < deadline) {
    (void)MediaTransports::selectVideo(*e, steadyNowMs() * 10000);
    sawAudio = hasNonSilentPcm(t.popAudio(steadyNowMs()));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(sawAudio);
  std::string reason;
  EXPECT_TRUE(t.operatorAction("test", MediaOperatorAction::Pause, reason));
  const auto pauseEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (std::chrono::steady_clock::now() < pauseEnd) {
    (void)MediaTransports::selectVideo(*e, steadyNowMs() * 10000);
    EXPECT_TRUE(t.popAudio(steadyNowMs()).empty());  // Not even silence.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(t.operatorAction("test", MediaOperatorAction::Play, reason));
  sawAudio = false;
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!sawAudio && std::chrono::steady_clock::now() < deadline) {
    (void)MediaTransports::selectVideo(*e, steadyNowMs() * 10000);
    sawAudio = hasNonSilentPcm(t.popAudio(steadyNowMs()));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(sawAudio);
  EXPECT_EQ(created.load(), 1);
}

// --- Review round 1 coverage -----------------------------------------------
namespace {
// A prefetch decoder that can be told to stop producing (end of media) or to
// stall for a few worker iterations, and that reports a position/duration the
// way MediaFoundationMediaFrameSource does.
struct ProbeState {
  std::atomic<bool> producing{true};
  // Real end of stream, as opposed to merely not producing (an open that has
  // not finished, a stall, a resume attempt).
  std::atomic<bool> ended{false};
  std::atomic<int> stallTicks{0};
  std::atomic<int64_t> positionMs{4200}, durationMs{9000};
  std::atomic<int64_t> frameId{0};
};
class ProbePrefetchDecoder final : public IMediaFrameSource, public IMediaVideoPrefetch {
 public:
  explicit ProbePrefetchDecoder(std::shared_ptr<ProbeState> state) : state_(std::move(state)) {}
  std::vector<VideoFrame> pollMediaFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t) override {
    if (!state_->producing.load()) return {};
    if (state_->stallTicks.load() > 0) { state_->stallTicks.fetch_sub(1); return {}; }
    VideoFrame frame;
    frame.participantId = layers.front().sourceId;
    frame.width = frame.pixelWidth = frame.height = frame.pixelHeight = 1;
    frame.pixelStride = 4; frame.frameId = state_->frameId.fetch_add(1) + 1;
    frame.pixels = std::make_shared<std::vector<uint8_t>>(4, 255);
    return {frame};
  }
  std::vector<ScheduledMediaVideo> prefetchMediaVideo(
      const std::vector<CompositorRenderPlanLayer>& layers, int64_t nowMs) override {
    std::vector<ScheduledMediaVideo> result;
    for (auto& frame : pollMediaFrames(layers, nowMs)) result.push_back({std::move(frame), nowMs * 10000});
    return result;
  }
  std::vector<AudioFrame> pollMediaAudioFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t) override {
    if (!state_->producing.load()) return {};
    AudioFrame frame; frame.participantId = layers.front().sourceId;
    frame.sampleRate = 48000; frame.channels = 2; frame.sampleCount = 960; frame.pcm.resize(1920, 0.5f);
    return {frame};
  }
  std::vector<std::string> warnings() const override { return {}; }
  int64_t playbackPositionMs() const override { return state_->positionMs.load(); }
  int64_t mediaDurationMs() const override { return state_->durationMs.load(); }
  bool mediaEnded() const override { return state_->ended.load(); }
 private:
  std::shared_ptr<ProbeState> state_;
};
std::optional<MediaTransports::Status> statusOf(const MediaTransports& transports, const std::string& sourceId) {
  for (const auto& row : transports.snapshot()) if (row.sourceId == sourceId) return row;
  return std::nullopt;
}
}  // namespace

// Finding 1. The ended window measures "the decoder has stopped producing".
// A clip deliberately not being read produced nothing for a different reason,
// so a pause longer than kEndedAfterNoNewFrameMs must not end it: the first
// resumed iteration (before any frame can arrive) used to trip the rule, which
// froze and silenced the clip ON AIR and made the next Play restart from 0.
TEST(MediaTransports, APauseLongerThanTheEndedWindowResumesRollingAndIsNeverEnded) {
  auto probe = std::make_shared<ProbeState>();
  MediaTransports t([probe] { return std::make_unique<ProbePrefetchDecoder>(probe); });
  auto opened = t.apply({testDesired(true, false)}, 0);
  ASSERT_EQ(opened.size(), 1u);
  if (opened.empty()) return;
  auto e = opened.front().entry;
  const auto rolled = pollUntilFrame(*e);
  ASSERT_GT(rolled, 0);
  std::string reason;
  EXPECT_TRUE(t.operatorAction("test", MediaOperatorAction::Pause, reason));
  // Held for comfortably longer than the 500 ms ended window.
  const auto pauseEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
  while (std::chrono::steady_clock::now() < pauseEnd) {
    (void)MediaTransports::selectVideo(*e, steadyNowMs() * 10000);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  // The first resumed iterations yield nothing, which is exactly the window in
  // which a stale anchor declared the clip over.
  probe->stallTicks.store(3);
  EXPECT_TRUE(t.operatorAction("test", MediaOperatorAction::Play, reason));
  EXPECT_TRUE(pollUntilFrame(*e, rolled) > rolled);
  const auto status = statusOf(t, "media:test");
  ASSERT_TRUE(status.has_value());
  if (status) EXPECT_TRUE(status->state == MediaTransportState::Live);
}

// Finding 2. A cap refusal is not a verdict for the life of the process: the
// source must stay NAMED while the cap still bites (warnings_ is rebuilt every
// apply) and must open the moment a decoder frees.
TEST(MediaTransports, ARefusedSourceStaysNamedAndOpensOnceADecoderFrees) {
  auto gate = std::make_shared<DecodeGate>();
  MediaTransports transports([gate] { return std::make_unique<TestDecoder>(gate); });
  std::vector<MediaTransportDesired> full;
  for (int i = 0; i < 17; ++i) {
    auto row = testDesired(true, false);
    char id[16]; std::snprintf(id, sizeof(id), "asset-%02d", i);
    row.assetId = id;
    row.sourceId = "media:" + row.assetId;
    full.push_back(row);
  }
  const auto names17th = [](const std::vector<std::string>& warnings) {
    return std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) {
      return w.find("Media decoder capacity reached") != std::string::npos &&
             w.find("media:asset-16") != std::string::npos;
    });
  };
  (void)transports.apply(full, 0);
  EXPECT_TRUE(names17th(transports.warnings()));
  // The SAME desired set again: the refusal is still true, so it is still said.
  (void)transports.apply(full, 0);
  EXPECT_TRUE(names17th(transports.warnings()));
  // Release one. The freed slot is not available until its worker has been
  // reaped, so re-assert the set until the refused source is admitted.
  // Release carries a short grace now, measured against the CALLER's nowNs, so
  // the re-assertions advance that clock past it (nothing here sleeps for real
  // time: the grace is decided from the number apply() is handed).
  std::vector<MediaTransportDesired> without(full.begin() + 1, full.end());
  bool opened = false;
  std::int64_t nowNs = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!opened && std::chrono::steady_clock::now() < deadline) {
    nowNs += 1'000'000'000;
    for (const auto& change : transports.apply(without, nowNs))
      if (change.added && change.sourceId == "media:asset-16") opened = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(opened);
  EXPECT_FALSE(names17th(transports.warnings()));
}

// Finding 3. One asset can be two sources. The operator transport names the
// ROUTE; `background:` sorts first in the map and must not answer for it.
TEST(MediaTransports, AnOperatorPauseResolvesTheRouteNotTheBackgroundLoop) {
  MediaTransports t([] { return std::make_unique<CountingDecoder>(); });
  auto route = testDesired(true, false);             // media:test, not a loop
  auto background = testDesired(true, false, true);  // background:test, same asset, a loop
  const auto opened = t.apply({route, background}, 0);
  ASSERT_EQ(opened.size(), 2u);
  if (opened.size() != 2) return;
  std::string reason;
  EXPECT_TRUE(t.operatorAction("test", MediaOperatorAction::Pause, reason));
  const auto clip = statusOf(t, "media:test");
  const auto loop = statusOf(t, "background:test");
  ASSERT_TRUE(clip.has_value());
  ASSERT_TRUE(loop.has_value());
  if (clip) EXPECT_TRUE(clip->state == MediaTransportState::Paused);
  if (loop) EXPECT_TRUE(loop->state == MediaTransportState::Live);  // a loop never pauses
}

// Finding 4a. Ended is BOOKKEEPING: the worker keeps holding the last picture
// and emits no audio; only the published state and the operator Play rule
// change.
TEST(MediaTransports, ANonLoopClipThatStopsProducingEndsAndKeepsItsLastFrame) {
  auto probe = std::make_shared<ProbeState>();
  MediaTransports t([probe] { return std::make_unique<ProbePrefetchDecoder>(probe); });
  auto opened = t.apply({testDesired(true, false)}, 0);
  ASSERT_EQ(opened.size(), 1u);
  if (opened.empty()) return;
  auto e = opened.front().entry;
  ASSERT_GT(pollUntilFrame(*e), 0);
  // THE DECODER SAYS IT ENDED. That is the evidence now; merely not producing
  // is a stall, and is judged by the (much longer) backstop below.
  probe->producing.store(false);
  probe->ended.store(true);
  bool ended = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (!ended && std::chrono::steady_clock::now() < deadline) {
    (void)MediaTransports::selectVideo(*e, steadyNowMs() * 10000);
    const auto status = statusOf(t, "media:test");
    ended = status && status->state == MediaTransportState::Ended;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(ended);
  const auto held = MediaTransports::selectVideo(*e, steadyNowMs() * 10000);
  EXPECT_TRUE(held.has_value());  // still serving its last picture, never nothing
  EXPECT_TRUE(t.popAudio(steadyNowMs()).empty());
}

// Finding 4b/4c. snapshot() republishes what the decoder measured, and -1 (not
// 0) when it cannot say.
TEST(MediaTransports, SnapshotPublishesTheDecodersPositionAndDuration) {
  auto probe = std::make_shared<ProbeState>();
  MediaTransports t([probe] { return std::make_unique<ProbePrefetchDecoder>(probe); });
  ASSERT_EQ(t.apply({testDesired(true, false)}, 0).size(), 1u);
  bool published = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!published && std::chrono::steady_clock::now() < deadline) {
    const auto status = statusOf(t, "media:test");
    published = status && status->durationMs == 9000 && status->positionMs == 4200;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(published);
}

TEST(MediaTransports, ADecoderThatCannotReportItsPositionPublishesMinusOne) {
  MediaTransports t([] { return std::make_unique<CountingDecoder>(); });
  auto opened = t.apply({testDesired(true, false)}, 0);
  ASSERT_EQ(opened.size(), 1u);
  if (opened.empty()) return;
  ASSERT_GT(pollUntilFrame(*opened.front().entry), 0);  // the worker has run
  const auto status = statusOf(t, "media:test");
  ASSERT_TRUE(status.has_value());
  if (status) {
    EXPECT_EQ(status->positionMs, -1);
    EXPECT_EQ(status->durationMs, -1);
  }
}

// Final review, CRITICAL 1. The frame-arrival window is now only a BACKSTOP
// for a decoder that cannot report EOS, and it is deliberately far longer than
// the FFmpeg resume ladder. A decoder that merely stops producing must NOT be
// called finished at the old 500 ms. This test is slow on purpose: the whole
// point of the constant is that it is long.
TEST(MediaTransports, ADecoderThatCannotSayItEndedIsOnlyJudgedByTheLongBackstop) {
  auto probe = std::make_shared<ProbeState>();
  MediaTransports t([probe] { return std::make_unique<ProbePrefetchDecoder>(probe); });
  auto opened = t.apply({testDesired(true, false)}, 0);
  ASSERT_EQ(opened.size(), 1u);
  if (opened.empty()) return;
  auto e = opened.front().entry;
  ASSERT_GT(pollUntilFrame(*e), 0);
  probe->producing.store(false);  // stopped, but the decoder never says "ended"

  const auto stateNow = [&] {
    const auto status = statusOf(t, "media:test");
    return status ? status->state : MediaTransportState::Cued;
  };
  // Well past the OLD 500 ms window and past the FFmpeg ladder's first rungs:
  // still rolling, because nothing has said the media ran out.
  const auto notYet = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
  while (std::chrono::steady_clock::now() < notYet) {
    (void)MediaTransports::selectVideo(*e, steadyNowMs() * 10000);
    ASSERT_TRUE(stateNow() == MediaTransportState::Live)
        << "a stalled clip was called finished inside the resume ladder's window";
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // The backstop still exists, so it eventually does end.
  bool ended = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!ended && std::chrono::steady_clock::now() < deadline) {
    (void)MediaTransports::selectVideo(*e, steadyNowMs() * 10000);
    ended = stateNow() == MediaTransportState::Ended;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(ended) << "the backstop is gone: a dead decoder never reads ended at all";
}

// Final review, CRITICAL 1(b). Ended is RECOVERABLE. A decoder that reported
// EOS and then hands back a picture (an FFmpeg resume off its ladder) brings
// the transport back to Live on its own, with no operator gesture - the only
// one available is Play, which is OpenLive and restarts the clip from 0.
TEST(MediaTransports, AnEndedClipComesBackToLiveWhenItsDecoderProducesAgain) {
  auto probe = std::make_shared<ProbeState>();
  MediaTransports t([probe] { return std::make_unique<ProbePrefetchDecoder>(probe); });
  auto opened = t.apply({testDesired(true, false)}, 0);
  ASSERT_EQ(opened.size(), 1u);
  if (opened.empty()) return;
  auto e = opened.front().entry;
  const auto rolledTo = pollUntilFrame(*e);
  ASSERT_GT(rolledTo, 0);
  probe->producing.store(false);
  probe->ended.store(true);

  const auto waitForState = [&](MediaTransportState want, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      (void)MediaTransports::selectVideo(*e, steadyNowMs() * 10000);
      const auto status = statusOf(t, "media:test");
      if (status && status->state == want) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  };
  ASSERT_TRUE(waitForState(MediaTransportState::Ended, 4000));
  probe->ended.store(false);
  probe->producing.store(true);
  EXPECT_TRUE(waitForState(MediaTransportState::Live, 4000))
      << "Ended is still terminal: nothing but an operator Play gets out of it";
  // The SAME decoder, carrying on from where it was - never a restart from 0.
  const auto after = MediaTransports::selectVideo(*e, steadyNowMs() * 10000);
  ASSERT_TRUE(after.has_value());
  if (after) EXPECT_GT(after->frameId, rolledTo);
}

// Final review, IMPORTANT 3. A source absent from both desired sets is kept
// for a short grace so ONE interleaved preview-only spine tick cannot destroy
// a warm decoder - and is released for real once the grace is up.
TEST(MediaTransports, AReleasedSourceSurvivesOneTickAndIsRetiredAfterTheGrace) {
  MediaTransports t([] { return std::make_unique<CountingDecoder>(); });
  const auto opened = t.apply({testDesired(false, true)}, 0);  // cued in Preview only
  ASSERT_EQ(opened.size(), 1u);
  if (opened.empty()) return;

  // The interleaved tick: on neither bus. No membership change, no retirement.
  EXPECT_TRUE(t.apply({}, 1'000'000).empty()) << "one absent tick retired the source";
  EXPECT_TRUE(statusOf(t, "media:test").has_value());

  // The Take lands behind it: the SAME entry resumes, no second decoder.
  const auto taken = t.apply({testDesired(true, false)}, 2'000'000);
  EXPECT_TRUE(taken.empty()) << "the re-claim churned bus membership";
  const auto live = statusOf(t, "media:test");
  ASSERT_TRUE(live.has_value());
  if (live) EXPECT_TRUE(live->state == MediaTransportState::Live);

  // Genuinely gone: absent past the grace, and it is retired.
  EXPECT_TRUE(t.apply({}, 2'100'000).empty());
  const auto retired = t.apply({}, 5'000'000'000);
  ASSERT_EQ(retired.size(), 1u);
  if (retired.size() == 1) EXPECT_FALSE(retired.front().added);
  EXPECT_FALSE(statusOf(t, "media:test").has_value());
}

// ...and the render tick's sweep retires it even when no further command ever
// arrives, which is the only path that runs with Engine off.
TEST(MediaTransports, TheRenderSweepRetiresASourceNoCommandEverMentionsAgain) {
  MediaTransports t([] { return std::make_unique<CountingDecoder>(); });
  ASSERT_EQ(t.apply({testDesired(true, false)}, 0).size(), 1u);
  EXPECT_TRUE(t.apply({}, 0).empty());  // grace opens
  EXPECT_TRUE(t.collectExpiredReleases(1'000'000).empty()) << "the sweep retired it inside the grace";
  const auto swept = t.collectExpiredReleases(5'000'000'000);
  ASSERT_EQ(swept.size(), 1u);
  if (swept.size() == 1) {
    EXPECT_FALSE(swept.front().added);
    EXPECT_EQ(swept.front().sourceId, "media:test");
  }
  EXPECT_FALSE(statusOf(t, "media:test").has_value());
}
