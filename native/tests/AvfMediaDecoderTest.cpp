#include "AvfMediaFixtures.h"
#include "modules/Interfaces.h"
#include "modules/MediaClipDecoder.h"
#include "modules/MediaVideoPresentation.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

// The reader seam's Apple side. Deliberately not in Interfaces.h (production
// reaches it only through createAvFoundationMediaDecoderFactory); declared here
// so the reader CONTRACT can be driven directly.
namespace corevideo::modules {
std::shared_ptr<IClipReaderFactory> createAvFoundationClipReaderFactory();
}

using namespace corevideo;
namespace {
int64_t steadyMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::filesystem::path tmp(const std::string& name) {
  return std::filesystem::temp_directory_path() /
         ("corevideo-avf-" + std::to_string(steadyMs()) + "-" + name);
}
struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code e; std::filesystem::remove(p, e); } };
modules::MediaDecodeRequest req(const std::filesystem::path& p, bool playing, bool loop = false) {
  modules::MediaDecodeRequest r;
  r.kind = "media-video"; r.sourceId = "media:fx"; r.assetId = "fx"; r.assetKind = "stinger";
  r.assetPath = p.string(); r.playing = playing; r.loop = loop;
  return r;
}
std::vector<modules::VideoFrame> pollUntilFrame(modules::IMediaDecoder& d,
    const modules::MediaDecodeRequest& r, int timeoutMs = 3000) {
  const auto deadline = steadyMs() + timeoutMs;
  while (steadyMs() < deadline) {
    auto f = d.pollMediaFrames(r, steadyMs());
    if (!f.empty()) return f;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return {};
}
std::array<int, 4> px(const modules::VideoFrame& f, int x, int y) {
  const size_t o = static_cast<size_t>(y) * f.pixelStride + static_cast<size_t>(x) * 4;
  return {(*f.pixels)[o], (*f.pixels)[o + 1], (*f.pixels)[o + 2], (*f.pixels)[o + 3]};
}
}  // namespace

TEST(AvfMediaDecoder, TheFactoryIsLiveAndTheCapabilityIsAvailable) {
  ASSERT_TRUE(static_cast<bool>(modules::createAvFoundationMediaDecoderFactory()));
  auto modulesSet = modules::createDefaultModules();
  ASSERT_TRUE(static_cast<bool>(modulesSet.mediaDecoderFactory));
  EXPECT_EQ(modulesSet.capabilityConstruction["media-clip-playback"].state, "available");
}

TEST(AvfMediaDecoder, ACuePosterIsTheFirstFrameAndOpaque) {
  // Fixture: 640x360 @30, 10 red frames then 10 blue. Red = BGRA(0x20,0x20,0xE0).
  const auto p = tmp("cue.mp4"); Cleanup c{p};
  ASSERT_EQ(corevideo::testing::writeFlatColourClip(p, 640, 360, 30, {{10, 0x20, 0x20, 0xE0}, {10, 0xE0, 0x20, 0x20}}), "");
  auto d = modules::createAvFoundationMediaDecoderFactory()();
  const auto f = pollUntilFrame(*d, req(p, false));
  ASSERT_EQ(f.size(), 1u);
  EXPECT_EQ(f[0].width, 640); EXPECT_EQ(f[0].height, 360); EXPECT_EQ(f[0].pixelStride, 640 * 4);
  const auto centre = px(f[0], 320, 180);
  EXPECT_NEAR(centre[2], 0xE0, 8); EXPECT_NEAR(centre[0], 0x20, 8);
  for (size_t o = 3; o < f[0].pixels->size(); o += 4) ASSERT_EQ((*f[0].pixels)[o], 0xff) << "opaque codec";
  EXPECT_TRUE(d->warnings().empty());
}

TEST(AvfMediaDecoder, Bt709LimitedBarsDecodeToTheirKnownRgb) {
  // Independent oracle: the quadrants' Y/Cb/Cr are fixture constants; expected RGB is the
  // BT.709 limited-range inverse, computed by hand. Margins that discriminate the failure modes:
  // full-range decoding moves black to ~16 and white to ~235 (16 off); a BT.601 matrix moves
  // green's R to ~20 (20 off). Tolerance 6 < both margins.
  // Mutation results on macOS 26.3 / M3 Max (rule 7): decoding 4:2:0 with a BT.601 matrix on the CPU
  // goes RED here. Dropping AVVideoColorPropertiesKey from the reader's output settings, or asking it
  // for a BT.601 matrix, stays GREEN: VideoToolbox converts by the SOURCE's tagged matrix either way.
  // So that key is CONFIRMATORY on this OS (rule 8), not what this test pins; the test pins the pixels.
  const auto p = tmp("bars.mp4"); Cleanup c{p};
  ASSERT_EQ(corevideo::testing::writeBt709BarsClip(p, 10, 30), "");
  auto d = modules::createAvFoundationMediaDecoderFactory()();
  const auto f = pollUntilFrame(*d, req(p, false));
  ASSERT_EQ(f.size(), 1u);
  const auto black = px(f[0], 16, 16), white = px(f[0], 48, 16), green = px(f[0], 16, 48), red = px(f[0], 48, 48);
  for (int ch = 0; ch < 3; ++ch) { EXPECT_NEAR(black[ch], 0, 6); EXPECT_NEAR(white[ch], 255, 6); }
  EXPECT_NEAR(green[2], 0, 6); EXPECT_NEAR(green[1], 255, 6); EXPECT_NEAR(green[0], 0, 6);
  EXPECT_NEAR(red[2], 255, 6); EXPECT_NEAR(red[1], 0, 6); EXPECT_NEAR(red[0], 0, 6);
}

TEST(AvfMediaDecoder, AProRes4444AlphaClipReachesTheOwnerAsStraightAlpha) {
  // Source pixel BGRA(0, 0, 200, 128), straight. Premultiplied output would read R ~100.
  const auto p = tmp("alpha.mov"); Cleanup c{p};
  const auto written = corevideo::testing::writeFlatColourClip(p, 64, 64, 30, {{10, 0, 0, 200, 128}}, /*prores4444=*/true);
  ASSERT_EQ(written, "") << "the alpha fixture could not be written on this OS: " << written;
  auto d = modules::createAvFoundationMediaDecoderFactory()();
  const auto f = pollUntilFrame(*d, req(p, false));
  ASSERT_EQ(f.size(), 1u);
  const auto v = px(f[0], 32, 32);
  EXPECT_NEAR(v[3], 128, 4); EXPECT_NEAR(v[2], 200, 6);
}

TEST(AvfMediaDecoder, PlayPauseResumeOnTheRealClockContinuesTheClip) {
  // The Windows oracle (MediaFoundationMediaFrameSource.PausingMidPlayback…, now re-gated) on the
  // prefetch path the owner actually uses. Fixture: 2 s @30, 60 frames.
  const auto p = tmp("pause.mp4"); Cleanup c{p};
  ASSERT_EQ(corevideo::testing::writeFlatColourClip(p, 128, 72, 30, {{30, 0x20, 0x20, 0xE0}, {30, 0xE0, 0x20, 0x20}}), "");
  auto d = modules::createAvFoundationMediaDecoderFactory()();
  auto* pre = dynamic_cast<modules::IMediaVideoPrefetch*>(d.get());
  ASSERT_NE(pre, nullptr);
  auto r = req(p, true);
  pre->syncMediaClock(r, steadyMs());
  int64_t held = 0;
  for (auto deadline = steadyMs() + 3000; held < 5 && steadyMs() < deadline;) {
    for (auto& s : pre->prefetchMediaVideo(r, steadyMs())) held = s.frame.frameId;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_GE(held, 5); ASSERT_LT(held, 50);
  r.playing = false; pre->syncMediaClock(r, steadyMs());
  for (auto end = steadyMs() + 300; steadyMs() < end;) {
    const auto f = d->pollMediaFrames(r, steadyMs());
    ASSERT_EQ(f.size(), 1u); EXPECT_EQ(f[0].frameId, held);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  r.playing = true; pre->syncMediaClock(r, steadyMs());
  int64_t next = held;
  for (auto deadline = steadyMs() + 2000; next == held && steadyMs() < deadline;) {
    for (auto& s : pre->prefetchMediaVideo(r, steadyMs())) next = s.frame.frameId;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(next, held + 1) << "the next frame of the same reader, not a reopen and not a skip";
}

TEST(AvfMediaDecoder, AClipEndsOnItsLastFrameAndALoopKeepsGoingWithABoundedSeam) {
  const auto p = tmp("short.mp4"); Cleanup c{p};
  ASSERT_EQ(corevideo::testing::writeFlatColourClip(p, 64, 64, 30, {{30, 0x40, 0x40, 0x40}}), "");   // 1 s
  {
    auto d = modules::createAvFoundationMediaDecoderFactory()();
    auto* pre = dynamic_cast<modules::IMediaVideoPrefetch*>(d.get());
    auto r = req(p, true); pre->syncMediaClock(r, steadyMs());
    for (auto deadline = steadyMs() + 3000; !pre->mediaEnded() && steadyMs() < deadline;) {
      (void)pre->prefetchMediaVideo(r, steadyMs());
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(pre->mediaEnded());
    EXPECT_EQ(pre->mediaDurationMs(), 1000);
    const auto held = d->pollMediaFrames(r, steadyMs());
    ASSERT_EQ(held.size(), 1u); EXPECT_EQ(held[0].frameId, 30);
  }
  {
    auto d = modules::createAvFoundationMediaDecoderFactory()();
    auto* pre = dynamic_cast<modules::IMediaVideoPrefetch*>(d.get());
    auto r = req(p, true, /*loop=*/true); pre->syncMediaClock(r, steadyMs());
    // Consume at presentation pace through MediaVideoPresentation, exactly as the owner does,
    // and measure the longest wall gap between new on-air frames across 2.5 s (2 seams).
    modules::MediaVideoPresentation presentation;
    int64_t lastId = 0, lastChangeMs = steadyMs(), worstGapMs = 0, newFrames = 0;
    for (auto end = steadyMs() + 2500; steadyMs() < end;) {
      if (presentation.hasRoom()) for (auto& s : pre->prefetchMediaVideo(r, steadyMs())) presentation.push(std::move(s));
      const auto& on = presentation.select(steadyMs() * 10000);
      if (on.hasPixels() && on.frameId != lastId) {
        if (lastId != 0) worstGapMs = std::max(worstGapMs, steadyMs() - lastChangeMs);
        lastId = on.frameId; lastChangeMs = steadyMs(); ++newFrames;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::fprintf(stderr, "[avf-loop-seam] worst gap %lld ms over %lld frames\n",
                 static_cast<long long>(worstGapMs), static_cast<long long>(newFrames));
    EXPECT_FALSE(pre->mediaEnded());
    EXPECT_GE(newFrames, 66) << "2.5 s @30 = 75 frames; a stalled seam loses many";
    EXPECT_LE(worstGapMs, 67) << "the seam may cost at most one extra frame period (2 x 33.3 ms)";
  }
}

TEST(AvfMediaDecoder, MissingAndUnsupportedFilesFailLoudly) {
  auto d = modules::createAvFoundationMediaDecoderFactory()();
  const auto missing = tmp("absent.mp4");
  EXPECT_TRUE(pollUntilFrame(*d, req(missing, true), 1500).empty());
  ASSERT_FALSE(d->warnings().empty());
  EXPECT_EQ(d->warnings()[0].rfind("Media asset fx could not be opened for Program playback: ", 0), 0u);
  EXPECT_NE(d->warnings()[0].find("does not exist"), std::string::npos);
  const auto webm = tmp("clip.webm"); Cleanup c{webm};
  { std::ofstream(webm) << "not really webm"; }
  auto w = modules::createAvFoundationMediaDecoderFactory()();
  EXPECT_TRUE(pollUntilFrame(*w, req(webm, false), 1500).empty());
  ASSERT_FALSE(w->warnings().empty());
  EXPECT_NE(w->warnings()[0].find("WebM playback on Mac needs FFmpeg"), std::string::npos);
}

TEST(AvfMediaDecoder, DestroyingMidReadNeverCallsBackIntoARetiredDecoder) {
  const auto p = tmp("churn.mp4"); Cleanup c{p};
  ASSERT_EQ(corevideo::testing::writeFlatColourClip(p, 640, 360, 60, {{120, 0x30, 0x60, 0x90}}), "");
  auto alive = std::make_shared<std::atomic<bool>>(true);
  auto lateWakes = std::make_shared<std::atomic<int>>(0);
  for (int i = 0; i < 50; ++i) {
    auto d = modules::createAvFoundationMediaDecoderFactory()();
    auto* pre = dynamic_cast<modules::IMediaVideoPrefetch*>(d.get());
    auto token = std::make_shared<std::atomic<bool>>(true);
    pre->setMediaWakeCallback([token, lateWakes] { if (!token->load()) lateWakes->fetch_add(1); });
    auto r = req(p, true); pre->syncMediaClock(r, steadyMs());
    for (int k = 0; k < i % 7; ++k) (void)pre->prefetchMediaVideo(r, steadyMs());
    d.reset();
    token->store(false);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(lateWakes->load(), 0);
}

TEST(AvfMediaDecoder, SixteenDecodersOpenAndEachDeliversAFrame) {
  // MediaTransports' cap is 16 (MediaTransports.h:552). If VideoToolbox refuses earlier, this
  // must fail with the decoder's own warning, which is what the operator would see.
  const auto p = tmp("sixteen.mp4"); Cleanup c{p};
  ASSERT_EQ(corevideo::testing::writeFlatColourClip(p, 1920, 1080, 30, {{30, 0x50, 0x50, 0x50}}), "");
  std::vector<std::unique_ptr<modules::IMediaDecoder>> decoders;
  for (int i = 0; i < 16; ++i) decoders.push_back(modules::createAvFoundationMediaDecoderFactory()());
  for (int i = 0; i < 16; ++i) {
    const auto f = pollUntilFrame(*decoders[i], req(p, false), 5000);
    EXPECT_EQ(f.size(), 1u) << "decoder " << i << ": "
        << (decoders[i]->warnings().empty() ? std::string("no warning") : decoders[i]->warnings()[0]);
  }
}

TEST(AvfMediaDecoder, AReaderNeverWakesAfterItsDestructorReturns) {
  // The reader contract itself (MediaClipDecoder.h: "no callback may run after the destructor
  // returns"), driven with nothing in front of the reader.
  // CONFIRMATORY (rule 8), and said so because the named mutation did NOT go red: with the queue
  // drain removed (3 runs), and with the drain and the wake's cancelled check both removed, this test
  // and DestroyingMidReadNeverCallsBackIntoARetiredDecoder stayed green on an M3 Max. Three layers
  // overlap (cancelReading + the cancelled flag at the top of each decode, the drain, and
  // MediaClipDecoder clearing its relay first); the window the drain closes — a decode already past
  // its cancelled check when the destructor returns — is microseconds wide and no test here hits it.
  // The drain is correct by construction, not by this assertion.
  const auto p = tmp("reader-churn.mp4"); Cleanup c{p};
  ASSERT_EQ(corevideo::testing::writeFlatColourClip(p, 640, 360, 60, {{120, 0x30, 0x60, 0x90}}), "");
  const auto readers = modules::createAvFoundationClipReaderFactory();
  ASSERT_NE(readers, nullptr);
  auto lateWakes = std::make_shared<std::atomic<int>>(0);
  int samplesTaken = 0;
  for (int i = 0; i < 50; ++i) {
    auto alive = std::make_shared<std::atomic<bool>>(true);
    std::string error;
    auto reader = readers->openVideo(p.string(), [alive, lateWakes] { if (!alive->load()) lateWakes->fetch_add(1); }, error);
    ASSERT_NE(reader, nullptr);
    // i % 7 == 0 destroys it while it is still opening; the others mid-stream, each take having
    // just scheduled the next decode on the reader's queue.
    for (int k = 0; k < i % 7; ++k) {
      modules::ClipVideoSample sample;
      for (auto deadline = steadyMs() + 3000; steadyMs() < deadline;) {
        if (reader->take(sample, error) == modules::ClipReadStatus::Ready) { ++samplesTaken; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    reader.reset();
    alive->store(false);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(samplesTaken, 147) << "fixture fact: sum of i % 7 for i in 0..49; fewer means takes timed out";
  EXPECT_EQ(lateWakes->load(), 0);
}
