// AVFoundation/VideoToolbox recording encoder tests (increment 1: program
// A+V). Compiled only under COREVIDEO_WITH_AVF_ENCODER; when the factory
// returns null (adapter compiled out) the tests log + early-return green —
// the repo's skip idiom (the vendored gtest has no GTEST_SKIP).

#if COREVIDEO_WITH_AVF_ENCODER

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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

}  // namespace

#endif  // COREVIDEO_WITH_AVF_ENCODER
