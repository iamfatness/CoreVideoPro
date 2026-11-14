// #1186 on Mac: does the Program file end with its final picture and an A/V tail
// that agrees, or does the video track stop short of the audio?
#if COREVIDEO_WITH_AVF_ENCODER
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "AvfMp4Decode.h"
#include "modules/Interfaces.h"

namespace {
using namespace corevideo::modules;
using Clock = std::chrono::steady_clock;

ProgramFrame frame(int64_t n) {
  ProgramFrame f;
  f.frameNumber = n; f.width = 640; f.height = 360;
  f.programFullBgra.width = 640; f.programFullBgra.height = 360;
  f.programFullBgra.bgra.assign(640u * 360u * 4u, static_cast<uint8_t>(n * 7));
  return f;
}

struct Tail { std::string path; double lastMean = 0, prevMean = 0; corevideo::testing::Mp4Extents e; int64_t v = 0, a = 0; double longest = 0; };
// One AAC packet (21.3 ms); a start-vs-end-of-frame error (33 ms at 30 fps) must fail.
constexpr double kEndTolerance = 0.025;

Tail measure(double videoStopsAtSeconds, double audioStopsAtSeconds = 99) {
  auto encoder = createAVFoundationEncoderSink();
  if (!encoder) { Tail skipped; skipped.v = -1; return skipped; }
  const auto dir = std::filesystem::temp_directory_path() / "avf-tail-1186";
  std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
  RecordingSessionRequest request;
  request.sessionId = "1186"; request.targetFolder = dir.string(); request.filenamePrefix = "tail";
  request.format = "mp4"; request.width = 640; request.height = 360; request.fps = 30;
  request.targetBitrateMbps = 4.0; request.audioBitrateKbps = 160;
  encoder->configureRecording(request);
  (void)encoder->start({"recording"}, {});
  const auto t0 = Clock::now();
  std::vector<float> pcm(960 * 2, 0.1f);
  int64_t v = 0, a = 0;
  // Video every 33.33 ms, audio every 20 ms, for 6 s (the two threads of the live core).
  while (Clock::now() - t0 < std::chrono::seconds(6)) {
    const auto now = Clock::now() - t0;
    if (std::chrono::duration<double>(now).count() < videoStopsAtSeconds && now >= std::chrono::microseconds(33333) * v) { encoder->submit(frame(++v)); }
    if (std::chrono::duration<double>(now).count() < audioStopsAtSeconds && now >= std::chrono::milliseconds(20) * a) { encoder->submitAudio(pcm.data(), 960, 2, 48000); ++a; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  encoder->stopRecording();
  const auto session = encoder->session();
  const auto e = corevideo::testing::mp4TrackExtents(session.recordingArtifactPath);
  std::fprintf(stderr,
      "[1186] submitted video=%lld audio=%lld | file video: n=%zu first=%.3f lastPts=%.3f end=%.3f | audio: n=%zu first=%.3f lastPts=%.3f end=%.3f | end delta (audio-video)=%.3fs lastPts delta=%.3fs\n",
      (long long)v, (long long)a, e.video.samples, e.video.firstPts, e.video.lastPts, e.video.end,
      e.audio.samples, e.audio.firstPts, e.audio.lastPts, e.audio.end,
      e.audio.end - e.video.end, e.audio.lastPts - e.video.lastPts);
  const auto decoded = corevideo::testing::decodeMp4VideoToBgra(session.recordingArtifactPath);
  const auto mean = [](const std::vector<uint8_t>& f) { double sum = 0; for (size_t i = 0; i < f.size(); i += 4) sum += f[i + 1]; return sum / (f.size() / 4); };
  Tail tail{session.recordingWarning, decoded.frames.size() > 1 ? mean(decoded.frames.back()) : -1,
      decoded.frames.size() > 1 ? mean(decoded.frames[decoded.frames.size() - 2]) : -1, e, v, a, corevideo::testing::mp4DurationSeconds(session.recordingArtifactPath)};
  std::filesystem::remove_all(dir);
  return tail;
}

// Windows rule (03ebd1c3 / reconcileTail): the Program file's video and audio end
// together; a short tail is held/padded, never cut, and no picture is dropped.
TEST(AvfProgramTail, SteadyStopKeepsEveryPictureAndAlignedEnds) {
  const auto t = measure(99);
  if (t.v < 0) { std::fprintf(stderr, "[avf-test] skipping: factory returned null\n"); return; }
  EXPECT_EQ(static_cast<int64_t>(t.e.video.samples), t.v) << "a Program picture was lost or duplicated";
  EXPECT_TRUE(t.path.empty()) << t.path;
  EXPECT_NEAR(t.e.audio.end, t.e.video.trackEnd, kEndTolerance);
}

TEST(AvfProgramTail, VideoStalledBeforeStopHoldsTheFinalPictureToTheAudioEnd) {
  const auto t = measure(5.0);
  if (t.v < 0) { std::fprintf(stderr, "[avf-test] skipping: factory returned null\n"); return; }
  std::fprintf(stderr, "[1186] video stalled 1s: video end=%.3f trackEnd=%.3f audio end=%.3f container=%.3f frames=%zu/%lld\n",
      t.e.video.end, t.e.video.trackEnd, t.e.audio.end, t.longest, t.e.video.samples, (long long)t.v);
  EXPECT_TRUE(t.path.empty()) << t.path;
  EXPECT_NEAR(t.e.video.trackEnd, t.e.audio.end, kEndTolerance) << "video track stops short of the audio";
  EXPECT_EQ(static_cast<int64_t>(t.e.video.samples), t.v) << "the final real picture was dropped or duplicated";
  EXPECT_NEAR(t.lastMean, t.prevMean, 20.0) << "the final decoded picture is not the real final picture";
}

// Stopping while the video input is busy (video is the longer track) must not
// raise a false "could not align its A/V tail" warning.
TEST(AvfProgramTail, StopUnderVideoBackpressureRaisesNoWarning) {
  auto encoder = createAVFoundationEncoderSink();
  if (!encoder) { std::fprintf(stderr, "[avf-test] skipping: factory returned null\n"); return; }
  const auto dir = std::filesystem::temp_directory_path() / "avf-tail-bp-1186";
  std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
  RecordingSessionRequest request;
  request.sessionId = "1186b"; request.targetFolder = dir.string(); request.filenamePrefix = "bp";
  request.format = "mp4"; request.width = 1920; request.height = 1080; request.fps = 30;
  request.targetBitrateMbps = 8.0; request.audioBitrateKbps = 160;
  encoder->configureRecording(request);
  (void)encoder->start({"recording"}, {});
  std::vector<float> pcm(960 * 2, 0.1f);
  encoder->submitAudio(pcm.data(), 960, 2, 48000);
  for (int i = 1; i <= 240; ++i) {  // unpaced burst: the writer input saturates
    ProgramFrame f; f.frameNumber = i; f.width = 1920; f.height = 1080;
    f.programFullBgra.width = 1920; f.programFullBgra.height = 1080;
    f.programFullBgra.bgra.assign(1920u * 1080u * 4u, static_cast<uint8_t>(i * 3));
    f.timelineTimestamp100ns = 1'000'000'000LL + i * 333'333LL;
    encoder->submit(f);
  }
  encoder->stopRecording();
  const auto session = encoder->session();
  EXPECT_TRUE(session.recordingWarning.empty()) << session.recordingWarning;
  EXPECT_NE(session.recordingStatus, "failed");
  std::filesystem::remove_all(dir);
}

// Below the old 2-frame threshold (66 ms): still reconciled, as Windows has no threshold.
TEST(AvfProgramTail, SubThresholdVideoSkewIsStillReconciled) {
  const auto t = measure(5.96);
  if (t.v < 0) { std::fprintf(stderr, "[avf-test] skipping: factory returned null\n"); return; }
  std::fprintf(stderr, "[1186] skew: video trackEnd=%.3f audio end=%.3f\n", t.e.video.trackEnd, t.e.audio.end);
  EXPECT_TRUE(t.path.empty()) << t.path;
  EXPECT_NEAR(t.e.video.trackEnd, t.e.audio.end, kEndTolerance);
  EXPECT_EQ(static_cast<int64_t>(t.e.video.samples), t.v);
}

TEST(AvfProgramTail, SubThresholdAudioSkewIsStillPadded) {
  const auto t = measure(99, 5.93);
  if (t.v < 0) { std::fprintf(stderr, "[avf-test] skipping: factory returned null\n"); return; }
  std::fprintf(stderr, "[1186] audio skew: video trackEnd=%.3f audio end=%.3f\n", t.e.video.trackEnd, t.e.audio.end);
  EXPECT_TRUE(t.path.empty()) << t.path;
  EXPECT_NEAR(t.e.video.trackEnd, t.e.audio.end, kEndTolerance);
}

TEST(AvfProgramTail, AudioStalledBeforeStopIsPaddedWithSilenceNotCut) {
  const auto t = measure(99, 5.0);
  if (t.v < 0) { std::fprintf(stderr, "[avf-test] skipping: factory returned null\n"); return; }
  std::fprintf(stderr, "[1186] audio stalled 1s: video end=%.3f audio end=%.3f\n", t.e.video.end, t.e.audio.end);
  EXPECT_TRUE(t.path.empty()) << t.path;
  EXPECT_NEAR(t.e.video.trackEnd, t.e.audio.end, kEndTolerance) << "audio track stops short of the video";
  EXPECT_EQ(static_cast<int64_t>(t.e.video.samples), t.v);
}

// Windows ISO rule: a short video tail is held with one real sample, so the ISO
// file's tracks end together instead of the picture stopping early.
TEST(AvfProgramTail, IsoVideoStalledBeforeStopIsHeldToItsAudioEnd) {
  auto encoder = createAVFoundationEncoderSink();
  if (!encoder) { std::fprintf(stderr, "[avf-test] skipping: factory returned null\n"); return; }
  const auto dir = std::filesystem::temp_directory_path() / "avf-iso-tail-1186";
  std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
  RecordingSessionRequest request;
  request.sessionId = "1186i"; request.targetFolder = dir.string(); request.filenamePrefix = "tail";
  request.format = "mp4"; request.width = 640; request.height = 360; request.fps = 30;
  request.targetBitrateMbps = 4.0; request.audioBitrateKbps = 160;
  IsoSourceSelection guest; guest.sourceId = "zoom:7"; guest.displayName = "Guest"; guest.hasAudio = true;
  request.isoSources = {guest};
  encoder->configureRecording(request);
  (void)encoder->start({"recording"}, {"zoom:7"});
  const auto t0 = Clock::now();
  int64_t v = 0, a = 0;
  while (Clock::now() - t0 < std::chrono::seconds(4)) {
    const auto now = Clock::now() - t0;
    if (std::chrono::duration<double>(now).count() < 3.0 && now >= std::chrono::microseconds(33333) * v) {
      VideoFrame f; f.participantId = "zoom:7"; f.frameId = ++v; f.i420Width = 320; f.i420Height = 180;
      f.i420 = std::make_shared<std::vector<uint8_t>>(320u * 180u * 3 / 2, static_cast<uint8_t>(40 + v % 50));
      encoder->submitIsoVideo({{"zoom:7", "Guest", f}});
    }
    if (now >= std::chrono::milliseconds(20) * a) {
      IsoSourceAudio stem; stem.sourceId = "zoom:7"; stem.channels = 1; stem.frameCount = 960;
      stem.pcm.assign(960, 0.1f);
      encoder->submitIsoAudio({stem});
      ++a;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  encoder->stopRecording();
  const auto session = encoder->session();
  ASSERT_EQ(session.isoStreams.size(), 1u);
  const auto e = corevideo::testing::mp4TrackExtents(session.isoStreams[0].path);
  std::fprintf(stderr, "[1186] iso: frames=%zu/%lld video trackEnd=%.3f audio end=%.3f\n",
      e.video.samples, (long long)v, e.video.trackEnd, e.audio.end);
  EXPECT_TRUE(session.isoStreams[0].warning.empty()) << session.isoStreams[0].warning;
  EXPECT_TRUE(session.recordingWarning.empty()) << session.recordingWarning;
  EXPECT_NEAR(e.video.trackEnd, e.audio.end, kEndTolerance);
  EXPECT_GE(static_cast<int64_t>(e.video.samples), v);
  EXPECT_LE(static_cast<int64_t>(e.video.samples), v + 1) << "more than one held picture";
  std::filesystem::remove_all(dir);
}
}  // namespace
#endif
