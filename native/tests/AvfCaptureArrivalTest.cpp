// #984 on Mac: every camera delivery must reach the ISO recorder, not just the
// picture that happens to be current when the render tick samples.
#if defined(__APPLE__)
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <random>
#include <thread>

#include <filesystem>

#include "AvfMp4Decode.h"
#include "core/IsoVideoIngress.h"
#include "modules/AvfCaptureSessionState.h"

namespace {
using namespace corevideo::modules;
using Clock = std::chrono::steady_clock;

std::shared_ptr<std::vector<uint8_t>> tinyI420() {
  return std::make_shared<std::vector<uint8_t>>(64 * 36 * 3 / 2, 128);
}

struct Run { int published = 0; uint64_t distinct = 0, duplicateRejected = 0, overflowed = 0; };

// Camera at `camFps`, render ticks at `tickMs` +- jitter, for `seconds`.
// `useArrivals` selects the consumer: the ISO arrival queue or the latest-wins sample.
Run measure(double camFps, double tickMs, double jitterMs, double seconds, bool useArrivals) {
  AvfSessionState state("capture:cam");
  if (useArrivals) state.isoArrivals.require(true);
  corevideo::core::IsoVideoIngress ingress;
  std::atomic<bool> stop{false};
  std::atomic<int> published{0};
  const auto t0 = Clock::now();
  std::thread camera([&] {
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / camFps));
    for (int i = 0; !stop; ++i) {
      std::this_thread::sleep_until(t0 + period * i);
      state.publish(tinyI420(), 64, 36, false, false);
      ++published;
    }
  });
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> jitter(-jitterMs, jitterMs);
  double next = tickMs;
  while (std::chrono::duration<double>(Clock::now() - t0).count() < seconds) {
    std::this_thread::sleep_until(t0 + std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double, std::milli>(next + jitter(rng))));
    next += tickMs;
    std::vector<VideoFrame> sampled;
    VideoFrame held; int64_t id = 0;
    if (state.sampleLatest(held, id)) { held.participantId = "capture:cam"; sampled.push_back(held); }
    std::vector<VideoFrame> arrivals;
    if (useArrivals) arrivals = state.isoArrivals.take();
    ingress.observe({"capture:cam"}, sampled, std::move(arrivals), 1);
    (void)ingress.drain(std::chrono::milliseconds(0));
  }
  stop = true; camera.join();
  const auto c = ingress.counters()["capture:cam"];
  return {published.load(), c.distinctSubmitted, c.duplicateRejected, c.queueOverflowed};
}

// Set COREVIDEO_984_SAMPLING=1 to run the assertions against the pre-fix
// latest-wins consumer (the red-first evidence in the task report).
bool useArrivalsForAssertions() { return std::getenv("COREVIDEO_984_SAMPLING") == nullptr; }

TEST(AvfCaptureArrival, EveryCameraDeliveryReachesTheIsoRecorderOver10Seconds) {
  const auto r = measure(30.0, 33.0, 3.0, 10.0, useArrivalsForAssertions());
  std::fprintf(stderr, "[984] published=%d recorded=%llu dupRejected=%llu overflow=%llu\n", r.published,
      (unsigned long long)r.distinct, (unsigned long long)r.duplicateRejected, (unsigned long long)r.overflowed);
  EXPECT_GE(r.published, 295);
  // The camera thread can publish one more frame after the final tick.
  EXPECT_GE(static_cast<int>(r.distinct), r.published - 1) << "camera frames never recorded";
  EXPECT_LE(static_cast<int>(r.distinct), r.published) << "a camera frame was recorded twice";
  EXPECT_EQ(r.overflowed, 0u);
}

TEST(AvfCaptureArrival, FastCameraIsNotDecimatedToTheTickRate) {
  const auto r = measure(60.0, 33.0, 3.0, 4.0, useArrivalsForAssertions());
  std::fprintf(stderr, "[984] 60fps published=%d recorded=%llu\n", r.published, (unsigned long long)r.distinct);
  EXPECT_GE(static_cast<int>(r.distinct), r.published - 1);
  EXPECT_EQ(r.overflowed, 0u);
}

TEST(AvfCaptureArrival, NoIsoDemandMeansNoRetainedFrames) {
  AvfSessionState state("capture:cam");
  for (int i = 0; i < 8; ++i) state.publish(tinyI420(), 64, 36, false, false);
  EXPECT_TRUE(state.isoArrivals.take().empty());
  EXPECT_EQ(state.isoArrivals.stats().accepted, 0u);
  EXPECT_EQ(state.isoArrivals.stats().observed, 8u);
  state.isoArrivals.require(true);
  state.publish(tinyI420(), 64, 36, false, false);
  EXPECT_EQ(state.isoArrivals.take().size(), 1u);
}

TEST(AvfCaptureArrival, HeldPictureKeepsItsArrivalIdentitySoItIsRecordedOnce) {
  AvfSessionState state("capture:cam");
  state.isoArrivals.require(true);
  state.publish(tinyI420(), 64, 36, true, true);
  auto arrivals = state.isoArrivals.take();
  ASSERT_EQ(arrivals.size(), 1u);
  VideoFrame held; int64_t id = 0;
  ASSERT_TRUE(state.sampleLatest(held, id));
  EXPECT_EQ(held.frameId, arrivals[0].frameId);
  EXPECT_EQ(held.sourceEpoch, arrivals[0].sourceEpoch);
  EXPECT_EQ(held.captureTimestamp100ns, arrivals[0].captureTimestamp100ns);
  EXPECT_TRUE(held.i420FullRange);
  EXPECT_TRUE(held.i420Bt601);
  EXPECT_EQ(held.i420Width, 64);
}

#if COREVIDEO_WITH_AVF_ENCODER
// The real recorder end to end: camera deliveries -> ISO arrival queue -> ingress
// -> AVFoundation ISO writer -> decoded MP4. 10 s of 30 fps camera.
TEST(AvfCaptureArrival, IsoMp4HoldsOneFramePerCameraDeliveryOver10Seconds) {
  auto encoder = createAVFoundationEncoderSink();
  if (!encoder) { std::fprintf(stderr, "[avf-test] skipping: factory returned null\n"); return; }
  const auto dir = std::filesystem::temp_directory_path() / "avf-capture-arrival-984";
  std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
  RecordingSessionRequest request;
  request.sessionId = "984"; request.targetFolder = dir.string(); request.filenamePrefix = "cam";
  request.format = "mp4"; request.width = 640; request.height = 360; request.fps = 30;
  request.targetBitrateMbps = 4.0; request.audioBitrateKbps = 160;
  IsoSourceSelection cam; cam.sourceId = "capture:cam"; cam.displayName = "Cam"; cam.hasAudio = false;
  request.isoSources = {cam};
  encoder->configureRecording(request);
  (void)encoder->start({"recording"}, {"capture:cam"});

  AvfSessionState state("capture:cam");
  state.isoArrivals.require(useArrivalsForAssertions());
  corevideo::core::IsoVideoIngress ingress;
  std::atomic<bool> stop{false};
  std::atomic<int> published{0};
  const auto t0 = Clock::now();
  std::thread camera([&] {
    for (int i = 0; !stop; ++i) {
      std::this_thread::sleep_until(t0 + std::chrono::microseconds(33333) * i);
      auto frame = std::make_shared<std::vector<uint8_t>>(320 * 180 * 3 / 2, static_cast<uint8_t>(60 + (i % 100)));
      state.publish(std::move(frame), 320, 180, false, false);
      ++published;
    }
  });
  std::mt19937 rng(7); std::uniform_real_distribution<double> jitter(-3.0, 3.0);
  for (double next = 33.0; std::chrono::duration<double>(Clock::now() - t0).count() < 10.0; next += 33.0) {
    std::this_thread::sleep_until(t0 + std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double, std::milli>(next + jitter(rng))));
    std::vector<VideoFrame> sampled; VideoFrame held; int64_t id = 0;
    if (state.sampleLatest(held, id)) { held.participantId = "capture:cam"; sampled.push_back(held); }
    ingress.observe({"capture:cam"}, sampled, state.isoArrivals.take(), 1);
    auto batch = ingress.drain(std::chrono::milliseconds(0));
    if (!batch.empty()) encoder->submitIsoVideo(batch);
  }
  stop = true; camera.join();
  encoder->stopRecording();
  const auto session = encoder->session();
  ASSERT_EQ(session.isoStreams.size(), 1u);
  const auto decoded = corevideo::testing::decodeMp4VideoToBgra(session.isoStreams[0].path);
  const double seconds = corevideo::testing::mp4DurationSeconds(session.isoStreams[0].path);
  std::fprintf(stderr, "[984] mp4 published=%d writerFrames=%lld decodedFrames=%zu duration=%.2fs\n",
      published.load(), static_cast<long long>(session.isoStreams[0].videoFrameCount),
      decoded.frames.size(), seconds);
  EXPECT_GE(static_cast<int>(decoded.frames.size()), published.load() - 1) << "camera frames missing from the ISO file";
  EXPECT_LE(static_cast<int>(decoded.frames.size()), published.load());
  EXPECT_NEAR(seconds, 10.0, 0.4);
  std::filesystem::remove_all(dir);
}
#endif
}  // namespace
#endif
