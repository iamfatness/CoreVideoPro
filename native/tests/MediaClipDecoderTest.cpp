#include "modules/MediaClipDecoder.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

using namespace corevideo::modules;

namespace {
// Fixture invariants (rule 1): every script has >= 1 sample; pts are strictly
// increasing and start at 0; durations are equal (a 30 fps clip = 333'333);
// a sample's colour byte is its INDEX in the script + 1 (so index 0 -> 0x01),
// which makes "which sample is on air" independent of the code under test.
constexpr int64_t kFrame = 333'333;
struct Script {
  int samples = 10;
  int pendingBeforeFirst = 0;   // take() returns Pending this many times first
  bool failOpen = false;        // openVideo returns nullptr + error
  int failAfter = -1;           // take() returns Failed after this many samples
  bool stallAfterLast = false;  // Pending forever instead of EndOfStream
  int failingReader = 0;        // the Nth reader opened (1-based) fails on its first take; 0 = none
  int64_t duration100ns = 10 * kFrame;
};
// Audio script (Task 6). Fixture invariants (rule 1): 48 kHz interleaved stereo;
// chunks are 480 frames (10 ms); pts are contiguous from the reader's start; every
// sample's VALUE encodes its own media millisecond, value = (ms + 1) / 4096 (the
// timecode scheme of MediaCoreCommandTest.cpp's timecoded WAV), so "which media
// time is this" is a fixture constant and a sample is never exactly 0.0.
// The fake honours the reader contract (rule 9): take() never blocks, and an
// open takes `openLatencyMs` of FIXTURE time before the first sample is ready,
// as a real open does.
struct AudioScript {
  bool present = true;       // false: openAudio returns nullptr + "" (no audio track)
  int seconds = 3;
  int openLatencyMs = 0;
};
struct Shared {
  int opened = 0, takesReady = 0, stillDecodes = 0, destroyed = 0;
  std::function<void()> lastWake;
  Script script;
  AudioScript audio;
  int audioOpened = 0, audioDestroyed = 0;
  std::vector<int64_t> audioStarts100ns;
  const int64_t* nowMs = nullptr;  // the rig's clock
};
int timecodeMs(float sample) { return static_cast<int>(std::lround(sample * 4096.f)) - 1; }
class FakeAudioReader final : public IClipAudioReader {
 public:
  FakeAudioReader(std::shared_ptr<Shared> s, int64_t start100ns) : s_(std::move(s)) {
    frame_ = start100ns * 48000 / 10'000'000;
    readyAtMs_ = (s_->nowMs ? *s_->nowMs : 0) + s_->audio.openLatencyMs;
  }
  ~FakeAudioReader() override { s_->audioDestroyed += 1; }
  ClipReadStatus take(ClipAudioSample& out, std::string&) override {
    if (s_->nowMs && *s_->nowMs < readyAtMs_) return ClipReadStatus::Pending;
    const int64_t total = static_cast<int64_t>(s_->audio.seconds) * 48000;
    if (frame_ >= total) return ClipReadStatus::EndOfStream;
    const int64_t count = (std::min)(int64_t{480}, total - frame_);
    out.pcm.resize(static_cast<size_t>(count) * 2u);
    for (int64_t i = 0; i < count; ++i) {
      const float value = static_cast<float>((frame_ + i) / 48 + 1) / 4096.f;
      out.pcm[static_cast<size_t>(i) * 2u] = value;
      out.pcm[static_cast<size_t>(i) * 2u + 1u] = value;
    }
    out.pts100ns = frame_ * 10'000'000 / 48000;
    frame_ += count;
    return ClipReadStatus::Ready;
  }
 private:
  std::shared_ptr<Shared> s_;
  int64_t frame_ = 0, readyAtMs_ = 0;
};
class FakeReader final : public IClipVideoReader {
 public:
  explicit FakeReader(std::shared_ptr<Shared> s, std::function<void()> wake) : s_(std::move(s)) {
    s_->lastWake = std::move(wake);
    ordinal_ = s_->opened;  // openVideo counted this reader before constructing it
  }
  ~FakeReader() override { s_->destroyed += 1; }
  ClipReadStatus take(ClipVideoSample& out, std::string& error) override {
    if (pending_ < s_->script.pendingBeforeFirst) { ++pending_; return ClipReadStatus::Pending; }
    if (s_->script.failingReader != 0 && ordinal_ == s_->script.failingReader) { error = "transient open failure"; return ClipReadStatus::Failed; }
    if (s_->script.failAfter >= 0 && next_ >= s_->script.failAfter) { error = "decode error"; return ClipReadStatus::Failed; }
    if (next_ >= s_->script.samples)
      return s_->script.stallAfterLast ? ClipReadStatus::Pending : ClipReadStatus::EndOfStream;
    out.width = 4; out.height = 2;
    out.bgra = std::make_shared<std::vector<uint8_t>>(4u * 2u * 4u, static_cast<uint8_t>(next_ + 1));
    out.pts100ns = next_ * kFrame; out.duration100ns = kFrame;
    ++next_; s_->takesReady += 1;
    return ClipReadStatus::Ready;
  }
  int64_t duration100ns() const override { return s_->script.duration100ns; }
 private:
  std::shared_ptr<Shared> s_;
  int pending_ = 0, next_ = 0, ordinal_ = 0;
};
class FakeReaders final : public IClipReaderFactory {
 public:
  explicit FakeReaders(std::shared_ptr<Shared> s) : s_(std::move(s)) {}
  std::unique_ptr<IClipVideoReader> openVideo(const std::string& path, std::function<void()> wake,
                                              std::string& error) override {
    if (s_->script.failOpen) { error = "AVFoundation cannot open " + path; return nullptr; }
    s_->opened += 1;
    return std::make_unique<FakeReader>(s_, std::move(wake));
  }
  std::unique_ptr<IClipAudioReader> openAudio(const std::string&, int64_t start100ns, std::function<void()>,
                                              std::string& error) override {
    error.clear();
    if (!s_->audio.present) return nullptr;
    s_->audioOpened += 1;
    s_->audioStarts100ns.push_back(start100ns);
    return std::make_unique<FakeAudioReader>(s_, start100ns);
  }
  bool decodeStill(const std::string&, VideoFrame& frame, std::string&) override {
    s_->stillDecodes += 1;
    frame.width = frame.pixelWidth = frame.naturalWidth = 2;
    frame.height = frame.pixelHeight = frame.naturalHeight = 2;
    frame.pixelStride = 8;
    frame.pixels = std::make_shared<std::vector<uint8_t>>(16u, 0x7f);
    return true;
  }
 private:
  std::shared_ptr<Shared> s_;
};
MediaDecodeRequest clip(bool playing, bool loop = false, const std::string& path = "/m/clip.mp4") {
  MediaDecodeRequest r;
  r.kind = "media-video"; r.sourceId = "media:clip"; r.assetId = "clip";
  r.assetKind = "stinger"; r.assetPath = path; r.playing = playing; r.loop = loop;
  return r;
}
uint8_t colour(const VideoFrame& f) { return (*f.pixels)[0]; }
struct Rig {
  std::shared_ptr<Shared> s = std::make_shared<Shared>();
  int64_t now = 1'000;
  std::unique_ptr<MediaClipDecoder> d;
  explicit Rig(Script script = {}, AudioScript audio = {}) {
    s->script = script;
    s->audio = audio;
    s->nowMs = &now;
    d = std::make_unique<MediaClipDecoder>(std::make_shared<FakeReaders>(s), [this] { return now; });
  }
  // Prefetch until a sample arrives (fake is synchronous; Pending only by script).
  ScheduledMediaVideo next(const MediaDecodeRequest& r) {
    for (int i = 0; i < 20; ++i) {
      auto v = d->prefetchMediaVideo(r, now);
      if (!v.empty()) return v.front();
    }
    EXPECT_TRUE(false) << "no sample in 20 prefetches";  // the vendored gtest shim has no ADD_FAILURE
    return {};
  }
};
}  // namespace

TEST(MediaClipDecoder, ACuePosterDecodesExactlyOneFrameAndHoldsIt) {
  Rig rig;
  for (int i = 0; i < 25; ++i) {
    const auto frames = rig.d->pollMediaFrames(clip(false), 1'000 + i * 4);
    ASSERT_EQ(frames.size(), 1u) << "poll " << i;
    EXPECT_EQ(frames.front().frameId, 1);
    EXPECT_EQ(colour(frames.front()), 0x01);
    EXPECT_EQ(frames.front().participantId, "media:clip");
  }
  EXPECT_EQ(rig.s->takesReady, 1) << "a poster reads ONE sample";
  EXPECT_EQ(rig.s->opened, 1);
}

TEST(MediaClipDecoder, ACueThatRollsContinuesFromTheSecondSampleWithoutReopening) {
  Rig rig;
  (void)rig.d->pollMediaFrames(clip(false), 1'000);
  rig.d->syncMediaClock(clip(true), 1'000);
  const auto s = rig.next(clip(true));
  EXPECT_EQ(s.frame.frameId, 2);
  EXPECT_EQ(colour(s.frame), 0x02) << "the reader continued; a reopen would show 0x01";
  EXPECT_EQ(rig.s->opened, 1);
}

TEST(MediaClipDecoder, PlayingFramesCarryDueTimeEpochPlusPts) {
  Rig rig;
  rig.d->syncMediaClock(clip(true), 1'000);            // epoch = 1000 ms = 10'000'000 x100ns
  for (int i = 0; i < 3; ++i) {
    const auto s = rig.next(clip(true));
    EXPECT_EQ(s.frame.frameId, i + 1);
    EXPECT_EQ(s.due100ns, 10'000'000 + i * kFrame) << "sample " << i;
  }
}

TEST(MediaClipDecoder, PausedAfterRollingReadsNothingAndHoldsTheFrame) {
  Rig rig;
  rig.d->syncMediaClock(clip(true), 1'000);
  for (int i = 0; i < 5; ++i) (void)rig.next(clip(true));
  const int takesBefore = rig.s->takesReady;            // == 5
  rig.d->syncMediaClock(clip(false), 1'200);
  for (int i = 0; i < 30; ++i) {
    const auto frames = rig.d->pollMediaFrames(clip(false), 1'200 + i * 16);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames.front().frameId, 5);
    EXPECT_EQ(colour(frames.front()), 0x05);
  }
  EXPECT_EQ(rig.s->takesReady, takesBefore) << "a paused clip reads nothing";
}

TEST(MediaClipDecoder, ResumeContinuesWithTheNextSampleOnTheShiftedClock) {
  Rig rig;
  rig.d->syncMediaClock(clip(true), 1'000);
  for (int i = 0; i < 5; ++i) (void)rig.next(clip(true));
  rig.d->syncMediaClock(clip(false), 1'200);             // paused at 200 ms elapsed
  rig.now = 1'700;
  rig.d->syncMediaClock(clip(true), 1'700);              // 500 ms paused -> epoch 1500 ms
  const auto s = rig.next(clip(true));
  EXPECT_EQ(s.frame.frameId, 6) << "bounded below: not a hold; bounded above: no skip";
  EXPECT_EQ(colour(s.frame), 0x06) << "a restart from 0 would be 0x01";
  EXPECT_EQ(s.due100ns, 15'000'000 + 5 * kFrame);
  EXPECT_EQ(rig.s->opened, 1) << "resume never reopens";
}

TEST(MediaClipDecoder, AClipHoldsItsLastFrameAndReportsEndedAtEndOfStream) {
  Script script; script.samples = 3; script.duration100ns = 3 * kFrame;
  Rig rig(script);
  rig.d->syncMediaClock(clip(true), 1'000);
  for (int i = 0; i < 3; ++i) (void)rig.next(clip(true));
  EXPECT_FALSE(rig.d->mediaEnded()) << "EOS has not been READ yet";
  (void)rig.d->prefetchMediaVideo(clip(true), 1'200);    // reads EOS
  EXPECT_TRUE(rig.d->mediaEnded());
  const auto held = rig.d->pollMediaFramesAt100ns(clip(true), 13'000'000);
  ASSERT_EQ(held.size(), 1u);
  EXPECT_EQ(held.front().frameId, 3);
}

TEST(MediaClipDecoder, AStalledReaderNeverReportsEndedAndKeepsTheHeldFrame) {
  Script script; script.samples = 2; script.stallAfterLast = true;
  Rig rig(script);
  rig.d->syncMediaClock(clip(true), 1'000);
  (void)rig.next(clip(true)); (void)rig.next(clip(true));
  for (int t = 0; t < 7'000; t += 100) {
    rig.now = 1'000 + t;
    (void)rig.d->prefetchMediaVideo(clip(true), rig.now);
  }
  EXPECT_FALSE(rig.d->mediaEnded()) << "a stall is not the end of the media";
  const auto held = rig.d->pollMediaFrames(clip(false), rig.now);
  ASSERT_EQ(held.size(), 1u);
  EXPECT_EQ(held.front().frameId, 2);
}

TEST(MediaClipDecoder, ALoopPreopensItsNextReaderAndAdvancesTheOffset) {
  // 3 samples of 333'333 = duration 999'999; the pre-open threshold is
  // duration - 3'000'000 < 0, so the FIRST sample taken already pre-opens.
  Script script; script.samples = 3; script.duration100ns = 3 * kFrame;
  Rig rig(script);
  rig.d->syncMediaClock(clip(true, /*loop=*/true), 1'000);
  (void)rig.next(clip(true, true));
  EXPECT_EQ(rig.s->opened, 2) << "the next reader is opened before the end";
  (void)rig.next(clip(true, true)); (void)rig.next(clip(true, true));
  const auto s = rig.next(clip(true, true));             // first sample of loop 2
  EXPECT_EQ(s.frame.frameId, 4) << "ids keep rising across the seam";
  EXPECT_EQ(colour(s.frame), 0x01) << "loop 2 starts at the top of the file";
  EXPECT_EQ(s.due100ns, 10'000'000 + 3 * kFrame) << "offset = lastPts + lastDuration";
  EXPECT_FALSE(rig.d->mediaEnded()) << "a loop never ends";
}

TEST(MediaClipDecoder, PositionWrapsForALoopClampsForAClipAndIsMinusOneBeforeAnyRequest) {
  Script script; script.duration100ns = 30'000'000;      // 3 s
  Rig loop(script), once(script);
  EXPECT_EQ(once.d->playbackPositionMs(), -1);
  EXPECT_EQ(once.d->mediaDurationMs(), -1) << "no reader yet";
  loop.d->syncMediaClock(clip(true, true), 1'000);  (void)loop.next(clip(true, true));
  once.d->syncMediaClock(clip(true, false), 1'000); (void)once.next(clip(true, false));
  loop.now = once.now = 1'000 + 3'400;
  EXPECT_EQ(loop.d->playbackPositionMs(), 400);
  EXPECT_EQ(once.d->playbackPositionMs(), 3'000);
  EXPECT_EQ(once.d->mediaDurationMs(), 3'000);
}

TEST(MediaClipDecoder, AnUnknownDurationReportsRawElapsedAndMinusOneDuration) {
  Script script; script.duration100ns = -1;
  Rig rig(script);
  rig.d->syncMediaClock(clip(true, true), 1'000); (void)rig.next(clip(true, true));
  rig.now = 1'000 + 3'400;
  EXPECT_EQ(rig.d->playbackPositionMs(), 3'400);
  EXPECT_EQ(rig.d->mediaDurationMs(), -1);
}

TEST(MediaClipDecoder, AStillOnAMediaVideoRequestReturnsNothingAndOpensNothing) {
  Rig rig;
  auto r = clip(true, false, "/m/logo.png");
  EXPECT_TRUE(rig.d->pollMediaFrames(r, 1'000).empty());
  EXPECT_TRUE(rig.d->prefetchMediaVideo(r, 1'000).empty());
  EXPECT_EQ(rig.s->opened, 0);
  EXPECT_EQ(rig.s->stillDecodes, 0);
}

TEST(MediaClipDecoder, ABackgroundStillDecodesOnceAndHolds) {
  Rig rig;
  auto r = clip(true, true, "/m/backdrop.png");
  r.kind = "media-background"; r.sourceId = "background:bg"; r.assetId = "bg";
  for (int i = 0; i < 10; ++i) {
    const auto frames = rig.d->pollMediaFrames(r, 1'000 + i * 16);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames.front().participantId, "background:bg");
  }
  EXPECT_EQ(rig.s->stillDecodes, 1);
  EXPECT_EQ(rig.s->opened, 0);
}

TEST(MediaClipDecoder, OpenFailuresWarnWithTheMediaFoundationWording) {
  Script script; script.failOpen = true;
  Rig rig(script);
  EXPECT_TRUE(rig.d->pollMediaFrames(clip(true), 1'000).empty());
  ASSERT_EQ(rig.d->warnings().size(), 1u);
  EXPECT_EQ(rig.d->warnings()[0],
            "Media asset clip could not be opened for Program playback: AVFoundation cannot open /m/clip.mp4");
  Rig cue(script);
  EXPECT_TRUE(cue.d->pollMediaFrames(clip(false), 1'000).empty());
  ASSERT_EQ(cue.d->warnings().size(), 1u);
  EXPECT_EQ(cue.d->warnings()[0],
            "Media asset clip could not be opened for Preview cueing: AVFoundation cannot open /m/clip.mp4");
}

TEST(MediaClipDecoder, AMidStreamFailureWarnsAndHoldsTheLastFrame) {
  Script script; script.failAfter = 2;
  Rig rig(script);
  rig.d->syncMediaClock(clip(true), 1'000);
  (void)rig.next(clip(true)); (void)rig.next(clip(true));
  EXPECT_TRUE(rig.d->prefetchMediaVideo(clip(true), 1'100).empty());
  ASSERT_FALSE(rig.d->warnings().empty());
  EXPECT_EQ(rig.d->warnings()[0], "Media asset clip failed while decoding a video frame.");
  const auto held = rig.d->pollMediaFrames(clip(false), 1'200);
  ASSERT_EQ(held.size(), 1u);
  EXPECT_EQ(held.front().frameId, 2);
}

TEST(MediaClipDecoder, TheDirectPollPathPresentsByMediaTime) {
  Rig rig;
  rig.d->syncMediaClock(clip(true), 1'000);
  // 34 ms elapsed: samples at 0 and 33.3 ms are due, 66.7 ms is not.
  // PLAN FIXTURE CORRECTED (rule 4 corollary): the plan polled at epoch + 3'400'000
  // (340 ms) and + 3'500'000 and described the samples as "0, 333.3 ms, 666.7 ms".
  // kFrame is 333'333 x100ns = 33.3 ms, so at 340 ms TEN samples are due and a
  // correct decoder presents sample 4 (its 4-take cap), not 2. The intent stated
  // by the assertions (two due, the third not) holds at 34 ms / 35 ms.
  const auto at340 = rig.d->pollMediaFramesAt100ns(clip(true), 10'000'000 + 340'000);
  ASSERT_EQ(at340.size(), 1u);
  EXPECT_EQ(at340.front().frameId, 2);
  EXPECT_EQ(colour(at340.front()), 0x02);
  const auto again = rig.d->pollMediaFramesAt100ns(clip(true), 10'000'000 + 350'000);
  ASSERT_EQ(again.size(), 1u);
  EXPECT_EQ(again.front().frameId, 2) << "a repeated poll holds the presented frame";
}

TEST(MediaClipDecoder, AnIdentityChangeOpensANewReaderAndIdsKeepRising) {
  Rig rig;
  rig.d->syncMediaClock(clip(true), 1'000);
  for (int i = 0; i < 3; ++i) (void)rig.next(clip(true));
  auto other = clip(true, false, "/m/other.mp4");
  rig.d->syncMediaClock(other, 1'100);
  const auto s = rig.next(other);
  EXPECT_EQ(rig.s->opened, 2);
  EXPECT_EQ(colour(s.frame), 0x01) << "the new file starts at its top";
  EXPECT_EQ(s.frame.frameId, 4) << "ids never restart inside one decoder";
  EXPECT_EQ(rig.s->destroyed, 1) << "the old reader was destroyed";
}

TEST(MediaClipDecoder, AReaderWakeReachesTheOwnerAndNeverAfterDestruction) {
  Rig rig;
  std::atomic<int> wakes{0};
  rig.d->setMediaWakeCallback([&] { wakes.fetch_add(1); });
  (void)rig.d->pollMediaFrames(clip(false), 1'000);
  ASSERT_TRUE(static_cast<bool>(rig.s->lastWake));
  rig.s->lastWake();
  EXPECT_EQ(wakes.load(), 1);
  auto lateWake = rig.s->lastWake;                       // a reader callback still in flight
  rig.d.reset();
  lateWake();
  EXPECT_EQ(wakes.load(), 1) << "no wake may reach a retired decoder's owner";
}

// ---- Audio (Task 6): the second reader, windows cut at the clock position ----

TEST(MediaClipDecoder, NoAudioWhilePausedAndAudioResumesAtTheClockPosition) {
  // Play 300 ms, pause 400 ms, play. Polled every 20 ms of fixture time, as the owner's audio slots are.
  Rig rig;
  rig.d->syncMediaClock(clip(true), 1'000);
  int lastHeardMs = -1;
  for (rig.now = 1'000; rig.now <= 1'300; rig.now += 20) {
    for (const auto& frame : rig.d->pollMediaAudioFrames(clip(true), rig.now)) lastHeardMs = timecodeMs(frame.pcm.back());
  }
  ASSERT_GE(lastHeardMs, 300);  // fixture fact: 300 ms were played, so media ms >= 300 was heard
  ASSERT_LE(lastHeardMs, 340);
  rig.now = 1'300;
  rig.d->syncMediaClock(clip(false), 1'300);
  for (; rig.now < 1'700; rig.now += 20) {
    EXPECT_EQ(rig.d->pollMediaAudioFrames(clip(false), rig.now).size(), 0u) << "paused at " << rig.now;
  }
  rig.now = 1'700;
  rig.d->syncMediaClock(clip(true), 1'700);
  int resumedMs = -1;
  for (int polls = 0; polls < 10 && resumedMs < 0; ++polls, rig.now += 20) {
    for (const auto& frame : rig.d->pollMediaAudioFrames(clip(true), rig.now)) {
      for (const auto sample : frame.pcm) if (sample != 0.f) { resumedMs = timecodeMs(sample); break; }
      if (resumedMs >= 0) break;
    }
  }
  ASSERT_GE(resumedMs, 0);
  EXPECT_LE(std::abs(resumedMs - lastHeardMs), 50) << "lastHeard=" << lastHeardMs << " resumed=" << resumedMs;
  EXPECT_GE(resumedMs, lastHeardMs - 50) << "bounded below: not a restart from 0";
  EXPECT_LE(resumedMs, lastHeardMs + 50) << "bounded above: the 400 ms spent paused was not skipped";
  EXPECT_EQ(rig.s->audioOpened, 1) << "resume never reopens the audio reader";
}

TEST(MediaClipDecoder, AudioWindowsAreContiguousAcrossALoopSeam) {
  // 1 s looping script (video 30 x 33.3 ms, audio 1 s) with a 100 ms open latency on every audio
  // reader. 2.5 s of windows at a fixed 20 ms cadence; video is polled too, as the owner does (the
  // loop's pre-open threshold comes from the video reader's duration).
  Script video; video.samples = 30; video.duration100ns = 30 * kFrame;
  AudioScript audio; audio.seconds = 1; audio.openLatencyMs = 100;
  Rig rig(video, audio);
  const auto request = clip(true, /*loop=*/true);
  rig.d->syncMediaClock(request, 1'000);
  std::vector<float> heard;
  int pollsAfterFirstWindow = 0, windowsAfterFirst = 0;
  bool started = false;
  for (int t = 0; t < 2'500; t += 20) {
    rig.now = 1'000 + t;
    (void)rig.d->prefetchMediaVideo(request, rig.now);
    const auto windows = rig.d->pollMediaAudioFrames(request, rig.now);
    if (started) { ++pollsAfterFirstWindow; windowsAfterFirst += static_cast<int>(windows.size()); }
    if (!windows.empty()) started = true;
    for (const auto& window : windows) {
      for (size_t i = 0; i < window.pcm.size(); i += 2) heard.push_back(window.pcm[i]);
    }
  }
  ASSERT_TRUE(started);
  // Fixture fact: the first reader is ready 100 ms in, so windows start by 200 ms and at least
  // 2.2 s of them follow. One window per poll from then on: a missed slot is a hole on air.
  EXPECT_GE(pollsAfterFirstWindow, 110);
  EXPECT_EQ(windowsAfterFirst, pollsAfterFirstWindow) << "a poll after the first window produced no audio";
  int wraps = 0;
  for (size_t i = 0; i < heard.size(); ++i) {
    ASSERT_TRUE(heard[i] != 0.f) << "sample " << i << " is a hole (the timecode is never 0.0)";
    if (i == 0) continue;
    const int previous = timecodeMs(heard[i - 1]), current = timecodeMs(heard[i]);
    if (previous == 999 && current == 0) { ++wraps; continue; }
    ASSERT_TRUE(current == previous || current == previous + 1)
        << "sample " << i << ": media ms " << previous << " -> " << current;
  }
  EXPECT_EQ(wraps, 2) << "2.5 s of a 1 s loop crosses its seam twice";
  EXPECT_EQ(rig.s->audioStarts100ns.size(), 3u) << "one reader per loop pass heard";
  for (size_t i = 1; i < rig.s->audioStarts100ns.size(); ++i) EXPECT_EQ(rig.s->audioStarts100ns[i], 0);
}

TEST(MediaClipDecoder, AnAssetWithNoAudioTrackIsSilentAndNotWarned) {
  AudioScript audio; audio.present = false;
  Rig rig({}, audio);
  rig.d->syncMediaClock(clip(true), 1'000);
  for (rig.now = 1'000; rig.now < 1'200; rig.now += 20) {
    EXPECT_TRUE(rig.d->pollMediaAudioFrames(clip(true), rig.now).empty());
    EXPECT_TRUE(rig.d->warnings().empty());
  }
  EXPECT_EQ(rig.s->audioOpened, 0);
  const auto s = rig.next(clip(true));
  EXPECT_EQ(s.frame.frameId, 1) << "video is unaffected";
  EXPECT_TRUE(rig.d->warnings().empty());
}

TEST(MediaClipDecoder, AudioWindowsAreExactly960StereoFramesAt48k) {
  Rig rig;
  rig.d->syncMediaClock(clip(true), 1'000);
  int windows = 0;
  for (rig.now = 1'000; rig.now < 1'400; rig.now += 20) {
    for (const auto& frame : rig.d->pollMediaAudioFrames(clip(true), rig.now)) {
      ++windows;
      EXPECT_EQ(frame.sampleRate, 48000);
      EXPECT_EQ(frame.channels, 2);
      EXPECT_EQ(frame.sampleCount, 960);
      EXPECT_EQ(frame.pcm.size(), 1920u);
      EXPECT_EQ(frame.participantId, "media:clip");
      EXPECT_TRUE(frame.voiceActive);
    }
  }
  EXPECT_EQ(windows, 20) << "fixture fact: 20 polls 20 ms apart against a synchronous reader";
}

// Final-review fix. The AVFoundation factory always returns a reader and reports a failed open on
// its first take, so a loop's PRE-OPENED successor can be adopted at the seam already failed. That
// used to become a sticky open failure: one transient error froze a looping background until the
// scene changed. The seam now opens once more before it gives up.
TEST(MediaClipDecoder, ALoopSurvivesAPreopenedReaderThatFailedToOpen) {
  Script script; script.samples = 3; script.duration100ns = 3 * kFrame;
  script.failingReader = 2;  // fixture: reader 1 plays, reader 2 is the pre-open, reader 3 is the retry
  Rig rig(script);
  rig.d->syncMediaClock(clip(true, /*loop=*/true), 1'000);
  for (int i = 0; i < 3; ++i) (void)rig.next(clip(true, true));
  EXPECT_EQ(rig.s->opened, 2) << "the successor was pre-opened";
  const auto s = rig.next(clip(true, true));
  EXPECT_EQ(s.frame.frameId, 4) << "the loop went on";
  EXPECT_EQ(colour(s.frame), 0x01) << "from the top of the file";
  EXPECT_EQ(s.due100ns, 10'000'000 + 3 * kFrame);
  // Fixture fact: reader 3 replaced the failed pre-open, and its first sample pre-opened reader 4
  // (this 1 s script is shorter than the pre-open lead, as in ALoopPreopensItsNextReader...).
  EXPECT_EQ(rig.s->opened, 4) << "exactly one fresh open replaced the failed pre-open";
  EXPECT_TRUE(rig.d->warnings().empty()) << "a recovered seam is not reported as a failure";
}

TEST(MediaClipDecoder, ALoopWhoseReopenAlsoFailsSaysSoAndStops) {
  // Readers 2 (the pre-open) AND 3 (the retry) fail: bounded, loud, and no open per poll afterwards.
  struct TwoFailures final : IClipReaderFactory {
    std::shared_ptr<Shared> s; int opens = 0;
    std::unique_ptr<IClipVideoReader> openVideo(const std::string&, std::function<void()> wake, std::string&) override {
      ++opens; s->opened += 1;
      s->script.failingReader = opens >= 2 ? opens : 0;
      return std::make_unique<FakeReader>(s, std::move(wake));
    }
    bool decodeStill(const std::string&, VideoFrame&, std::string&) override { return false; }
  };
  auto shared = std::make_shared<Shared>();
  shared->script.samples = 3; shared->script.duration100ns = 3 * kFrame;
  auto factory = std::make_shared<TwoFailures>(); factory->s = shared;
  MediaClipDecoder decoder(factory, [] { return int64_t{1'000}; });
  const auto request = clip(true, true);
  decoder.syncMediaClock(request, 1'000);
  int frames = 0;
  for (int i = 0; i < 40; ++i) frames += static_cast<int>(decoder.prefetchMediaVideo(request, 1'000).size());
  EXPECT_EQ(frames, 3) << "the first pass played";
  EXPECT_EQ(factory->opens, 3) << "the pre-open and ONE retry, never an open per poll";
  ASSERT_FALSE(decoder.warnings().empty());
  EXPECT_EQ(decoder.warnings()[0], "Media background clip could not restart its loop: transient open failure");
  const auto held = decoder.pollMediaFrames(clip(false, true), 1'000);
  ASSERT_EQ(held.size(), 1u);
  EXPECT_EQ(held.front().frameId, 3) << "the last frame stays on air";
}
