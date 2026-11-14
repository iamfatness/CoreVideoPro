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
  Tail tail{{}, decoded.frames.size() > 1 ? mean(decoded.frames.back()) : -1,
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
  EXPECT_NEAR(t.e.audio.end, t.e.video.end, 0.1);
}

TEST(AvfProgramTail, VideoStalledBeforeStopHoldsTheFinalPictureToTheAudioEnd) {
  const auto t = measure(5.0);
  if (t.v < 0) { std::fprintf(stderr, "[avf-test] skipping: factory returned null\n"); return; }
  std::fprintf(stderr, "[1186] video stalled 1s: video end=%.3f trackEnd=%.3f audio end=%.3f container=%.3f frames=%zu/%lld\n",
      t.e.video.end, t.e.video.trackEnd, t.e.audio.end, t.longest, t.e.video.samples, (long long)t.v);
  EXPECT_NEAR(t.e.video.end, t.e.audio.end, 0.1) << "video track stops short of the audio";
  EXPECT_EQ(static_cast<int64_t>(t.e.video.samples), t.v) << "the final real picture was dropped or duplicated";
  EXPECT_NEAR(t.lastMean, t.prevMean, 20.0) << "the final decoded picture is not the real final picture";
}

TEST(AvfProgramTail, AudioStalledBeforeStopIsPaddedWithSilenceNotCut) {
  const auto t = measure(99, 5.0);
  if (t.v < 0) { std::fprintf(stderr, "[avf-test] skipping: factory returned null\n"); return; }
  std::fprintf(stderr, "[1186] audio stalled 1s: video end=%.3f audio end=%.3f\n", t.e.video.end, t.e.audio.end);
  EXPECT_NEAR(t.e.video.end, t.e.audio.end, 0.1) << "audio track stops short of the video";
  EXPECT_EQ(static_cast<int64_t>(t.e.video.samples), t.v);
}
}  // namespace
#endif
