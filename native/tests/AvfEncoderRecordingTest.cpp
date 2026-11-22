// AVFoundation/VideoToolbox recording encoder tests (increment 1: program
// A+V). Compiled only under COREVIDEO_WITH_AVF_ENCODER; when the factory
// returns null (adapter compiled out) the tests log + early-return green —
// the repo's skip idiom (the vendored gtest has no GTEST_SKIP).

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_AVF_ENCODER

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <sstream>
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

// The operator's MOV choice used to be ignored: every file was .mp4.
std::string containerBrand(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return {};
  char header[12] = {};
  const size_t read = std::fread(header, 1, sizeof(header), file);
  std::fclose(file);
  if (read < 12 || std::string(header + 4, 4) != "ftyp") return {};
  return std::string(header + 8, 4);
}

TEST(AvfEncoderRecording, MovFormatWritesAQuickTimeMovie) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("mov");
  auto request = testRequest(dir.string());
  request.format = "mov";
  encoder->configureRecording(request);
  (void)encoder->start({"recording"}, {});
  recordSomeAv(*encoder, 20);
  encoder->stopRecording();
  const auto session = encoder->session();
  EXPECT_EQ(session.recordingStatus, "stopped");
  EXPECT_GT(session.recordingVideoFrameCount, 0);
  ASSERT_FALSE(session.recordingArtifactPath.empty());
  EXPECT_EQ(fs::path(session.recordingArtifactPath).extension(), ".mov");
  EXPECT_EQ(containerBrand(session.recordingArtifactPath), "qt  ");

  // MP4 stays MP4.
  auto mp4 = createAVFoundationEncoderSink();
  const auto mp4Dir = freshTempDir("mp4-brand");
  mp4->configureRecording(testRequest(mp4Dir.string()));
  (void)mp4->start({"recording"}, {});
  recordSomeAv(*mp4, 20);
  mp4->stopRecording();
  const auto mp4Path = mp4->session().recordingArtifactPath;
  EXPECT_EQ(fs::path(mp4Path).extension(), ".mp4");
  EXPECT_NE(containerBrand(mp4Path), "qt  ");
  fs::remove_all(dir);
  fs::remove_all(mp4Dir);
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
IsoSourceAudio stemTick(const std::string& id, bool real, bool departed, int64_t timeline) {
  IsoSourceAudio stem;
  stem.sourceId = id;
  stem.timelineTimestamp100ns = timeline;
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

// Synthetic timeline: the sink stamps media from timelineTimestamp100ns (same
// steady-clock/100ns domain as its own now100ns()), so durations depend on the
// tick index, never on how fast the host (or a VM software encoder) runs.
int64_t steadyNow100ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
}
constexpr int64_t kTick100ns = 200000;  // 20 ms

// One 20 ms tick of a live two-guest show: Carol always live; Alice live or not.
void departureTick(IEncoderSink& encoder, int64_t& frameId, int64_t base, int tickIndex,
                   bool aliceLive, bool aliceDeparted) {
  ++frameId;
  const int64_t ts = base + static_cast<int64_t>(tickIndex) * kTick100ns;
  std::vector<IsoSourceVideoFrame> video;
  video.push_back({"zoom:102", "Carol", isoI420Frame("zoom:102", frameId, 100), ts});
  // A departed guest delivers no new pictures; the worker resubmits the last
  // frame (same id), which the per-source dedup swallows.
  video.push_back({"zoom:101", "Alice", isoI420Frame("zoom:101", aliceLive ? frameId : 1, 90), ts});
  encoder.submitIsoVideo(video);
  encoder.submitIsoAudio({stemTick("zoom:101", aliceLive, aliceDeparted, ts),
                          stemTick("zoom:102", true, false, ts)});
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
  const int64_t base = steadyNow100ns();
  int tick = 0;
  for (int i = 0; i < 25; ++i) departureTick(*encoder, frameId, base, tick++, true, false);   // together: 0.5 s
  for (int i = 0; i < 125; ++i) departureTick(*encoder, frameId, base, tick++, false, true);  // Alice gone: to 3.0 s
  encoder->stopRecording();

  const auto session = encoder->session();
  const auto& alice = isoStatus(session, "zoom:101");
  const auto& carol = isoStatus(session, "zoom:102");
  EXPECT_TRUE(alice.warning.empty()) << alice.warning;
  const auto aliceSecs = corevideo::testing::mp4DurationSeconds(alice.path);
  const auto carolSecs = corevideo::testing::mp4DurationSeconds(carol.path);
  std::ostringstream m;
  m << "aliceSecs=" << aliceSecs << " carolSecs=" << carolSecs
    << " alice{video=" << alice.videoFrameCount << " audio=" << alice.audioSampleCount
    << " droppedVideo=" << alice.droppedVideoFrames << " droppedAudio=" << alice.droppedAudioPackets
    << "} carol{video=" << carol.videoFrameCount << " audio=" << carol.audioSampleCount
    << " droppedVideo=" << carol.droppedVideoFrames << " droppedAudio=" << carol.droppedAudioPackets << "}";
  std::fprintf(stderr, "[departed-iso] %s\n", m.str().c_str());
  EXPECT_GE(aliceSecs, 0.3) << m.str();
  EXPECT_LE(aliceSecs, 0.9) << "departed ISO kept growing with silence until Stop: " << m.str();
  EXPECT_GE(carolSecs, 2.6) << "the present guest must keep recording: " << m.str();
  // The discriminating property: Alice stopped growing while Carol kept going.
  EXPECT_LT(aliceSecs, carolSecs - 1.5) << m.str();
  // Audio counts are PCM submitted (25 live ticks vs 150), not wall time.
  EXPECT_LT(alice.audioSampleCount, 48000 * 1.4) << m.str();
  EXPECT_GT(carol.audioSampleCount, 48000 * 2.4) << m.str();
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
  const int64_t base = steadyNow100ns();
  int tick = 0;
  for (int i = 0; i < 25; ++i) departureTick(*encoder, frameId, base, tick++, true, false);
  const auto atDeparture = isoStatus(encoder->session(), "zoom:101").audioSampleCount;
  for (int i = 0; i < 50; ++i) departureTick(*encoder, frameId, base, tick++, false, true);       // ~1 s gone
  EXPECT_EQ(isoStatus(encoder->session(), "zoom:101").audioSampleCount, atDeparture)
      << "no silence while departed";
  for (int i = 0; i < 25; ++i) departureTick(*encoder, frameId, base, tick++, true, false);  // back
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

// ── O2 (2026-11-22 settings quick wins): scale not crop, HEVC, honest AV1 ──

namespace {

// A Program frame whose bottom-right quadrant is bright (BGRA 230) and the rest
// dark (BGRA 30). Fixture invariant: every check samples a quadrant CENTRE, so
// the scaler's edge filtering never decides a verdict.
ProgramFrame quadrantProgramFrame(int64_t frameNumber, int width, int height) {
  ProgramFrame frame;
  frame.frameNumber = frameNumber;
  frame.width = width;
  frame.height = height;
  frame.programFullBgra.width = width;
  frame.programFullBgra.height = height;
  frame.programFullBgra.bgra.assign(static_cast<size_t>(width) * height * 4u, 30);
  for (int y = height / 2; y < height; ++y) {
    for (int x = width / 2; x < width; ++x) {
      const size_t o = (static_cast<size_t>(y) * width + x) * 4u;
      frame.programFullBgra.bgra[o + 0] = frame.programFullBgra.bgra[o + 1] = frame.programFullBgra.bgra[o + 2] = 230;
      frame.programFullBgra.bgra[o + 3] = 255;
    }
  }
  return frame;
}

int lumaAt(const corevideo::testing::DecodedMp4Video& decoded, int x, int y) {
  const auto& last = decoded.frames.back();
  const size_t o = (static_cast<size_t>(y) * decoded.width + x) * 4u;
  return (last[o] + last[o + 1] + last[o + 2]) / 3;
}

std::string recordProgram(IEncoderSink& encoder, const std::string& folder, int srcW, int srcH,
                          const std::string& codec = "h264") {
  auto request = testRequest(folder);  // writer 640x360
  request.videoCodec = codec;
  encoder.configureRecording(request);
  (void)encoder.start({"recording"}, {});
  for (int i = 1; i <= 12; ++i) {
    encoder.submit(quadrantProgramFrame(i, srcW, srcH));
    const auto pcm = tonePcm(960, 2);
    encoder.submitAudio(pcm.data(), 960, 2, 48000);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  encoder.stopRecording();
  return encoder.session().recordingArtifactPath;
}

bool fileContains(const std::string& path, const std::string& needle) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return false;
  std::string bytes;
  char chunk[65536];
  size_t read = 0;
  while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) bytes.append(chunk, read);
  std::fclose(file);
  return bytes.find(needle) != std::string::npos;
}

}  // namespace

// A 1280x720 Program into a 640x360 recording is scaled: the bright quadrant
// lands bottom-right. Mutation: restore the top-left crop -> the crop shows only
// the source's dark top-left quadrant, so (480,270) reads dark -> red.
TEST(AvfEncoderRecording, ProgramScalesToTheRequestedSizeWithoutCropping) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("o2-scale");
  const auto path = recordProgram(*encoder, dir.string(), 1280, 720);
  ASSERT_FALSE(path.empty());
  const auto decoded = corevideo::testing::decodeMp4VideoToBgra(path);
  ASSERT_TRUE(decoded.error.empty()) << decoded.error;
  ASSERT_EQ(decoded.width, 640);
  ASSERT_EQ(decoded.height, 360);
  ASSERT_FALSE(decoded.frames.empty());
  EXPECT_GT(lumaAt(decoded, 480, 270), 180) << "bottom-right quadrant missing: Program was cropped";
  EXPECT_LT(lumaAt(decoded, 160, 90), 80);
  EXPECT_LT(lumaAt(decoded, 480, 90), 80);
  EXPECT_LT(lumaAt(decoded, 160, 270), 80);
  fs::remove_all(dir);
}

// A 4:3 source (960x720) into 16:9 (640x360) is letterboxed: fit 480x360 centred,
// so x<80 and x>=560 are black bars. Mutation: stretch to the full width -> the
// bars carry picture -> red.
TEST(AvfEncoderRecording, ProgramLetterboxesAnotherAspect) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("o2-letterbox");
  const auto path = recordProgram(*encoder, dir.string(), 960, 720);
  const auto decoded = corevideo::testing::decodeMp4VideoToBgra(path);
  ASSERT_TRUE(decoded.error.empty()) << decoded.error;
  ASSERT_FALSE(decoded.frames.empty());
  EXPECT_LT(lumaAt(decoded, 40, 270), 20) << "left bar should be black";
  EXPECT_LT(lumaAt(decoded, 600, 270), 20) << "right bar should be black";
  EXPECT_GT(lumaAt(decoded, 440, 270), 180) << "bright quadrant inside the fitted picture";
  EXPECT_GT(lumaAt(decoded, 200, 90), 20) << "the dark source picture is not a bar";
  fs::remove_all(dir);
}

// H.265 is honoured and written as an hvc1 track; the session says h265.
// Mutation: ignore the codec (always H.264) -> no hvc1 box, session h264 -> red.
TEST(AvfEncoderRecording, HevcRecordingWritesAnHvc1Track) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("o2-hevc");
  const auto path = recordProgram(*encoder, dir.string(), 640, 360, "h265");
  const auto session = encoder->session();
  ASSERT_EQ(session.recordingStatus, "stopped") << session.recordingWarning;
  EXPECT_GT(session.recordingVideoFrameCount, 0);
  EXPECT_EQ(session.recordingVideoCodec, "h265");
  EXPECT_TRUE(fileContains(path, "hvc1")) << "no hvc1 sample entry: the track is not HEVC";
  EXPECT_FALSE(fileContains(path, "avc1")) << "an H.264 track was written for an H.265 request";
  fs::remove_all(dir);
}

// AV1 is refused at open, loudly, and nothing is written as H.264.
// Mutation: fall back to H.264 -> the take records and the status is not failed -> red.
TEST(AvfEncoderRecording, Av1RecordingIsRefusedLoudly) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("o2-av1");
  auto request = testRequest(dir.string());
  request.videoCodec = "av1";
  encoder->configureRecording(request);
  const auto started = encoder->start({"recording"}, {});
  EXPECT_EQ(started.recordingStatus, "failed");
  EXPECT_NE(started.recordingWarning.find("AV1"), std::string::npos) << started.recordingWarning;
  recordSomeAv(*encoder, 3);
  EXPECT_EQ(encoder->session().recordingVideoFrameCount, 0);
  bool anyMedia = false;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const auto ext = it->path().extension();
    anyMedia = anyMedia || ext == ".mp4" || ext == ".mov";
  }
  EXPECT_FALSE(anyMedia) << "an AV1 refusal must not leave a media file behind";
  fs::remove_all(dir);
}

// The session reports the real container. Mutation: the "mp4" literal -> red.
TEST(AvfEncoderRecording, MovReportsItsRealContainerAndCodec) {
  MAKE_ENCODER_OR_SKIP(encoder);
  const auto dir = freshTempDir("o2-mov-report");
  auto request = testRequest(dir.string());
  request.format = "mov";
  encoder->configureRecording(request);
  const auto started = encoder->start({"recording"}, {});
  EXPECT_EQ(started.recordingFormat, "mov");
  EXPECT_EQ(started.recordingContainerFormat, "mov");
  EXPECT_EQ(started.recordingVideoCodec, "h264");
  encoder->stopRecording();
  fs::remove_all(dir);
}
