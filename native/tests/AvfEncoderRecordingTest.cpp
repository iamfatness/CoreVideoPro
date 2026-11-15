// AVFoundation/VideoToolbox recording encoder tests (increment 1: program
// A+V). Compiled only under COREVIDEO_WITH_AVF_ENCODER; when the factory
// returns null (adapter compiled out) the tests log + early-return green —
// the repo's skip idiom (the vendored gtest has no GTEST_SKIP).

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_AVF_ENCODER

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "AvfMp4Decode.h"
#include "modules/Interfaces.h"

namespace {

using namespace corevideo::modules;
namespace fs = std::filesystem;

#define MAKE_ENCODER_OR_SKIP(var)                                            \
  auto var = createAVFoundationEncoderSink();                                \
  if (!var) {                                                                \
    std::fprintf(stderr, "[avf-test] skipping: factory returned null\n");    \
    return;                                                                  \
  }

RecordingSessionRequest testRequest(const std::string& folder) {
  RecordingSessionRequest request;
  request.sessionId = "avf-test-session";
  request.targetFolder = folder;
  request.filenamePrefix = "avftest";
  request.format = "mp4";
  request.width = 640;
  request.height = 360;
  request.fps = 30;
  request.targetBitrateMbps = 4.0;
  request.audioBitrateKbps = 160;
  return request;
}

ProgramFrame frameWithFullBgra(int64_t frameNumber, int width, int height) {
  ProgramFrame frame;
  frame.frameNumber = frameNumber;
  frame.width = width;
  frame.height = height;
  frame.programFullBgra.width = width;
  frame.programFullBgra.height = height;
  frame.programFullBgra.bgra.assign(static_cast<size_t>(width) * height * 4u, 0);
  // Non-uniform content so the encoder has something real to compress.
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
      frame.programFullBgra.bgra[offset + 0] = static_cast<uint8_t>(x & 0xff);
      frame.programFullBgra.bgra[offset + 1] = static_cast<uint8_t>(y & 0xff);
      frame.programFullBgra.bgra[offset + 2] = static_cast<uint8_t>((x + y + frameNumber) & 0xff);
      frame.programFullBgra.bgra[offset + 3] = 0xff;
    }
  }
  return frame;
}

std::vector<float> tonePcm(int frameCount, int channels) {
  std::vector<float> pcm(static_cast<size_t>(frameCount) * channels);
  for (int i = 0; i < frameCount; ++i) {
    const float sample = 0.25f * static_cast<float>((i % 96) - 48) / 48.f;
    for (int ch = 0; ch < channels; ++ch) {
      pcm[static_cast<size_t>(i) * channels + ch] = sample;
    }
  }
  return pcm;
}

fs::path freshTempDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / ("avf-encoder-test-" + tag);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void recordSomeAv(IEncoderSink& encoder, int frames) {
  for (int i = 1; i <= frames; ++i) {
    encoder.submit(frameWithFullBgra(i, 640, 360));
    const auto pcm = tonePcm(960, 2);
    encoder.submitAudio(pcm.data(), 960, 2, 48000);
  }
}

TEST(AvfEncoderRecording, RecordsProgramAvToPlayableFile) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("basic");
  encoder->configureRecording(testRequest(dir.string()));
  const auto started = encoder->start({"recording"}, {});
  EXPECT_TRUE(started.active);
  EXPECT_EQ(started.encoderName, "videotoolbox");
  EXPECT_EQ(started.recordingStatus, "recording");
  // The full-resolution fix: the writer records at the REQUESTED size, fed by
  // programFullBgra — never a 320x180 thumbnail into a larger container.
  EXPECT_EQ(started.recordingWidth, 640);
  EXPECT_EQ(started.recordingHeight, 360);

  recordSomeAv(*encoder, 30);
  encoder->stopRecording();

  const auto session = encoder->session();
  EXPECT_EQ(session.recordingStatus, "stopped");
  EXPECT_GT(session.recordingVideoFrameCount, 0);
  EXPECT_GT(session.recordingAudioPacketCount, 0);
  EXPECT_GT(session.recordingAudioSampleCount, 0);
  EXPECT_TRUE(session.recordingWarning.empty());
  ASSERT_FALSE(session.recordingArtifactPath.empty());
  std::error_code ec;
  const auto size = fs::file_size(session.recordingArtifactPath, ec);
  EXPECT_FALSE(ec);
  EXPECT_GT(size, 0u);
  fs::remove_all(dir);
}

TEST(AvfEncoderRecording, DoubleStartStillMuxesAudio) {
  // The #286 mirror: the live flow calls start() twice per recording
  // (start-program-output arms it, start-recording-session restarts it). The
  // second session's writer must be rebuilt from nothing, keeping its audio.
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("doublestart");
  encoder->configureRecording(testRequest(dir.string()));
  (void)encoder->start({"recording"}, {});
  recordSomeAv(*encoder, 5);
  (void)encoder->start({"recording"}, {});
  recordSomeAv(*encoder, 20);
  encoder->stopRecording();

  const auto session = encoder->session();
  EXPECT_EQ(session.recordingStatus, "stopped");
  EXPECT_GT(session.recordingVideoFrameCount, 0);
  EXPECT_GT(session.recordingAudioPacketCount, 0);
  EXPECT_TRUE(session.recordingWarning.empty());
  fs::remove_all(dir);
}

TEST(AvfEncoderRecording, FallsBackToPreviewWhenNoFullProgram) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("fallback");
  encoder->configureRecording(testRequest(dir.string()));
  (void)encoder->start({"recording"}, {});
  ProgramFrame frame;
  frame.frameNumber = 1;
  frame.preview.width = 320;
  frame.preview.height = 180;
  frame.preview.bgra.assign(320u * 180u * 4u, 0x40);
  encoder->submit(frame);
  encoder->stopRecording();
  const auto session = encoder->session();
  EXPECT_GT(session.recordingVideoFrameCount, 0);
  fs::remove_all(dir);
}

TEST(AvfEncoderRecording, BadTargetFolderFailsLoudly) {
  MAKE_ENCODER_OR_SKIP(encoder);
  auto request = testRequest("/dev/null/not-a-directory");
  encoder->configureRecording(request);
  const auto started = encoder->start({"recording"}, {});
  EXPECT_EQ(started.recordingStatus, "failed");
  EXPECT_FALSE(started.recordingWarning.empty());
  // Submits after a failed open must not crash or resurrect the session.
  recordSomeAv(*encoder, 3);
  EXPECT_EQ(encoder->session().recordingVideoFrameCount, 0);
}

TEST(AvfEncoderRecording, NonRecordingDestinationsStayIdle) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("idle");
  encoder->configureRecording(testRequest(dir.string()));
  const auto started = encoder->start({"streaming"}, {});
  EXPECT_TRUE(started.active);
  EXPECT_EQ(started.recordingStatus, "idle");
  recordSomeAv(*encoder, 3);
  EXPECT_EQ(encoder->session().recordingVideoFrameCount, 0);
  fs::remove_all(dir);
}

VideoFrame isoI420Frame(const std::string& sourceId, int64_t frameId, uint8_t yValue) {
  VideoFrame frame;
  frame.participantId = sourceId;
  frame.frameId = frameId;
  frame.i420Width = 320;
  frame.i420Height = 180;
  const size_t yLen = 320u * 180u;
  auto planes = std::make_shared<std::vector<uint8_t>>(yLen + (yLen / 4) * 2, yValue);
  frame.i420 = std::move(planes);
  return frame;
}

VideoFrame isoBgraFrame(const std::string& sourceId, int64_t frameId) {
  VideoFrame frame;
  frame.participantId = sourceId;
  frame.frameId = frameId;
  frame.pixelWidth = 256;
  frame.pixelHeight = 144;
  frame.pixelStride = 256 * 4;
  frame.pixels = std::make_shared<std::vector<uint8_t>>(256u * 144u * 4u, 0x80);
  return frame;
}

RecordingSessionRequest isoRequest(const std::string& folder) {
  auto request = testRequest(folder);
  IsoSourceSelection alice;
  alice.sourceId = "zoom:101";
  alice.displayName = "Alice";
  alice.hasAudio = true;
  IsoSourceSelection bob;
  bob.sourceId = "capture:cam1";
  bob.displayName = "Bob Jones";
  bob.hasAudio = false;  // video-only capture ISO: no fabricated stem
  request.isoSources = {alice, bob};
  return request;
}

TEST(AvfEncoderRecording, IsoWritersProduceIndependentFilesWithManifest) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("iso");
  encoder->configureRecording(isoRequest(dir.string()));
  const auto started = encoder->start({"recording"}, {"zoom:101", "capture:cam1"});
  EXPECT_EQ(started.recordingStatus, "recording");
  ASSERT_FALSE(started.recordingSessionDir.empty());
  ASSERT_FALSE(started.recordingManifestPath.empty());

  for (int i = 1; i <= 20; ++i) {
    encoder->submit(frameWithFullBgra(i, 640, 360));
    const auto pcm = tonePcm(960, 2);
    encoder->submitAudio(pcm.data(), 960, 2, 48000);
    std::vector<IsoSourceVideoFrame> iso;
    iso.push_back({"zoom:101", "Alice", isoI420Frame("zoom:101", i, 90)});
    iso.push_back({"capture:cam1", "Bob Jones", isoBgraFrame("capture:cam1", i)});
    encoder->submitIsoVideo(iso);
    IsoSourceAudio stem;
    stem.sourceId = "zoom:101";
    stem.pcm = tonePcm(960, 1);
    stem.frameCount = 960;
    stem.channels = 1;
    encoder->submitIsoAudio({stem});
  }
  encoder->stopRecording();

  const auto session = encoder->session();
  // Program never regressed by ISO work.
  EXPECT_GT(session.recordingVideoFrameCount, 0);
  EXPECT_GT(session.recordingAudioPacketCount, 0);
  ASSERT_EQ(session.isoStreams.size(), 2u);
  namespace fs = std::filesystem;
  int64_t zoomAudio = 0;
  for (const auto& stream : session.isoStreams) {
    EXPECT_TRUE(stream.trackOpen) << stream.sourceId;
    EXPECT_GT(stream.videoFrameCount, 0) << stream.sourceId;
    EXPECT_TRUE(stream.warning.empty()) << stream.warning;
    std::error_code ec;
    EXPECT_GT(fs::file_size(stream.path, ec), 0u) << stream.path;
    const auto filename = fs::path(stream.path).filename().string();
    if (stream.sourceId == "zoom:101") {
      EXPECT_EQ(filename.rfind("ISO-", 0), 0u);
      EXPECT_NE(filename.find("Alice"), std::string::npos);
      zoomAudio = stream.audioSampleCount;
    } else {
      EXPECT_NE(filename.find("Bob-Jones"), std::string::npos);
      // Video-only ISO: NO fabricated audio track.
      EXPECT_EQ(stream.audioSampleCount, 0);
    }
  }
  EXPECT_GT(zoomAudio, 0);
  std::error_code ec;
  EXPECT_GT(fs::file_size(session.recordingManifestPath, ec), 0u);
  fs::remove_all(dir);
}

TEST(AvfEncoderRecording, IsoGappedStemSilenceFillsToSharedEpoch) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("iso-gap");
  auto request = isoRequest(dir.string());
  request.isoSources.resize(1);  // Alice only
  encoder->configureRecording(request);
  (void)encoder->start({"recording"}, {"zoom:101"});

  // Open the writer with a first frame, deliver one audio burst, then submit
  // EMPTY stems (Zoom gating) for several ticks, then a resumed burst.
  std::vector<IsoSourceVideoFrame> iso;
  iso.push_back({"zoom:101", "Alice", isoI420Frame("zoom:101", 1, 120)});
  encoder->submitIsoVideo(iso);
  IsoSourceAudio real;
  real.sourceId = "zoom:101";
  real.pcm = tonePcm(960, 1);
  real.frameCount = 960;
  real.channels = 1;
  encoder->submitIsoAudio({real});
  IsoSourceAudio gap;
  gap.sourceId = "zoom:101";  // empty pcm/frameCount==0 = the gating signal
  for (int i = 0; i < 5; ++i) {
    encoder->submitIsoAudio({gap});
  }
  encoder->submitIsoAudio({real});
  encoder->stopRecording();

  const auto session = encoder->session();
  ASSERT_EQ(session.isoStreams.size(), 1u);
  // The stem accumulated real + silence samples ≥ two real bursts; the
  // wall-anchored position math itself is pinned by the portable
  // RecordingPtsClock tests — here we prove the sink WIRES it (silence was
  // actually written between the bursts, so the count exceeds 2*960).
  EXPECT_GE(session.isoStreams[0].audioSampleCount, 2 * 960);
  fs::remove_all(dir);
}

TEST(AvfEncoderRecording, IsoRefusedLoudlyOnBadSessionFolder) {
  MAKE_ENCODER_OR_SKIP(encoder);
  auto request = isoRequest("/dev/null/nope");
  encoder->configureRecording(request);
  const auto started = encoder->start({"recording"}, {"zoom:101", "capture:cam1"});
  EXPECT_EQ(started.recordingStatus, "failed");
  EXPECT_FALSE(started.recordingWarning.empty());
}

TEST(AvfEncoderRecording, IsoPerSourceDedupSkipsResubmittedFrames) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("iso-dedup");
  auto request = isoRequest(dir.string());
  request.isoSources.resize(1);
  encoder->configureRecording(request);
  (void)encoder->start({"recording"}, {"zoom:101"});
  std::vector<IsoSourceVideoFrame> iso;
  iso.push_back({"zoom:101", "Alice", isoI420Frame("zoom:101", 7, 60)});
  // The worker resubmits the latest frame every tick; same frameId must mux ONCE.
  for (int i = 0; i < 6; ++i) {
    encoder->submitIsoVideo(iso);
  }
  encoder->stopRecording();
  const auto session = encoder->session();
  ASSERT_EQ(session.isoStreams.size(), 1u);
  EXPECT_EQ(session.isoStreams[0].videoFrameCount, 1);
  fs::remove_all(dir);
}

// #1030/#1183 (Windows rule, MediaFoundationEncoderAdapter submitIsoAudio):
// once the core marks a Zoom ISO departed, its silence-only ticks are neither
// queued nor written, so the file ends at its last real media and finalizes
// with the real duration instead of growing with silence until Stop. Real
// PCM from a returning source resumes the SAME file and the clock bridges the
// gap with silence at that point.
namespace {
IsoSourceAudio stemTick(const std::string& id, bool real, bool departed) {
  IsoSourceAudio stem;
  stem.sourceId = id;
  stem.departed = departed;
  stem.channels = 1;
  if (real) {
    stem.pcm = tonePcm(960, 1);
    stem.frameCount = 960;
  }
  return stem;
}

RecordingSessionRequest twoZoomIsoRequest(const std::string& folder) {
  auto request = testRequest(folder);
  IsoSourceSelection alice;
  alice.sourceId = "zoom:101";
  alice.displayName = "Alice";
  alice.hasAudio = true;
  IsoSourceSelection carol;
  carol.sourceId = "zoom:102";
  carol.displayName = "Carol";
  carol.hasAudio = true;
  request.isoSources = {alice, carol};
  return request;
}

// One 20 ms tick of a live two-guest show: Carol always live; Alice live or not.
void departureTick(IEncoderSink& encoder, int64_t& frameId, bool aliceLive, bool aliceDeparted) {
  ++frameId;
  std::vector<IsoSourceVideoFrame> video;
  video.push_back({"zoom:102", "Carol", isoI420Frame("zoom:102", frameId, 100)});
  // A departed guest delivers no new pictures; the worker resubmits the last
  // frame (same id), which the per-source dedup swallows.
  video.push_back({"zoom:101", "Alice", isoI420Frame("zoom:101", aliceLive ? frameId : 1, 90)});
  encoder.submitIsoVideo(video);
  encoder.submitIsoAudio({stemTick("zoom:101", aliceLive, aliceDeparted),
                          stemTick("zoom:102", true, false)});
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

const IsoStreamStatus& isoStatus(const OutputSession& s, const std::string& id) {
  for (const auto& st : s.isoStreams) if (st.sourceId == id) return st;
  static const IsoStreamStatus none;
  return none;
}
}  // namespace

TEST(AvfEncoderRecording, DepartedIsoFinalizesAtItsRealDurationWhileSiblingKeepsGrowing) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("iso-departed");
  encoder->configureRecording(twoZoomIsoRequest(dir.string()));
  (void)encoder->start({"recording"}, {"zoom:101", "zoom:102"});
  int64_t frameId = 1;
  // ISO video PTS follow the wall clock (no capture timestamp is submitted) and a
  // tick takes 20 ms plus encode time, which is longer on a runner without a
  // hardware encoder. Measure each phase and assert relative to what really elapsed.
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 25; ++i) departureTick(*encoder, frameId, true, false);       // together
  const auto t1 = std::chrono::steady_clock::now();
  for (int i = 0; i < 125; ++i) departureTick(*encoder, frameId, false, true);      // Alice gone
  const auto t2 = std::chrono::steady_clock::now();
  const double togetherWall = std::chrono::duration<double>(t1 - t0).count();
  const double departedWall = std::chrono::duration<double>(t2 - t1).count();
  encoder->stopRecording();

  const auto session = encoder->session();
  const auto& alice = isoStatus(session, "zoom:101");
  const auto& carol = isoStatus(session, "zoom:102");
  EXPECT_TRUE(alice.warning.empty()) << alice.warning;
  const auto aliceSecs = corevideo::testing::mp4DurationSeconds(alice.path);
  const auto carolSecs = corevideo::testing::mp4DurationSeconds(carol.path);
  EXPECT_GT(aliceSecs, 0.2);
  EXPECT_LE(aliceSecs, togetherWall + 0.5)
      << "departed ISO kept growing with silence until Stop (together=" << togetherWall
      << "s departed=" << departedWall << "s)";
  EXPECT_GE(carolSecs, togetherWall + 0.8 * departedWall) << "the present guest must keep recording";
  // The discriminating property: Alice stopped growing while Carol kept going.
  EXPECT_LT(aliceSecs, carolSecs - 0.5 * departedWall);
  // Audio counts are PCM submitted (25 live ticks vs 150), not wall time.
  EXPECT_LT(alice.audioSampleCount, 48000 * 1.4);
  EXPECT_GT(carol.audioSampleCount, 48000 * 2.4);
  const auto decoded = corevideo::testing::decodeMp4VideoToBgra(alice.path);
  EXPECT_TRUE(decoded.error.empty()) << decoded.error;  // finalized and playable
  EXPECT_GT(decoded.frames.size(), 0u);
  fs::remove_all(dir);
}

TEST(AvfEncoderRecording, DepartedIsoThatReturnsResumesTheSameFileAndBridgesTheGap) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("iso-returned");
  encoder->configureRecording(twoZoomIsoRequest(dir.string()));
  (void)encoder->start({"recording"}, {"zoom:101", "zoom:102"});
  int64_t frameId = 1;
  for (int i = 0; i < 25; ++i) departureTick(*encoder, frameId, true, false);
  const auto atDeparture = isoStatus(encoder->session(), "zoom:101").audioSampleCount;
  for (int i = 0; i < 50; ++i) departureTick(*encoder, frameId, false, true);       // ~1 s gone
  EXPECT_EQ(isoStatus(encoder->session(), "zoom:101").audioSampleCount, atDeparture)
      << "no silence while departed";
  for (int i = 0; i < 25; ++i) departureTick(*encoder, frameId, true, false);       // back
  encoder->stopRecording();

  const auto session = encoder->session();
  const auto& alice = isoStatus(session, "zoom:101");
  EXPECT_TRUE(alice.warning.empty()) << alice.warning;
  // Same path, one file: it spans the whole take because the gap was bridged.
  EXPECT_EQ(fs::path(alice.path).filename().string().rfind("ISO-01-Alice", 0), 0u);
  EXPECT_GT(corevideo::testing::mp4DurationSeconds(alice.path), 1.8);
  EXPECT_GT(alice.audioSampleCount, atDeparture + 48000 * 0.8) << "gap not bridged on return";
  fs::remove_all(dir);
}

// I420 frame whose bottom-right quadrant is white and everything else dark grey.
VideoFrame isoQuadrantI420(const std::string& sourceId, int64_t frameId, int width, int height) {
  VideoFrame frame;
  frame.participantId = sourceId;
  frame.frameId = frameId;
  frame.i420Width = width;
  frame.i420Height = height;
  const size_t yLen = static_cast<size_t>(width) * height;
  auto planes = std::make_shared<std::vector<uint8_t>>(yLen + yLen / 2, 128);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      (*planes)[static_cast<size_t>(y) * width + x] = (x >= width / 2 && y >= height / 2) ? 235 : 60;
    }
  }
  frame.i420 = std::move(planes);
  return frame;
}

// #1203 (Windows 219ca1db): a Zoom camera ISO opens at the SELECTED resolution,
// not at whatever its first frame happens to be, and later frames of another
// size are scaled (aspect-fit), never cropped.
TEST(AvfEncoderRecording, ZoomIsoOpensAtSelectedSizeAndScalesLaterFramesWithoutCropping) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("iso-selected-size");
  auto request = testRequest(dir.string());
  IsoSourceSelection guest;
  guest.sourceId = "zoom:1";
  guest.displayName = "Guest";
  guest.hasAudio = false;
  guest.targetWidth = 1280;
  guest.targetHeight = 720;
  request.isoSources = {guest};
  encoder->configureRecording(request);
  (void)encoder->start({"recording"}, {"zoom:1"});

  // First delivered frame is 640x360 (a guest still on a low tier), the rest 1080p.
  for (int i = 1; i <= 12; ++i) {
    encoder->submit(frameWithFullBgra(i, 640, 360));
    const bool low = i == 1;
    encoder->submitIsoVideo({{"zoom:1", "Guest",
                              isoQuadrantI420("zoom:1", i, low ? 640 : 1920, low ? 360 : 1080)}});
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
  encoder->stopRecording();
  const auto session = encoder->session();
  ASSERT_EQ(session.isoStreams.size(), 1u);
  ASSERT_TRUE(session.isoStreams[0].warning.empty()) << session.isoStreams[0].warning;
  ASSERT_GT(session.isoStreams[0].videoFrameCount, 2);

  const auto decoded = corevideo::testing::decodeMp4VideoToBgra(session.isoStreams[0].path);
  ASSERT_TRUE(decoded.error.empty()) << decoded.error;
  EXPECT_EQ(decoded.width, 1280);
  EXPECT_EQ(decoded.height, 720);
  ASSERT_GT(decoded.frames.size(), 2u);
  const auto& last = decoded.frames.back();
  const auto luma = [&](int x, int y) {
    const size_t o = (static_cast<size_t>(y) * decoded.width + x) * 4u;
    return (last[o] + last[o + 1] + last[o + 2]) / 3;
  };
  // The white quadrant survives the scale (it is absent under a top-left crop).
  EXPECT_GT(luma(960, 540), 180) << "bottom-right quadrant missing: frame was cropped";
  EXPECT_LT(luma(320, 180), 110) << "top-left quadrant should stay dark";
  EXPECT_LT(luma(960, 180), 110);
  EXPECT_LT(luma(320, 540), 110);
  fs::remove_all(dir);
}

// Windows ruling: no BGRA scaler, so a size change after open is refused with a
// named warning instead of being cropped into the file.
TEST(AvfEncoderRecording, BgraIsoSizeChangeAfterOpenIsRefusedNotCropped) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("iso-bgra-resize");
  auto request = isoRequest(dir.string());
  request.isoSources = {request.isoSources[1]};  // capture:cam1 (BGRA, video-only)
  encoder->configureRecording(request);
  (void)encoder->start({"recording"}, {"capture:cam1"});
  encoder->submitIsoVideo({{"capture:cam1", "Bob Jones", isoBgraFrame("capture:cam1", 1)}});
  auto bigger = isoBgraFrame("capture:cam1", 2);
  bigger.pixelWidth = 512;
  bigger.pixelHeight = 288;
  bigger.pixelStride = 512 * 4;
  bigger.pixels = std::make_shared<std::vector<uint8_t>>(512u * 288u * 4u, 0x80);
  encoder->submitIsoVideo({{"capture:cam1", "Bob Jones", bigger}});
  encoder->stopRecording();
  const auto session = encoder->session();
  ASSERT_EQ(session.isoStreams.size(), 1u);
  EXPECT_EQ(session.isoStreams[0].videoFrameCount, 1);
  EXPECT_NE(session.isoStreams[0].warning.find("changed size 512x288"), std::string::npos)
      << session.isoStreams[0].warning;
  fs::remove_all(dir);
}

}  // namespace

#endif  // COREVIDEO_WITH_AVF_ENCODER
