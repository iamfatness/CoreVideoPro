#include "core/MediaCore.h"
#include "modules/Interfaces.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#endif

namespace {

std::size_t fileAsciiCount(const std::filesystem::path& path, const std::string& token) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return 0;
  }
  const std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  std::size_t count = 0;
  for (std::size_t offset = 0; (offset = bytes.find(token, offset)) != std::string::npos;
       offset += token.size()) {
    ++count;
  }
  return count;
}

bool fileContainsAscii(const std::filesystem::path& path, const std::string& token) {
  return fileAsciiCount(path, token) > 0;
}

}  // namespace

TEST(EncoderRecordingSession, StubSnapshotsStayCoherentDuringConcurrentAudioAndVideo) {
  auto encoder = corevideo::modules::createStubRecordingEncoderSink();
  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "concurrent-stub";
  request.width = 1920;
  request.height = 1080;
  request.fps = 60;
  encoder->configureRecording(request);
  (void)encoder->start({"recording"}, {});
  constexpr int count = 4000;
  std::atomic<bool> start{false};
  std::atomic<int> finished{0};
  std::thread video([&] {
    while (!start.load()) std::this_thread::yield();
    corevideo::modules::ProgramFrame frame;
    frame.width = request.width;
    frame.height = request.height;
    for (int i = 1; i <= count; ++i) {
      frame.frameNumber = i;
      encoder->submit(frame);
      if (i % 64 == 0) std::this_thread::yield();
    }
    ++finished;
  });
  std::thread audio([&] {
    while (!start.load()) std::this_thread::yield();
    const std::array<float, 960> pcm{};
    for (int i = 0; i < count; ++i) {
      encoder->submitAudio(pcm.data(), 480, 2, 48000);
      if (i % 64 == 0) std::this_thread::yield();
    }
    ++finished;
  });
  start.store(true);
  bool coherent = true;
  int snapshots = 0;
  do {
    const auto observed = encoder->session();
    coherent = coherent && observed.recordingLastFrameNumber == observed.recordingVideoFrameCount &&
        observed.recordingDurationMs == observed.recordingVideoFrameCount * 1000 / request.fps &&
        observed.recordingProgramBytesWritten == observed.recordingBytesWritten &&
        observed.recordingAudioSampleCount == observed.recordingAudioPacketCount * 480;
    ++snapshots;
  } while (finished.load() != 2);
  video.join();
  audio.join();
  const auto final = encoder->session();
  EXPECT_TRUE(coherent);
  EXPECT_GT(snapshots, 0);
  EXPECT_EQ(final.recordingVideoFrameCount, count);
  EXPECT_EQ(final.recordingAudioPacketCount, count);
  EXPECT_EQ(final.recordingAudioSampleCount, static_cast<int64_t>(count) * 480);
}

TEST(EncoderRecordingSession, StubTracksRequestedProfilePathAndFramesDeterministically) {
  auto encoder = corevideo::modules::createStubRecordingEncoderSink();
  ASSERT_NE(encoder, nullptr);

  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "show-42";
  request.targetFolder = "Recordings/CoreVideo Pro/tests";
  request.filenamePrefix = "line-cut";
  request.format = "mp4";
  request.quality = "medium";
  request.isoParticipantIds = {"host", "guest"};
  request.width = 1280;
  request.height = 720;
  request.fps = 25;
  request.videoCodec = "h264";
  request.audioCodec = "aac";
  request.audioBitrateKbps = 224;
  request.targetBitrateMbps = 8;
  encoder->configureRecording(request);

  const auto started = encoder->start({"recording"}, {});
  EXPECT_TRUE(started.active);
  EXPECT_EQ(started.recordingSessionId, "show-42");
  EXPECT_EQ(started.recordingStatus, "recording");
  EXPECT_EQ(started.recordingArtifactPath, "Recordings/CoreVideo Pro/tests/line-cut-program-0.mp4");
  EXPECT_EQ(started.recordingTargetFolder, "Recordings/CoreVideo Pro/tests");
  EXPECT_EQ(started.recordingFilenamePrefix, "line-cut");
  EXPECT_EQ(started.recordingFormat, "mp4");
  EXPECT_EQ(started.recordingQuality, "medium");
  EXPECT_EQ(started.isoParticipantIds.size(), 2u);
  EXPECT_EQ(started.targetBitrateMbps, 8);
  EXPECT_EQ(started.recordingAudioBitrateKbps, 224);
  EXPECT_FALSE(started.hardwareAccelerated);

  encoder->submit({1280, 720, 3, 10, "plan-a", "software"});
  encoder->submit({1280, 720, 3, 11, "plan-a", "software"});

  const auto session = encoder->session();
  EXPECT_EQ(session.encodedFrameCount, 2);
  EXPECT_EQ(session.recordingVideoFrameCount, 2);
  EXPECT_EQ(session.recordingLastFrameNumber, 11);
  EXPECT_EQ(session.recordingDurationMs, 80);
  EXPECT_EQ(session.recordingWidth, 1280);
  EXPECT_EQ(session.recordingHeight, 720);
  EXPECT_EQ(session.recordingFps, 25);
  EXPECT_EQ(session.recordingContainerFormat, "mp4");
  EXPECT_EQ(session.recordingVideoCodec, "h264");
  EXPECT_EQ(session.recordingAudioCodec, "aac");
  EXPECT_EQ(session.recordingAudioBitrateKbps, 224);
  EXPECT_TRUE(session.recordingBytesWritten > 0);
  EXPECT_TRUE(session.recordingMetadataValid);
  EXPECT_TRUE(session.recordingWarning.empty());
  EXPECT_TRUE(session.recordingError.empty());
}

TEST(EncoderRecordingSession, StubMuxesRealProgramAudioPacketsAndSamples) {
  auto encoder = corevideo::modules::createStubRecordingEncoderSink();
  ASSERT_NE(encoder, nullptr);

  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "audio-show";
  request.targetFolder = "Recordings/CoreVideo Pro/tests";
  request.filenamePrefix = "with-audio";
  request.format = "mp4";
  request.width = 1920;
  request.height = 1080;
  request.fps = 30;
  request.videoCodec = "h264";
  request.audioCodec = "aac";
  encoder->configureRecording(request);
  encoder->start({"recording"}, {});

  // Two stereo audio packets (real interleaved PCM), 1024 sample-frames each.
  std::vector<float> pcm(1024 * 2, 0.25f);
  encoder->submitAudio(pcm.data(), 1024, 2, 48000);
  encoder->submitAudio(pcm.data(), 1024, 2, 48000);

  const auto session = encoder->session();
  EXPECT_EQ(session.recordingAudioPacketCount, 2);
  EXPECT_EQ(session.recordingAudioSampleCount, 2048);
  EXPECT_EQ(session.recordingAudioChannels, 2);
  EXPECT_EQ(session.recordingAudioSampleRate, 48000);
  EXPECT_TRUE(session.recordingBytesWritten > 0);
}

TEST(EncoderRecordingSession, StubIgnoresAudioWhenRecordingNotSelected) {
  auto encoder = corevideo::modules::createStubRecordingEncoderSink();
  ASSERT_NE(encoder, nullptr);

  corevideo::modules::RecordingSessionRequest request;
  request.audioCodec = "aac";
  encoder->configureRecording(request);
  // Only an output destination, no "recording" target.
  encoder->start({"rtmp"}, {});

  std::vector<float> pcm(960 * 2, 0.1f);
  encoder->submitAudio(pcm.data(), 960, 2, 48000);

  const auto session = encoder->session();
  EXPECT_EQ(session.recordingAudioPacketCount, 0);
  EXPECT_EQ(session.recordingAudioSampleCount, 0);
}

TEST(EncoderRecordingSession, StubStopRecordingFreezesRecordingAccumulation) {
  auto encoder = corevideo::modules::createStubRecordingEncoderSink();
  ASSERT_NE(encoder, nullptr);

  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "stop-show";
  request.targetFolder = "Recordings/CoreVideo Pro/tests";
  request.filenamePrefix = "stop-cut";
  request.format = "mp4";
  request.width = 1280;
  request.height = 720;
  request.fps = 30;
  request.videoCodec = "h264";
  request.audioCodec = "aac";
  encoder->configureRecording(request);
  encoder->start({"recording", "rtmp"}, {});

  std::vector<float> pcm(1024 * 2, 0.25f);
  encoder->submit({1280, 720, 3, 1, "plan-a", "software"});
  encoder->submitAudio(pcm.data(), 1024, 2, 48000);

  encoder->stopRecording();
  const auto stopped = encoder->session();
  EXPECT_EQ(stopped.recordingStatus, "stopped");
  // The encoder session survives a recording stop — streaming destinations
  // continue; only the recording container is finalized.
  EXPECT_TRUE(stopped.active);

  encoder->submit({1280, 720, 3, 2, "plan-a", "software"});
  encoder->submitAudio(pcm.data(), 1024, 2, 48000);

  const auto after = encoder->session();
  EXPECT_EQ(after.recordingVideoFrameCount, stopped.recordingVideoFrameCount);
  EXPECT_EQ(after.recordingAudioPacketCount, stopped.recordingAudioPacketCount);
  EXPECT_EQ(after.recordingBytesWritten, stopped.recordingBytesWritten);
  EXPECT_EQ(after.encodedFrameCount, stopped.encodedFrameCount + 1);
}

TEST(EncoderRecordingSession, MediaCoreStopRecordingSessionFinalizesEncoderRecording) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-recording-targets"},
          {"targetFolder", "Recordings/CoreVideo Pro/tests"},
          {"filenamePrefix", "finalize-cut"},
          {"format", "mp4"},
          {"quality", "medium"},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-recording-session"},
          {"sessionId", "session-finalize"},
          {"startedAtMs", 100},
      },
  });

  const auto stoppedState = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "stop-recording-session"},
          {"reason", "Operator stopped recording."},
      },
  });

  const auto* encoderSession = stoppedState.get("encoderSession");
  ASSERT_NE(encoderSession, nullptr);
  const double frozenFrameCount = encoderSession->get("recordingFrameCount")->asNumber();

  // Further ticks keep encoding (streaming may continue) but must NOT
  // accumulate into the finalized recording.
  const auto laterState = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "recommend-auto-production"}},
  });
  const auto* laterSession = laterState.get("encoderSession");
  ASSERT_NE(laterSession, nullptr);
  EXPECT_EQ(laterSession->get("recordingFrameCount")->asNumber(), frozenFrameCount);
}

TEST(EncoderRecordingSession, MediaCorePropagatesRecordingTargetsIntoEncoderSession) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-recording-targets"},
          {"targetFolder", "Recordings/CoreVideo Pro/tests"},
          {"filenamePrefix", "director-cut"},
          {"format", "mp4"},
          {"quality", "medium"},
          {"isoParticipantIds", corevideo::rpc::Json::Array{"host"}},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-recording-session"},
          {"sessionId", "session-b4"},
          {"startedAtMs", 100},
      },
  });

  const auto* encoderSession = state.get("encoderSession");
  ASSERT_NE(encoderSession, nullptr);
  EXPECT_EQ(encoderSession->getString("status"), "encoding");
  EXPECT_EQ(encoderSession->getString("recordingArtifactPath"), "Recordings/CoreVideo Pro/tests/director-cut-program-0.mp4");
  EXPECT_GE(encoderSession->get("recordingFrameCount")->asNumber(), 1);
  EXPECT_TRUE(encoderSession->get("recordingMetadataValid")->asBool());

  const auto health = mediaCore.health();
  EXPECT_EQ(health.getString("recordingArtifactPath"), "Recordings/CoreVideo Pro/tests/director-cut-program-0.mp4");
  EXPECT_GE(health.get("recordingFrameCount")->asNumber(), 1);
  EXPECT_TRUE(health.get("recordingMetadataValid")->asBool());

  const auto* recording = state.get("recording");
  ASSERT_NE(recording, nullptr);
  EXPECT_EQ(recording->getString("sessionId"), "session-b4");
  EXPECT_EQ(recording->getString("artifactPath"), "Recordings/CoreVideo Pro/tests/director-cut-program-0.mp4");
  EXPECT_EQ(recording->get("proof")->getString("containerFormat"), "mp4");
  EXPECT_EQ(recording->get("proof")->getString("videoCodec"), "h264");
  EXPECT_EQ(recording->get("proof")->getString("audioCodec"), "aac");
}

TEST(EncoderRecordingSession, MediaCorePropagatesRecordingRenderProfileFromRecordingTargets) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-recording-targets"},
          {"targetFolder", "Recordings/CoreVideo Pro/tests"},
          {"filenamePrefix", "profile-cut"},
          {"format", "mkv"},
          {"quality", "archive"},
          {"targetBitrateMbps", 24.0},
          {"audioBitrateKbps", 256},
          {"renderProfile",
           corevideo::rpc::Json::Object{
               {"profileId", "recording-720p30-h265"},
               {"resolution", "1280x720"},
               {"width", 1280},
               {"height", 720},
               {"fps", 30},
               {"targetBitrateMbps", 24.0},
               {"codec", "h265"},
           }},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-recording-session"},
          {"sessionId", "session-profile"},
          {"startedAtMs", 100},
      },
  });

  const auto* encoderSession = state.get("encoderSession");
  ASSERT_NE(encoderSession, nullptr);
  const auto* encoderTargetBitrate = encoderSession->get("targetBitrateMbps");
  ASSERT_NE(encoderTargetBitrate, nullptr);
  EXPECT_EQ(encoderTargetBitrate->asNumber(), 24.0);
  EXPECT_EQ(encoderSession->getString("recordingFormat"), "mkv");
  EXPECT_EQ(encoderSession->getString("recordingVideoCodec"), "h265");
  EXPECT_EQ(encoderSession->getString("recordingAudioCodec"), "aac");
  EXPECT_EQ(encoderSession->get("recordingAudioBitrateKbps")->asNumber(), 256);

  const auto* recording = state.get("recording");
  ASSERT_NE(recording, nullptr);
  const auto* proof = recording->get("proof");
  ASSERT_NE(proof, nullptr);
  EXPECT_EQ(proof->getString("containerFormat"), "mkv");
  EXPECT_EQ(proof->get("width")->asNumber(), 1280);
  EXPECT_EQ(proof->get("height")->asNumber(), 720);
  EXPECT_EQ(proof->get("frameRate")->asNumber(), 30);
  const auto* proofTargetBitrate = proof->get("targetBitrateMbps");
  ASSERT_NE(proofTargetBitrate, nullptr);
  EXPECT_EQ(proofTargetBitrate->asNumber(), 24.0);
  EXPECT_EQ(proof->get("audioBitrateKbps")->asNumber(), 256);
}

// --- RecordingPtsClock (audio overhaul spec 4.3 / R4) ---------------------------

#include "modules/RecordingPtsClock.h"

namespace {
constexpr std::int64_t kMs = 10'000;  // 100ns ticks per millisecond
}

TEST(RecordingPtsClock, VideoPtsTracksWallClockAndDedupsRepeatedFrames) {
  corevideo::modules::RecordingPtsClock clock;

  // The audio worker submits the LATEST program frame every 20ms tick; the
  // render produces new frames at ~60fps, so frame numbers repeat.
  const auto first = clock.videoPts(0, 100);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, 0);

  EXPECT_FALSE(clock.videoPts(20 * kMs, 100).has_value());  // duplicate frame

  const auto second = clock.videoPts(40 * kMs, 102);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*second, 40 * kMs);  // wall time, NOT frame-count * 1/fps
  EXPECT_EQ(clock.lastVideoPts100ns(), 40 * kMs);
}

TEST(RecordingPtsClock, AudioAnchorsToSharedEpochAndCountsSamples) {
  corevideo::modules::RecordingPtsClock clock;

  // Video starts the epoch at t=0; the first audio buffer lands 100ms later
  // and must carry that offset instead of pretending both tracks start at 0.
  ASSERT_TRUE(clock.videoPts(0, 1).has_value());
  const auto audioFirst = clock.audioPts(100 * kMs, 960, 48000);
  EXPECT_EQ(audioFirst, 100 * kMs);

  // Thereafter the track is sample-counted (gapless), regardless of when the
  // submit call arrives: 960 samples at 48kHz = exactly 20ms after the base.
  const auto audioSecond = clock.audioPts(137 * kMs, 960, 48000);
  EXPECT_EQ(audioSecond, 100 * kMs + 20 * kMs);
}

TEST(RecordingPtsClock, TenSimulatedSecondsStayAligned) {
  corevideo::modules::RecordingPtsClock clock;

  // Simulate the real cadences: the audio worker ticks every 20ms, submitting
  // the latest 60fps program frame (duplicates included) and 20ms of audio.
  // Under the OLD per-track counters this scenario drifted ~1.7s apart in 10s
  // (video: ~50 muxed frames/s tagged 1/60s = 0.83x real time).
  std::int64_t lastVideo = 0;
  std::int64_t lastAudio = 0;
  for (int tick = 0; tick < 500; ++tick) {          // 10s of 20ms ticks
    const std::int64_t now = tick * 20 * kMs;
    const std::int64_t frameNumber = (now * 60) / (1000 * kMs);  // 60fps render
    if (const auto pts = clock.videoPts(now, frameNumber)) {
      lastVideo = *pts;
    }
    lastAudio = clock.audioPts(now, 960, 48000);
  }
  // Both timelines must end within one tick of each other (shared clock).
  const std::int64_t divergence = lastVideo > lastAudio ? lastVideo - lastAudio : lastAudio - lastVideo;
  EXPECT_TRUE(divergence <= 20 * kMs) << "A/V PTS divergence " << divergence / kMs << "ms";
  // And the video timeline must track real time, not muxed-frame count.
  EXPECT_TRUE(lastVideo >= 9'900 * kMs) << "video timeline ended at " << lastVideo / kMs << "ms";
}

// --- ISO-1: per-(sourceId,frameId) video dedup on the SHARED epoch ----------

TEST(RecordingPtsClock, IsoVideoDedupsPerSourceOnSharedEpoch) {
  corevideo::modules::RecordingPtsClock clock;

  // Program frame at t=0 anchors the shared epoch.
  ASSERT_TRUE(clock.videoPts(0, 1).has_value());

  // Two ISO sources advance on their OWN frameIds. Both share the epoch, so a
  // clap on program lands at the same timeline position on every ISO.
  const auto a0 = clock.videoPtsForSource(10 * kMs, "zoom:A", 100);
  ASSERT_TRUE(a0.has_value());
  EXPECT_EQ(*a0, 10 * kMs);  // wall time on the shared epoch, not 0

  // Source B is INDEPENDENT of A — its dedup + monotonic pts are per-source, so
  // its first frame carries the shared-epoch wall time with no cross-source bump.
  const auto b0 = clock.videoPtsForSource(10 * kMs, "zoom:B", 500);
  ASSERT_TRUE(b0.has_value());
  EXPECT_EQ(*b0, 10 * kMs);

  // Re-submitting the SAME (source, frameId) each tick muxes once per source.
  EXPECT_FALSE(clock.videoPtsForSource(30 * kMs, "zoom:A", 100).has_value());
  EXPECT_FALSE(clock.videoPtsForSource(30 * kMs, "zoom:B", 500).has_value());

  // A fresh frameId for A advances A only; B's dedup is independent.
  const auto a1 = clock.videoPtsForSource(50 * kMs, "zoom:A", 101);
  ASSERT_TRUE(a1.has_value());
  EXPECT_EQ(*a1, 50 * kMs);
  EXPECT_FALSE(clock.videoPtsForSource(50 * kMs, "zoom:B", 500).has_value());
}

TEST(RecordingPtsClock, IsoVideoStaysStrictlyMonotonicPerSource) {
  corevideo::modules::RecordingPtsClock clock;
  // Two frames of the same source in the SAME 100ns instant must not collide.
  const auto first = clock.videoPtsForSource(5 * kMs, "zoom:A", 1);
  const auto second = clock.videoPtsForSource(5 * kMs, "zoom:A", 2);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_GT(*second, *first);
}

// ISO-3: per-(sourceId,frameId) dedup is scheme-agnostic — a capture source
// (`capture:<id>`, its own advancing frameId) dedups independently of a Zoom
// source, both on the ONE shared program epoch. This is the capture leg of the
// same per-source clock ISO-1 proved for Zoom (the fake engine is Zoom-only, so
// this pure test is the capture-frameId coverage; §10 harness-gap note).
TEST(RecordingPtsClock, IsoVideoDedupsCaptureSourceIndependentlyOfZoom) {
  corevideo::modules::RecordingPtsClock clock;
  ASSERT_TRUE(clock.videoPts(0, 1).has_value());  // program anchors the epoch

  const auto cam0 = clock.videoPtsForSource(10 * kMs, "capture:cam0", 7);
  const auto zoomA = clock.videoPtsForSource(10 * kMs, "zoom:A", 7);  // same frameId, other src
  ASSERT_TRUE(cam0.has_value());
  ASSERT_TRUE(zoomA.has_value());
  EXPECT_EQ(*cam0, 10 * kMs);   // shared-epoch wall time
  EXPECT_EQ(*zoomA, 10 * kMs);  // no cross-source collision despite equal frameId

  // The capture reader holds its last frame on a stall (CaptureReaderStallPolicy):
  // the same frameId re-submitted every tick muxes exactly once — no churn.
  EXPECT_FALSE(clock.videoPtsForSource(30 * kMs, "capture:cam0", 7).has_value());
  EXPECT_FALSE(clock.videoPtsForSource(50 * kMs, "capture:cam0", 7).has_value());
  // A fresh capture frameId advances capture only.
  const auto cam1 = clock.videoPtsForSource(70 * kMs, "capture:cam0", 8);
  ASSERT_TRUE(cam1.has_value());
  EXPECT_EQ(*cam1, 70 * kMs);
}

// --- ISO-2: per-source raw-stem audio silence-fill on the SHARED epoch -------
//
// THE key correctness test (spec §10): a Zoom guest's isolate_audio is gated —
// it stops between talk bursts — so a stem must silence-fill the gap and land
// the NEXT real buffer at the correct, program-aligned sample position, never
// drifting earlier. Drives isoAudioAdvance the way the audio worker does: one
// call per ~20ms tick per source, empty (frameCount==0) when the source is
// silent, real (960 frames) when it talks.
TEST(RecordingPtsClock, IsoAudioSilenceFillKeepsGappedStemAligned) {
  corevideo::modules::RecordingPtsClock clock;
  constexpr int kRate = 48000;
  constexpr int kTickFrames = 960;  // 20ms at 48kHz

  // Program frame at t=0 anchors the shared epoch (the stem is aligned to THIS).
  ASSERT_TRUE(clock.videoPts(0, 1).has_value());

  const std::string src = "zoom:A";

  // Ticks 0..4: the guest talks — five contiguous 960-frame buffers from t=0.
  for (int tick = 0; tick < 5; ++tick) {
    const std::int64_t now = tick * 20 * kMs;
    const auto adv = clock.isoAudioAdvance(now, src, kTickFrames, kRate);
    EXPECT_EQ(adv.silenceFrames, 0) << "tick " << tick << " should need no silence";
    EXPECT_EQ(adv.realPts100ns, static_cast<std::int64_t>(tick) * 20 * kMs);
  }
  // 5 contiguous ticks written == 5 * 960 frames.
  EXPECT_EQ(clock.isoAudioSamples(src), 5 * kTickFrames);

  // Ticks 5..9: the guest goes SILENT — the worker still submits every tick with
  // an EMPTY buffer, so the stem's timeline advances by silence alone. The gap is
  // filled by real silence (the exact per-tick split straddles a tick boundary —
  // tick 4's real buffer already reached the 100ms mark — so assert the TOTAL).
  std::int64_t totalSilence = 0;
  for (int tick = 5; tick < 10; ++tick) {
    const std::int64_t now = tick * 20 * kMs;
    const auto adv = clock.isoAudioAdvance(now, src, 0, kRate);
    totalSilence += adv.silenceFrames;
  }
  EXPECT_EQ(totalSilence, 4 * kTickFrames) << "5 silent ticks fill the 80ms gap after t=100ms";

  // Tick 10: the guest RESUMES. The real buffer must land at the position that
  // matches wall time on the shared epoch (10 ticks == 200ms == 9600 frames),
  // NOT back-to-back after tick 4 (that would be a 100ms drift EARLIER of
  // program). Silence-fill covered ticks 5..9 exactly.
  const std::int64_t resumeNow = 10 * 20 * kMs;
  const auto resume = clock.isoAudioAdvance(resumeNow, src, kTickFrames, kRate);
  EXPECT_EQ(resume.realPts100ns, 200 * kMs) << "resumed buffer must land at 200ms, not drift";
  EXPECT_EQ(clock.isoAudioSamples(src), 10 * kTickFrames + kTickFrames);

  // Two sources are INDEPENDENT (own accumulators) but share the one epoch, so a
  // clap at the same wall time lands at the same position on both stems.
  const auto a = clock.isoAudioAdvance(300 * kMs, "zoom:X", kTickFrames, kRate);
  const auto b = clock.isoAudioAdvance(300 * kMs, "zoom:Y", kTickFrames, kRate);
  EXPECT_EQ(a.realPts100ns, b.realPts100ns);
  EXPECT_EQ(a.realPts100ns, 300 * kMs);
}

// A stem that FIRST delivers audio well after the epoch (a guest silent for the
// show's opening) silence-fills from the epoch so its track still starts at t=0,
// staying aligned to program — the leading silence is chunked by the writer.
TEST(RecordingPtsClock, IsoAudioLateStartSilenceFillsFromEpoch) {
  corevideo::modules::RecordingPtsClock clock;
  ASSERT_TRUE(clock.videoPts(0, 1).has_value());  // epoch at t=0
  // First audio for this source arrives at t=5s.
  const auto adv = clock.isoAudioAdvance(5000 * kMs, "zoom:Late", 960, 48000);
  EXPECT_EQ(adv.silencePts100ns, 0) << "leading silence must start at the epoch (t=0)";
  EXPECT_EQ(adv.silenceFrames, 5 * 48000) << "5s of leading silence at 48kHz";
  EXPECT_EQ(adv.realPts100ns, 5000 * kMs) << "real audio lands at its wall position";
}

// ---------------------------------------------------------------------------
// Regression: the 2026-07-13 alpha-blocking zero-audio-recording bug.
//
// The live command flow calls encoder->start() TWICE for one recording:
// start-program-output arms the encoder (generation 1 writers), then
// start-recording-session restarts it (generation 2 writers). Mp4Writer was
// reused across generations WITHOUT resetting audioConfigured_/stream indices,
// so ensureAudioStream skipped AddStream on the generation-2 sink writer and
// every audio WriteSample failed with MF_E_INVALIDSTREAMNUMBER (0xC00D36B3) —
// a video-only MP4 while the master bus carried live program audio.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_MF_ENCODER

TEST(EncoderRecordingSession, MediaFoundationRestartedSessionStillMuxesAudio) {
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) {
    // Media Foundation unavailable (MFStartup failed) — nothing to test here.
    // (This vendored gtest predates GTEST_SKIP.)
    return;
  }

  namespace fs = std::filesystem;
  std::error_code cleanupError;
  const auto targetDir = fs::temp_directory_path() / "corevideo-mf-audio-regression";
  fs::remove_all(targetDir, cleanupError);

  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "mf-audio-regression";
  request.targetFolder = targetDir.string();
  request.filenamePrefix = "restart";
  request.format = "mp4";
  request.quality = "high";
  request.width = 640;
  request.height = 360;
  request.fps = 30;
  request.videoCodec = "h264";
  request.audioCodec = "aac";
  request.audioBitrateKbps = 128;
  request.targetBitrateMbps = 4;
  encoder->configureRecording(request);

  // Generation 1 (start-program-output), then generation 2
  // (start-recording-session) — the restart that used to lose the AAC stream.
  encoder->start({"recording"}, {});
  encoder->start({"recording"}, {});

  corevideo::modules::ProgramFrame frame;
  frame.width = 640;
  frame.height = 360;
  frame.frameNumber = 1;
  frame.preview.width = 640;
  frame.preview.height = 360;
  frame.preview.bgra.assign(static_cast<size_t>(640) * 360 * 4, 0x40);
  encoder->submit(frame);

  std::vector<float> pcm(static_cast<size_t>(960) * 2, 0.25f);  // 20ms stereo @48k
  encoder->submitAudio(pcm.data(), 960, 2, 48000);
  encoder->submitAudio(pcm.data(), 960, 2, 48000);

  const auto session = encoder->session();
  EXPECT_GT(session.recordingAudioPacketCount, 0)
      << "generation-2 writer muxed no audio; warning: " << session.recordingWarning;
  EXPECT_EQ(session.recordingAudioSampleCount, 1920);  // per-generation counters reset
  EXPECT_TRUE(session.recordingWarning.empty()) << session.recordingWarning;

  encoder->stopRecording();
  encoder.reset();
  fs::remove_all(targetDir, cleanupError);
}

// Program recording must mux the FULL-RESOLUTION program readback, never the
// 320x180 `preview` thumbnail. Writing the thumbnail into a writer opened at
// program dimensions put the entire show into a small corner of a black frame —
// shipped that way for months (a 2026-07-13 recording: 8995 frames, flat luma 4)
// because no validator ever looked at the pixels, only at stream presence.
// Pinned two ways: a full-res-only frame MUST mux (the old code early-returned
// on the empty preview and wrote nothing), and once full-res has been seen a
// later preview-only tick must NOT be written at the wrong geometry.
TEST(EncoderRecordingSession, MediaFoundationProgramRecordingUsesFullResNotPreview) {
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) {
    return;  // Media Foundation unavailable
  }

  namespace fs = std::filesystem;
  std::error_code cleanupError;
  const auto targetDir = fs::temp_directory_path() / "corevideo-mf-fullres-regression";
  fs::remove_all(targetDir, cleanupError);

  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "mf-fullres";
  request.targetFolder = targetDir.string();
  request.filenamePrefix = "fullres";
  request.format = "mp4";
  request.quality = "high";
  request.width = 640;
  request.height = 360;
  request.fps = 30;
  request.videoCodec = "h264";
  request.audioCodec = "aac";
  request.targetBitrateMbps = 4;
  encoder->configureRecording(request);
  encoder->start({"recording"}, {});

  // FULL-RES ONLY: no preview at all. The old code read `preview`, found it
  // empty, and returned without muxing anything.
  corevideo::modules::ProgramFrame full;
  full.width = 640;
  full.height = 360;
  full.frameNumber = 1;
  full.programFullBgra.width = 640;
  full.programFullBgra.height = 360;
  full.programFullBgra.bgra.assign(static_cast<size_t>(640) * 360 * 4, 0xC0);
  encoder->submit(full);

  const auto afterFull = encoder->session();
  EXPECT_EQ(afterFull.recordingVideoFrameCount, 1)
      << "full-resolution program frame was not muxed; warning: " << afterFull.recordingWarning;

  // Now a preview-only tick (the async tap missed this one). Having locked to
  // full-res, it must be SKIPPED rather than written at thumbnail geometry.
  corevideo::modules::ProgramFrame previewOnly;
  previewOnly.width = 640;
  previewOnly.height = 360;
  previewOnly.frameNumber = 2;
  previewOnly.preview.width = 320;
  previewOnly.preview.height = 180;
  previewOnly.preview.bgra.assign(static_cast<size_t>(320) * 180 * 4, 0x20);
  encoder->submit(previewOnly);

  EXPECT_EQ(encoder->session().recordingVideoFrameCount, 1)
      << "a 320x180 preview was muxed after full-res was established — that is the "
         "mid-file geometry flip that produces the corner-tile frame";

  encoder->stopRecording();
  encoder.reset();
  fs::remove_all(targetDir, cleanupError);
}

// WINDOWS program recording: the full-resolution program exists only as NV12
// (compositor->takeVcamNv12 -> ProgramFrame::programNv12; programFullBgra is
// macOS-only), so a request carrying programNv12 must open the writer with an
// NV12 media type and mux that buffer. Before this, Windows fell through to the
// 320x180 preview and put the whole show in a corner of a black frame.
TEST(EncoderRecordingSession, MediaFoundationProgramRecordingMuxesNv12ProgramTap) {
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) {
    return;  // Media Foundation unavailable
  }

  namespace fs = std::filesystem;
  std::error_code cleanupError;
  const auto targetDir = fs::temp_directory_path() / "corevideo-mf-nv12-program";
  fs::remove_all(targetDir, cleanupError);

  constexpr int kW = 640;
  constexpr int kH = 360;
  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "mf-nv12-program";
  request.targetFolder = targetDir.string();
  request.filenamePrefix = "nv12";
  request.format = "mp4";
  request.quality = "high";
  request.width = kW;
  request.height = kH;
  request.fps = 30;
  request.videoCodec = "h264";
  request.audioCodec = "aac";
  request.targetBitrateMbps = 4;
  request.programNv12 = true;  // the compositor supplies the NV12 program tap
  encoder->configureRecording(request);
  encoder->start({"recording"}, {});

  // NV12 program tap only — no preview at all. The pre-fix code read `preview`,
  // found it empty, and muxed nothing.
  corevideo::modules::ProgramFrame frame;
  frame.width = kW;
  frame.height = kH;
  frame.frameNumber = 1;
  frame.programNv12Width = kW;
  frame.programNv12Height = kH;
  frame.programNv12.assign(static_cast<size_t>(kW) * kH * 3 / 2, 0x80);
  encoder->submit(frame);

  const auto afterNv12 = encoder->session();
  EXPECT_EQ(afterNv12.recordingVideoFrameCount, 1)
      << "NV12 program tap was not muxed; warning: " << afterNv12.recordingWarning;
  EXPECT_TRUE(afterNv12.recordingWarning.empty()) << afterNv12.recordingWarning;

  // A tick where the async tap produced nothing must be SKIPPED, never written
  // from the thumbnail — the writer's media type is NV12 and the geometry would
  // be wrong regardless.
  corevideo::modules::ProgramFrame previewOnly;
  previewOnly.width = kW;
  previewOnly.height = kH;
  previewOnly.frameNumber = 2;
  previewOnly.preview.width = 320;
  previewOnly.preview.height = 180;
  previewOnly.preview.bgra.assign(static_cast<size_t>(320) * 180 * 4, 0x20);
  encoder->submit(previewOnly);
  EXPECT_EQ(encoder->session().recordingVideoFrameCount, 1)
      << "a preview thumbnail was muxed into an NV12 program writer";

  encoder->stopRecording();
  encoder.reset();
  fs::remove_all(targetDir, cleanupError);
}

TEST(EncoderRecordingSession, MediaFoundationFragmentedMp4SurvivesAbruptProcessExit) {
#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_MF_ENCODER
  namespace fs = std::filesystem;
  constexpr const char* kChildEnv = "COREVIDEO_FMP4_CRASH_CHILD";
  constexpr const char* kTargetEnv = "COREVIDEO_FMP4_CRASH_TARGET";
  const char* childMode = std::getenv(kChildEnv);
  if (childMode != nullptr && std::string(childMode) == "1") {
    const char* target = std::getenv(kTargetEnv);
    if (target == nullptr || *target == '\0') {
      ::TerminateProcess(::GetCurrentProcess(), 78);
    }
    auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
    if (!encoder) {
      ::TerminateProcess(::GetCurrentProcess(), 77);
    }
    corevideo::modules::RecordingSessionRequest request;
    request.sessionId = "fmp4-crash-child";
    request.targetFolder = target;
    request.filenamePrefix = "crash-safe";
    request.format = "mp4";
    request.width = 320;
    request.height = 180;
    request.fps = 30;
    request.videoCodec = "h264";
    request.audioCodec = "aac";
    request.audioBitrateKbps = 96;
    request.targetBitrateMbps = 2;
    encoder->configureRecording(request);
    encoder->start({"recording"}, {});

    corevideo::modules::ProgramFrame frame;
    frame.width = 320;
    frame.height = 180;
    frame.preview.width = 320;
    frame.preview.height = 180;
    frame.preview.bgra.assign(320u * 180u * 4u, 0x50);
    std::vector<float> pcm(1600u * 2u, 0.1f);
    for (int i = 0; i < 120; ++i) {
      frame.frameNumber = i + 1;
      encoder->submit(frame);
      encoder->submitAudio(pcm.data(), 1600, 2, 48000);
      std::this_thread::sleep_for(std::chrono::milliseconds(34));
    }
    // Deliberately skip stopRecording and every destructor/finalizer.
    ::TerminateProcess(::GetCurrentProcess(), 99);
  }

  std::error_code ec;
  const auto targetDir = fs::temp_directory_path() /
      ("corevideo-fmp4-crash-" + std::to_string(::GetCurrentProcessId()));
  fs::remove_all(targetDir, ec);
  fs::create_directories(targetDir, ec);
  ASSERT_FALSE(ec);

  char modulePath[MAX_PATH] = {};
  ASSERT_TRUE(::GetModuleFileNameA(nullptr, modulePath, MAX_PATH) > 0);
  ASSERT_TRUE(::SetEnvironmentVariableA(kChildEnv, "1") != FALSE);
  ASSERT_TRUE(::SetEnvironmentVariableA(kTargetEnv, targetDir.string().c_str()) != FALSE);
  std::string command = std::string("\"") + modulePath +
      "\" --gtest_filter=EncoderRecordingSession.MediaFoundationFragmentedMp4SurvivesAbruptProcessExit";
  std::vector<char> mutableCommand(command.begin(), command.end());
  mutableCommand.push_back('\0');
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const BOOL created = ::CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  (void)::SetEnvironmentVariableA(kChildEnv, nullptr);
  (void)::SetEnvironmentVariableA(kTargetEnv, nullptr);
  ASSERT_TRUE(created != FALSE);
  ASSERT_EQ(::WaitForSingleObject(process.hProcess, 15'000), WAIT_OBJECT_0);
  DWORD childExit = 0;
  ASSERT_TRUE(::GetExitCodeProcess(process.hProcess, &childExit) != FALSE);
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  ASSERT_EQ(childExit, 99u);

  fs::path programPath;
  for (const auto& entry : fs::recursive_directory_iterator(targetDir)) {
    if (entry.is_regular_file() && entry.path().filename() == "Program.mp4") {
      programPath = entry.path();
      break;
    }
  }
  ASSERT_FALSE(programPath.empty());
  EXPECT_GT(fs::file_size(programPath, ec), 1024u);
  EXPECT_TRUE(fileContainsAscii(programPath, "moov"));
  EXPECT_GE(fileAsciiCount(programPath, "moof"), 2u);
  ASSERT_TRUE(SUCCEEDED(MFStartup(MF_VERSION)));
  Microsoft::WRL::ComPtr<IMFSourceReader> reader;
  const HRESULT openResult = MFCreateSourceReaderFromURL(
      programPath.wstring().c_str(), nullptr, &reader);
  EXPECT_TRUE(SUCCEEDED(openResult));
  if (SUCCEEDED(openResult)) {
    Microsoft::WRL::ComPtr<IMFMediaType> videoType;
    EXPECT_TRUE(SUCCEEDED(reader->GetNativeMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &videoType)));
  }
  reader.Reset();
  (void)MFShutdown();
  fs::remove_all(targetDir, ec);
#endif
}

namespace {
// Build a VideoFrame carrying a synthetic I420 payload (a flat mid-gray field),
// keyed by `sourceId`/`frameId` — the shape the ISO gather hands to the encoder.
corevideo::modules::IsoSourceVideoFrame makeIsoI420(const std::string& sourceId, int w, int h,
                                                    int64_t frameId, uint8_t luma) {
  corevideo::modules::IsoSourceVideoFrame iso;
  iso.sourceId = sourceId;
  auto buf = std::make_shared<std::vector<uint8_t>>();
  const size_t ySize = static_cast<size_t>(w) * h;
  buf->assign(ySize + ySize / 2, 128);            // neutral chroma
  std::fill(buf->begin(), buf->begin() + ySize, luma);  // flat luma
  iso.frame.participantId = sourceId;
  iso.frame.i420 = buf;
  iso.frame.i420Width = w;
  iso.frame.i420Height = h;
  iso.frame.frameId = frameId;
  return iso;
}
}  // namespace

// ISO-1: N per-source ISO writers open with their OWN NV12 video, finalize
// independently (no 0-byte tails), and PROGRAM is never regressed (it still muxes
// A+V with ISO writers present). Exercises the real Media Foundation sink.
TEST(EncoderRecordingSession, MediaFoundationIsoWritersProduceIndependentPlayableFiles) {
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) {
    return;  // Media Foundation unavailable — nothing to test.
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto targetDir = fs::temp_directory_path() / "corevideo-iso1-regression";
  fs::remove_all(targetDir, ec);

  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "iso1-show";
  request.targetFolder = targetDir.string();
  request.filenamePrefix = "show";
  request.format = "mp4";
  request.quality = "high";
  request.width = 640;
  request.height = 360;
  request.fps = 30;
  request.videoCodec = "h264";
  request.audioCodec = "aac";
  request.audioBitrateKbps = 128;
  request.targetBitrateMbps = 4;
  // Two ISO sources with roster names → ISO-01-Alice.mp4 / ISO-02-Bob-Jones.mp4.
  request.isoSources = {{"zoom:A", "Alice"}, {"zoom:B", "Bob Jones"}};
  encoder->configureRecording(request);

  // #286 shape: start() runs twice (arm, then restart) — every ISO writer's
  // per-session state must reset in Mp4Writer::open().
  encoder->start({"recording"}, {});
  encoder->start({"recording"}, {});

  // Program A+V (priority-1, must never regress).
  corevideo::modules::ProgramFrame frame;
  frame.width = 640;
  frame.height = 360;
  frame.preview.width = 640;
  frame.preview.height = 360;
  frame.preview.bgra.assign(static_cast<size_t>(640) * 360 * 4, 0x40);
  std::vector<float> pcm(static_cast<size_t>(960) * 2, 0.2f);

  for (int i = 0; i < 4; ++i) {
    frame.frameNumber = i + 1;
    encoder->submit(frame);
    // Each ISO source advances its own frameId; re-submitting the same id twice
    // must dedup (proves the per-source clock).
    encoder->submitIsoVideo({makeIsoI420("zoom:A", 320, 240, i, 90),
                             makeIsoI420("zoom:B", 640, 360, i, 200)});
    encoder->submitIsoVideo({makeIsoI420("zoom:A", 320, 240, i, 90)});  // duplicate frameId
    encoder->submitAudio(pcm.data(), 960, 2, 48000);
  }

  const auto session = encoder->session();
  EXPECT_TRUE(session.recordingWarning.empty()) << session.recordingWarning;
  // Program not regressed: A+V both present.
  EXPECT_GT(session.recordingVideoFrameCount, 0);
  EXPECT_GT(session.recordingAudioPacketCount, 0) << session.recordingWarning;
  // Two ISO writers, each with its own video and no cross-source contamination.
  ASSERT_EQ(session.isoStreams.size(), 2u);
  for (const auto& iso : session.isoStreams) {
    EXPECT_TRUE(iso.trackOpen) << iso.sourceId << ": " << iso.warning;
    EXPECT_EQ(iso.videoFrameCount, 4) << iso.sourceId;  // 4 distinct frames, dupes deduped
    EXPECT_TRUE(iso.warning.empty()) << iso.warning;
    EXPECT_EQ(iso.audioSampleCount, 0);  // ISO-1 video-only
  }
  EXPECT_EQ(session.isoStreams[0].displayName, "Alice");
  EXPECT_EQ(session.isoStreams[1].displayName, "Bob Jones");

  encoder->stopRecording();

  // Files are finalized + playable: program + two ISOs + manifest, all non-zero.
  ASSERT_FALSE(session.recordingSessionDir.empty());
  const fs::path dir(session.recordingSessionDir);
  EXPECT_TRUE(fs::exists(dir / "Program.mp4"));
  EXPECT_TRUE(fs::exists(dir / "ISO-01-Alice.mp4"));
  EXPECT_TRUE(fs::exists(dir / "ISO-02-Bob-Jones.mp4"));
  EXPECT_TRUE(fs::exists(dir / "manifest.json"));
  EXPECT_GT(fs::file_size(dir / "ISO-01-Alice.mp4", ec), 0u);
  EXPECT_GT(fs::file_size(dir / "ISO-02-Bob-Jones.mp4", ec), 0u);
  EXPECT_TRUE(fileContainsAscii(dir / "Program.mp4", "moof"));
  EXPECT_TRUE(fileContainsAscii(dir / "ISO-01-Alice.mp4", "moof"));
  EXPECT_TRUE(fileContainsAscii(dir / "manifest.json", "\"containerMode\": \"fragmented-mp4\""));
  EXPECT_TRUE(fileContainsAscii(dir / "manifest.json", "\"crashSafe\": true"));
  EXPECT_FALSE(session.recordingManifestPath.empty());

  encoder.reset();
  fs::remove_all(targetDir, ec);
}

// ISO-1 loud-failure: a bad/uncreatable target folder must NOT silently redirect
// ISO files to %TEMP% — it raises recording.warning and refuses ISO, while the
// program keeps recording (priority-1).
TEST(EncoderRecordingSession, MediaFoundationIsoRefusesBadFolderLoudly) {
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) {
    return;
  }
  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "iso1-badfolder";
  // A path that cannot be created (a NUL device style / reserved path on Windows).
  request.targetFolder = "\\\\?\\Z:\\nonexistent-volume\\corevideo-iso";
  request.filenamePrefix = "show";
  request.format = "mp4";
  request.width = 320;
  request.height = 240;
  request.fps = 30;
  request.videoCodec = "h264";
  request.isoSources = {{"zoom:A", "Alice"}};
  encoder->configureRecording(request);
  encoder->start({"recording"}, {});

  const auto session = encoder->session();
  // ISO was refused loudly (a warning is set) and no ISO writer is armed.
  EXPECT_FALSE(session.recordingWarning.empty());
  EXPECT_TRUE(session.isoStreams.empty());
  encoder->stopRecording();
  encoder.reset();
}

namespace {
// One tick of a distinct-frequency raw-stem for `sourceId` (so two ISO writers
// carry DIFFERENT audio content, not the same program mix).
corevideo::modules::IsoSourceAudio makeIsoTone(const std::string& sourceId, int frameCount,
                                               int channels, double freqHz, int64_t startFrame) {
  corevideo::modules::IsoSourceAudio iso;
  iso.sourceId = sourceId;
  iso.frameCount = frameCount;
  iso.channels = channels;
  iso.sampleRate = 48000;
  iso.pcm.resize(static_cast<size_t>(frameCount) * channels);
  for (int i = 0; i < frameCount; ++i) {
    const double t = static_cast<double>(startFrame + i) / 48000.0;
    const float s = static_cast<float>(0.25 * std::sin(2.0 * 3.14159265358979 * freqHz * t));
    for (int c = 0; c < channels; ++c) iso.pcm[static_cast<size_t>(i) * channels + c] = s;
  }
  return iso;
}
}  // namespace

// ISO-2: each ISO writer muxes its OWN raw-stem audio (self-contained A+V), the
// #286 per-ISO-writer audio-stream reset holds across a double start (a reused
// ISO writer must NOT lose its audio track), silence-fill advances a gapped stem,
// and PROGRAM A+V is never regressed with ISO AUDIO enabled.
TEST(EncoderRecordingSession, MediaFoundationIsoWritersMuxOwnAudioStems) {
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) {
    return;  // Media Foundation unavailable.
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto targetDir = fs::temp_directory_path() / "corevideo-iso2-audio";
  fs::remove_all(targetDir, ec);

  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "iso2-show";
  request.targetFolder = targetDir.string();
  request.filenamePrefix = "show";
  request.format = "mp4";
  request.quality = "high";
  request.width = 640;
  request.height = 360;
  request.fps = 30;
  request.videoCodec = "h264";
  request.audioCodec = "aac";
  request.audioBitrateKbps = 128;
  request.targetBitrateMbps = 4;
  request.isoSources = {{"zoom:A", "Alice"}, {"zoom:B", "Bob Jones"}};
  encoder->configureRecording(request);

  // #286 double-start: arm then restart. Every ISO writer's audioConfigured_
  // must reset in Mp4Writer::open() so the reused writer re-adds its AAC stream.
  encoder->start({"recording"}, {});
  encoder->start({"recording"}, {});

  corevideo::modules::ProgramFrame frame;
  frame.width = 640;
  frame.height = 360;
  frame.preview.width = 640;
  frame.preview.height = 360;
  frame.preview.bgra.assign(static_cast<size_t>(640) * 360 * 4, 0x40);
  std::vector<float> programPcm(static_cast<size_t>(960) * 2, 0.2f);

  // 20 ticks. Source A talks the whole time (mono 220Hz). Source B is SILENT for
  // ticks 5..14 (submitted with empty PCM → silence-fill), talking otherwise
  // (mono 440Hz) — proves distinct content AND the gapped-stem silence-fill.
  int64_t aFrames = 0;
  int64_t bFrames = 0;
  for (int i = 0; i < 20; ++i) {
    frame.frameNumber = i + 1;
    encoder->submit(frame);
    encoder->submitIsoVideo({makeIsoI420("zoom:A", 320, 240, i, 90),
                             makeIsoI420("zoom:B", 640, 360, i, 200)});
    encoder->submitAudio(programPcm.data(), 960, 2, 48000);

    std::vector<corevideo::modules::IsoSourceAudio> isoAudio;
    isoAudio.push_back(makeIsoTone("zoom:A", 960, 1, 220.0, aFrames));
    aFrames += 960;
    if (i < 5 || i >= 15) {
      isoAudio.push_back(makeIsoTone("zoom:B", 960, 1, 440.0, bFrames));
      bFrames += 960;
    } else {
      corevideo::modules::IsoSourceAudio silent;  // gated: empty → silence-fill
      silent.sourceId = "zoom:B";
      silent.sampleRate = 48000;
      isoAudio.push_back(silent);
    }
    encoder->submitIsoAudio(isoAudio);
  }

  const auto session = encoder->session();
  EXPECT_TRUE(session.recordingWarning.empty()) << session.recordingWarning;
  // PROGRAM NOT REGRESSED: A+V both present WITH ISO audio enabled (#286 class).
  EXPECT_GT(session.recordingVideoFrameCount, 0);
  EXPECT_GT(session.recordingAudioPacketCount, 0) << session.recordingWarning;
  ASSERT_EQ(session.isoStreams.size(), 2u);
  for (const auto& iso : session.isoStreams) {
    EXPECT_TRUE(iso.trackOpen) << iso.sourceId << ": " << iso.warning;
    EXPECT_TRUE(iso.warning.empty()) << iso.warning;
    // Each ISO muxed its OWN audio (silence + real), so the AAC track is present.
    EXPECT_GT(iso.audioSampleCount, 0) << iso.sourceId << " lost its audio track";
  }

  encoder->stopRecording();

  ASSERT_FALSE(session.recordingSessionDir.empty());
  const fs::path dir(session.recordingSessionDir);
  const auto aPath = dir / "ISO-01-Alice.mp4";
  const auto bPath = dir / "ISO-02-Bob-Jones.mp4";
  EXPECT_TRUE(fs::exists(aPath));
  EXPECT_TRUE(fs::exists(bPath));
  EXPECT_GT(fs::file_size(aPath, ec), 0u);
  EXPECT_GT(fs::file_size(bPath, ec), 0u);

  encoder.reset();
  fs::remove_all(targetDir, ec);
}

// Live-meeting regression: Program plus eight Zoom ISO A/V writers must arm at
// the same time. This is the production shape that exposed Media Foundation's
// finite hardware-encoder capacity: creating every Sink Writer with hardware
// transforms enabled left the ISO files at zero bytes when the GPU budget was
// already consumed. Auto placement must keep every stem alive by spilling the
// overflow to the software H.264 transform.
TEST(EncoderRecordingSession, MediaFoundationEightZoomIsoWritersSurviveEncoderCapacity) {
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) {
    return;  // Media Foundation unavailable.
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto targetDir = fs::temp_directory_path() / "corevideo-iso8-capacity";
  fs::remove_all(targetDir, ec);

  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "iso8-capacity";
  request.targetFolder = targetDir.string();
  request.filenamePrefix = "show";
  request.format = "mp4";
  request.quality = "high";
  request.width = 640;
  request.height = 360;
  request.fps = 30;
  request.videoCodec = "h264";
  request.audioCodec = "aac";
  request.audioBitrateKbps = 128;
  request.targetBitrateMbps = 8;
  for (int source = 0; source < 8; ++source) {
    request.isoSources.push_back(
        {"zoom:" + std::to_string(source + 1), "Guest " + std::to_string(source + 1), true});
  }
  encoder->configureRecording(request);
  encoder->start({"recording"}, {});

  corevideo::modules::ProgramFrame frame;
  frame.width = 640;
  frame.height = 360;
  frame.preview.width = 640;
  frame.preview.height = 360;
  frame.preview.bgra.assign(static_cast<size_t>(640) * 360 * 4, 0x40);
  std::vector<float> programPcm(static_cast<size_t>(960) * 2, 0.2f);
  std::array<int64_t, 8> sourceFrames{};

  for (int tick = 0; tick < 12; ++tick) {
    frame.frameNumber = tick + 1;
    encoder->submit(frame);

    std::vector<corevideo::modules::IsoSourceVideoFrame> isoVideo;
    std::vector<corevideo::modules::IsoSourceAudio> isoAudio;
    isoVideo.reserve(8);
    isoAudio.reserve(8);
    for (int source = 0; source < 8; ++source) {
      const std::string sourceId = "zoom:" + std::to_string(source + 1);
      isoVideo.push_back(makeIsoI420(sourceId, 640, 360, tick,
                                    static_cast<uint8_t>(40 + source * 20)));
      isoAudio.push_back(makeIsoTone(sourceId, 960, 1, 180.0 + source * 40.0,
                                     sourceFrames[static_cast<size_t>(source)]));
      sourceFrames[static_cast<size_t>(source)] += 960;
    }
    encoder->submitIsoVideo(isoVideo);
    encoder->submitAudio(programPcm.data(), 960, 2, 48000);
    encoder->submitIsoAudio(isoAudio);
  }

  const auto session = encoder->session();
  EXPECT_TRUE(session.recordingWarning.empty()) << session.recordingWarning;
  ASSERT_EQ(session.isoStreams.size(), 8u);
  int hardwareIsoCount = 0;
  int softwareIsoCount = 0;
  for (const auto& iso : session.isoStreams) {
    EXPECT_TRUE(iso.trackOpen) << iso.sourceId << ": " << iso.warning;
    EXPECT_TRUE(iso.warning.empty()) << iso.sourceId << ": " << iso.warning;
    EXPECT_GT(iso.videoFrameCount, 0) << iso.sourceId;
    EXPECT_GT(iso.audioSampleCount, 0) << iso.sourceId;
    hardwareIsoCount += iso.encoderPath == "hardware" ? 1 : 0;
    softwareIsoCount += iso.encoderPath == "software" ? 1 : 0;
  }
  EXPECT_EQ(hardwareIsoCount, 7);
  EXPECT_EQ(softwareIsoCount, 1);

  encoder->stopRecording();
  ASSERT_FALSE(session.recordingSessionDir.empty());
  const fs::path dir(session.recordingSessionDir);
  for (int source = 0; source < 8; ++source) {
    char filename[64];
    std::snprintf(filename, sizeof(filename), "ISO-%02d-Guest-%d.mp4", source + 1, source + 1);
    const auto path = dir / filename;
    EXPECT_TRUE(fs::exists(path)) << path.string();
    if (fs::exists(path)) {
      EXPECT_GT(fs::file_size(path, ec), 0u) << path.string();
    }
  }

  encoder.reset();
  fs::remove_all(targetDir, ec);
}

namespace {
// Build a VideoFrame carrying a synthetic BGRA payload (a flat color field),
// keyed by `sourceId`/`frameId` — the shape a CAPTURE source (UVC camera /
// screen / browser) hands the ISO gather. Capture frames are BGRA on the CPU
// (unlike Zoom's I420), so the writer opens with the RGB32 input path.
corevideo::modules::IsoSourceVideoFrame makeIsoBgra(const std::string& sourceId, int w, int h,
                                                    int64_t frameId, uint8_t value) {
  corevideo::modules::IsoSourceVideoFrame iso;
  iso.sourceId = sourceId;
  auto buf = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(w) * h * 4, value);
  iso.frame.participantId = sourceId;
  iso.frame.pixels = buf;
  iso.frame.pixelWidth = w;
  iso.frame.pixelHeight = h;
  iso.frame.pixelStride = w * 4;
  iso.frame.frameId = frameId;
  return iso;
}
}  // namespace

// ISO-3: a CAPTURE (BGRA) ISO writer produces a playable file, AND a mixed
// session — zoom (NV12) + capture (BGRA) writers side by side — muxes both with
// the correct per-source input type, program A+V is never regressed, and a
// paired capture source carries its own audio stem.
TEST(EncoderRecordingSession, MediaFoundationCaptureBgraIsoMixedWithZoomNv12) {
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) {
    return;  // Media Foundation unavailable.
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto targetDir = fs::temp_directory_path() / "corevideo-iso3-capture";
  fs::remove_all(targetDir, ec);

  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "iso3-show";
  request.targetFolder = targetDir.string();
  request.filenamePrefix = "show";
  request.format = "mp4";
  request.quality = "high";
  request.width = 640;
  request.height = 360;
  request.fps = 30;
  request.videoCodec = "h264";
  request.audioCodec = "aac";
  request.audioBitrateKbps = 128;
  request.targetBitrateMbps = 4;
  // A Zoom participant (NV12, has audio) + a paired capture camera (BGRA, has
  // audio — Elgato-class). hasAudio=true on both → each ISO is self-contained A+V.
  request.isoSources = {{"zoom:A", "Alice", true}, {"capture:cam0", "Logitech Cam", true}};
  encoder->configureRecording(request);
  encoder->start({"recording"}, {});
  encoder->start({"recording"}, {});  // #286 double-start

  corevideo::modules::ProgramFrame frame;
  frame.width = 640;
  frame.height = 360;
  frame.preview.width = 640;
  frame.preview.height = 360;
  frame.preview.bgra.assign(static_cast<size_t>(640) * 360 * 4, 0x40);
  std::vector<float> programPcm(static_cast<size_t>(960) * 2, 0.2f);

  int64_t camFrames = 0;
  for (int i = 0; i < 8; ++i) {
    frame.frameNumber = i + 1;
    encoder->submit(frame);
    // Zoom source → NV12 path; capture source → BGRA path. Both in ONE submit.
    encoder->submitIsoVideo({makeIsoI420("zoom:A", 320, 240, i, 90),
                             makeIsoBgra("capture:cam0", 640, 360, i, 0x80)});
    encoder->submitAudio(programPcm.data(), 960, 2, 48000);
    encoder->submitIsoAudio({makeIsoTone("zoom:A", 960, 1, 220.0, camFrames),
                             makeIsoTone("capture:cam0", 960, 2, 330.0, camFrames)});
    camFrames += 960;
  }

  const auto session = encoder->session();
  EXPECT_TRUE(session.recordingWarning.empty()) << session.recordingWarning;
  // PROGRAM NOT REGRESSED with a capture ISO enabled.
  EXPECT_GT(session.recordingVideoFrameCount, 0);
  EXPECT_GT(session.recordingAudioPacketCount, 0) << session.recordingWarning;
  ASSERT_EQ(session.isoStreams.size(), 2u);
  for (const auto& iso : session.isoStreams) {
    EXPECT_TRUE(iso.trackOpen) << iso.sourceId << ": " << iso.warning;
    EXPECT_TRUE(iso.warning.empty()) << iso.warning;
    EXPECT_EQ(iso.videoFrameCount, 8) << iso.sourceId;
    EXPECT_GT(iso.audioSampleCount, 0) << iso.sourceId << " lost its paired audio";
  }
  EXPECT_EQ(session.isoStreams[1].sourceId, "capture:cam0");
  EXPECT_EQ(session.isoStreams[1].displayName, "Logitech Cam");

  encoder->stopRecording();

  ASSERT_FALSE(session.recordingSessionDir.empty());
  const fs::path dir(session.recordingSessionDir);
  const auto camPath = dir / "ISO-02-Logitech-Cam.mp4";
  EXPECT_TRUE(fs::exists(dir / "ISO-01-Alice.mp4"));
  EXPECT_TRUE(fs::exists(camPath));
  EXPECT_GT(fs::file_size(camPath, ec), 0u);
  EXPECT_TRUE(fs::exists(dir / "Program.mp4"));

  encoder.reset();
  fs::remove_all(targetDir, ec);
}

// ISO-3 pairing rule: a PURE-VIDEO capture source (a camera with no paired
// audio, hasAudio=false) produces a VIDEO-ONLY ISO — its writer opens WITHOUT an
// AAC stream, so even if the audio worker submits an (empty) stem the file stays
// honestly video-only (no all-silence track), and program is never regressed.
TEST(EncoderRecordingSession, MediaFoundationVideoOnlyCaptureIsoHasNoAudioTrack) {
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) {
    return;
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto targetDir = fs::temp_directory_path() / "corevideo-iso3-videoonly";
  fs::remove_all(targetDir, ec);

  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "iso3-videoonly";
  request.targetFolder = targetDir.string();
  request.filenamePrefix = "show";
  request.format = "mp4";
  request.width = 640;
  request.height = 360;
  request.fps = 30;
  request.videoCodec = "h264";
  request.audioCodec = "aac";
  request.audioBitrateKbps = 128;
  request.targetBitrateMbps = 4;
  // A camera with NO paired audio → hasAudio=false → video-only ISO.
  request.isoSources = {{"capture:cam0", "Webcam", false}};
  encoder->configureRecording(request);
  encoder->start({"recording"}, {});

  corevideo::modules::ProgramFrame frame;
  frame.width = 640;
  frame.height = 360;
  frame.preview.width = 640;
  frame.preview.height = 360;
  frame.preview.bgra.assign(static_cast<size_t>(640) * 360 * 4, 0x40);
  std::vector<float> programPcm(static_cast<size_t>(960) * 2, 0.2f);

  for (int i = 0; i < 6; ++i) {
    frame.frameNumber = i + 1;
    encoder->submit(frame);
    encoder->submitIsoVideo({makeIsoBgra("capture:cam0", 640, 360, i, 0x70)});
    encoder->submitAudio(programPcm.data(), 960, 2, 48000);
    // The worker submits an EMPTY stem for a video-only capture source; the sink
    // must skip it (no audio stream), never silence-fill an all-silence track.
    corevideo::modules::IsoSourceAudio empty;
    empty.sourceId = "capture:cam0";
    empty.sampleRate = 48000;
    encoder->submitIsoAudio({empty});
  }

  const auto session = encoder->session();
  EXPECT_TRUE(session.recordingWarning.empty()) << session.recordingWarning;
  EXPECT_GT(session.recordingVideoFrameCount, 0);      // program video
  EXPECT_GT(session.recordingAudioPacketCount, 0);     // program audio (not regressed)
  ASSERT_EQ(session.isoStreams.size(), 1u);
  const auto& iso = session.isoStreams[0];
  EXPECT_TRUE(iso.trackOpen) << iso.warning;
  EXPECT_EQ(iso.videoFrameCount, 6);
  EXPECT_EQ(iso.audioSampleCount, 0) << "a pure-video capture ISO must have NO audio track";

  encoder->stopRecording();
  ASSERT_FALSE(session.recordingSessionDir.empty());
  const fs::path dir(session.recordingSessionDir);
  const auto camPath = dir / "ISO-01-Webcam.mp4";
  EXPECT_TRUE(fs::exists(camPath));
  EXPECT_GT(fs::file_size(camPath, ec), 0u);

  encoder.reset();
  fs::remove_all(targetDir, ec);
}

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_MF_ENCODER

namespace {

// Minimal encoder sink whose session carries a recording warning — drives the
// MediaCore-side propagation (encoder warning -> snapshot recording.warning).
class WarningEncoderSink final : public corevideo::modules::IEncoderSink {
 public:
  void configureRecording(const corevideo::modules::RecordingSessionRequest& request) override {
    session_.recordingSessionId = request.sessionId;
  }
  corevideo::modules::OutputSession start(const std::vector<std::string>& destinations,
                                          const std::vector<std::string>&) override {
    session_.active = true;
    session_.destinations = destinations;
    session_.recordingStatus = "recording";
    return session_;
  }
  void submit(const corevideo::modules::ProgramFrame&) override { ++session_.encodedFrameCount; }
  corevideo::modules::OutputSession session() const override { return session_; }
  void setRecordingWarning(std::string warning) { session_.recordingWarning = std::move(warning); }

 private:
  corevideo::modules::OutputSession session_;
};

}  // namespace

namespace {
// Records the RecordingSessionRequest MediaCore builds, so a test can assert the
// ISO-3 source resolution (display name + audio-pairing hasAudio per source).
class RequestCapturingSink final : public corevideo::modules::IEncoderSink {
 public:
  void configureRecording(const corevideo::modules::RecordingSessionRequest& request) override {
    lastRequest = request;
    session_.recordingSessionId = request.sessionId;
  }
  corevideo::modules::OutputSession start(const std::vector<std::string>& destinations,
                                          const std::vector<std::string>&) override {
    session_.active = true;
    session_.destinations = destinations;
    session_.recordingStatus = "recording";
    return session_;
  }
  void submit(const corevideo::modules::ProgramFrame&) override {}
  corevideo::modules::OutputSession session() const override { return session_; }

  corevideo::modules::RecordingSessionRequest lastRequest;

 private:
  corevideo::modules::OutputSession session_;
};

class StartCountingEncoderSink final : public corevideo::modules::IEncoderSink {
 public:
  void configureRecording(const corevideo::modules::RecordingSessionRequest& request) override {
    session_.recordingSessionId = request.sessionId;
  }

  corevideo::modules::OutputSession start(const std::vector<std::string>& destinations,
                                          const std::vector<std::string>& isoParticipantIds) override {
    ++startCount;
    session_.active = true;
    session_.destinations = destinations;
    session_.isoParticipantIds = isoParticipantIds;
    const bool startsRecording =
        std::find(destinations.begin(), destinations.end(), "recording") != destinations.end();
    if (startsRecording) {
      ++recordingStartCount;
      session_.recordingStatus = "recording";
    }
    return session_;
  }

  void submit(const corevideo::modules::ProgramFrame&) override {}
  corevideo::modules::OutputSession session() const override { return session_; }

  int startCount = 0;
  int recordingStartCount = 0;

 private:
  corevideo::modules::OutputSession session_;
};
}  // namespace

TEST(EncoderRecordingSession, RepeatedProgramSyncDoesNotRestartActiveRecordingWriters) {
  auto modules = corevideo::modules::createStubModules();
  auto countingSink = std::make_unique<StartCountingEncoderSink>();
  auto* encoder = countingSink.get();
  modules.encoder = std::move(countingSink);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const corevideo::rpc::Json startProgram = corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"recording", "rtmp"}},
      {"isoSourceIds", corevideo::rpc::Json::Array{"zoom:host", "zoom:guest"}},
  };
  const corevideo::rpc::Json setTargets = corevideo::rpc::Json::Object{
      {"type", "set-recording-targets"},
      {"targetFolder", "Recordings/CoreVideo Pro/tests"},
      {"filenamePrefix", "idempotent-show"},
      {"format", "mp4"},
      {"isoSourceIds", corevideo::rpc::Json::Array{"zoom:host", "zoom:guest"}},
  };
  const corevideo::rpc::Json startRecording = corevideo::rpc::Json::Object{
      {"type", "start-recording-session"},
      {"sessionId", "idempotent-show-host-guest"},
      {"isoSourceIds", corevideo::rpc::Json::Array{"zoom:host", "zoom:guest"}},
  };

  (void)mediaCore.applyCommands(
      corevideo::rpc::Json::Array{setTargets, startRecording, startProgram});
  EXPECT_EQ(encoder->recordingStartCount, 1)
      << "the initial output sync must not open a throwaway recording generation";

  for (int sync = 0; sync < 20; ++sync) {
    (void)mediaCore.applyCommands(
        corevideo::rpc::Json::Array{setTargets, startRecording, startProgram});
  }

  EXPECT_EQ(encoder->recordingStartCount, 1)
      << "scene/preview sync must leave the active Program and ISO writers untouched";
  EXPECT_EQ(encoder->startCount, 1)
      << "the recording writer generation also supplies the Program tap for outputs";
  EXPECT_EQ(encoder->session().recordingSessionId, "idempotent-show-host-guest");
}

// ISO-3: MediaCore resolves each ISO source's display name AND its audio-pairing
// decision. A Zoom participant always has audio; a capture device has audio ONLY
// when the operator paired an audio input to it (sync-capture-audio-sources) —
// otherwise it is a VIDEO-ONLY capture ISO. Capture display names come from the
// enumerated device (post sees ISO-NN-<CameraName>.mp4, not a hashed id).
TEST(EncoderRecordingSession, MediaCoreResolvesCaptureIsoDisplayNamesAndAudioPairing) {
  auto modules = corevideo::modules::createStubModules();
  auto sink = std::make_unique<RequestCapturingSink>();
  auto* sinkPtr = sink.get();
  modules.encoder = std::move(sink);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      // Pair a real audio input to the DeckLink capture device (Elgato-class);
      // leave the AJA device unpaired (a pure-video capture card).
      corevideo::rpc::Json::Object{
          {"type", "sync-capture-audio-sources"},
          {"sources", corevideo::rpc::Json::Array{
                          corevideo::rpc::Json::Object{
                              {"captureDeviceId", "decklink-1"},
                              {"audioDeviceId", "board-feed-1"},
                              {"audioDeviceName", "Board Feed"},
                              {"audioSourceKind", "wasapi-input"},
                          },
                      }},
      },
      corevideo::rpc::Json::Object{
          {"type", "set-recording-targets"},
          {"targetFolder", "Recordings/CoreVideo Pro/tests"},
          {"filenamePrefix", "iso3"},
          {"format", "mp4"},
          {"isoSourceIds", corevideo::rpc::Json::Array{"zoom:host", "capture:decklink-1",
                                                       "capture:aja-io-1"}},
      },
  });

  const auto& req = sinkPtr->lastRequest;
  ASSERT_EQ(req.isoSources.size(), 3u);

  // [0] Zoom participant — always has its own isolate_audio stem.
  EXPECT_EQ(req.isoSources[0].sourceId, "zoom:host");
  EXPECT_TRUE(req.isoSources[0].hasAudio);

  // [1] Paired capture device — display name from enumerate, audio muxed.
  EXPECT_EQ(req.isoSources[1].sourceId, "capture:decklink-1");
  EXPECT_EQ(req.isoSources[1].displayName, "DeckLink Mini Recorder 4K");
  EXPECT_TRUE(req.isoSources[1].hasAudio) << "a capture device with paired audio must mux audio";

  // [2] Unpaired capture device — VIDEO-ONLY ISO (no all-silence audio track).
  EXPECT_EQ(req.isoSources[2].sourceId, "capture:aja-io-1");
  EXPECT_EQ(req.isoSources[2].displayName, "AJA Io 4K Plus");
  EXPECT_FALSE(req.isoSources[2].hasAudio) << "a pure-video capture source must be video-only";
}

TEST(EncoderRecordingSession, EncoderRecordingWarningSurfacesInSnapshotRecordingSection) {
  auto modules = corevideo::modules::createStubModules();
  auto warningSink = std::make_unique<WarningEncoderSink>();
  auto* encoder = warningSink.get();
  modules.encoder = std::move(warningSink);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-recording-session"},
      {"sessionId", "warn-1"},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}},
  });

  const std::string dropWarning =
      "Media Foundation dropped program audio: write audio sample: 0xC00D36B3.";
  encoder->setRecordingWarning(dropWarning);

  // Any subsequent tick must publish the encoder warning into the recording
  // section — a video-only recording with live program audio must be
  // impossible to miss from the snapshot alone.
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{}, 16.0);
  const auto* recording = state.get("recording");
  ASSERT_NE(recording, nullptr);
  EXPECT_EQ(recording->getString("warning"), dropWarning);
  EXPECT_EQ(recording->getString("status"), "recording");  // still recording; warning, not failure
}

TEST(RecordingPtsClock, SessionBoundaryRejectsPreEpochVideoWithoutConsumingIdentity) {
  corevideo::modules::RecordingPtsClock clock;
  clock.reset(10'000'000);
  EXPECT_FALSE(clock.videoPts(9'000'000, 42).has_value());
  EXPECT_FALSE(clock.videoPtsForSource(9'000'000, "camera", 42).has_value());
  const auto accepted = clock.videoPts(10'000'000, 42);
  ASSERT_TRUE(accepted.has_value());
  EXPECT_EQ(*accepted, 0);
  const auto iso = clock.videoPtsForSource(10'000'000, "camera", 42);
  ASSERT_TRUE(iso.has_value());
  EXPECT_EQ(*iso, 0);
}
TEST(RecordingPtsClock, AudioFirstCannotProduceNegativeVideoPts) {
  corevideo::modules::RecordingPtsClock clock;
  EXPECT_EQ(clock.audioPts(10'000'000, 960, 48000), 0);
  EXPECT_FALSE(clock.videoPts(9'000'000, 1).has_value());
  const auto accepted = clock.videoPts(10'166'667, 2);
  ASSERT_TRUE(accepted.has_value());
  EXPECT_EQ(*accepted, 166'667);
}
TEST(RecordingPtsClock, ExplicitBoundaryKeepsCapturedAudioOffsetIndependentOfWriterDelay) {
  corevideo::modules::RecordingPtsClock clock;
  clock.reset(10'000'000);
  EXPECT_EQ(clock.audioPts(10'200'000, 960, 48000), 200'000);
  EXPECT_EQ(clock.audioPts(10'400'000, 960, 48000), 400'000);
  const auto video = clock.videoPts(10'200'000, 1);
  ASSERT_TRUE(video.has_value());
  EXPECT_EQ(*video, 200'000);
}

TEST(RecordingPtsClock, AudioBoundaryRetainsPostEpochTailOfStraddlingBlock) {
  corevideo::modules::RecordingPtsClock clock;
  clock.reset(10'000'000);
  const auto tail = clock.trimAudioToEpoch(9'900'000, 960, 48000);
  EXPECT_EQ(tail.skippedFrames, 480);
  EXPECT_EQ(tail.retainedFrames, 480);
  EXPECT_EQ(tail.timestamp100ns, 10'000'000);
  EXPECT_EQ(clock.audioPts(tail.timestamp100ns, tail.retainedFrames, 48000), 0);
  EXPECT_EQ(clock.audioPts(10'100'000, 960, 48000), 100'000);
}
TEST(RecordingPtsClock, AudioBoundaryRoundsFractionalSamplesUpAndRejectsFullyStaleBlock) {
  corevideo::modules::RecordingPtsClock clock;
  clock.reset(10'000'000);
  const auto fractional = clock.trimAudioToEpoch(9'999'999, 960, 48000);
  EXPECT_EQ(fractional.skippedFrames, 1);
  EXPECT_EQ(fractional.retainedFrames, 959);
  EXPECT_EQ(fractional.timestamp100ns, 10'000'208);
  const auto stale = clock.trimAudioToEpoch(9'800'000, 960, 48000);
  EXPECT_EQ(stale.skippedFrames, 960);
  EXPECT_EQ(stale.retainedFrames, 0);
  const auto exact = clock.trimAudioToEpoch(10'000'000, 960, 48000);
  EXPECT_EQ(exact.skippedFrames, 0);
  EXPECT_EQ(exact.retainedFrames, 960);
  EXPECT_EQ(exact.timestamp100ns, 10'000'000);
}

TEST(RecordingPtsClock, FractionalAudioEpochUsesOneIntegerSampleTimeline) {
  corevideo::modules::RecordingPtsClock clock;
  clock.reset(10'000'000);
  // Just past480samples: leading silence must occupy481 whole samples.
  const auto first = clock.audioPts(10'100'001, 1024, 48000);
  EXPECT_EQ(first, 481LL * 10'000'000 / 48000);
  EXPECT_EQ((first * 48000 + 9'999'999) / 10'000'000, 481);
  for (int block = 1; block < 100; ++block) {
    const auto pts = clock.audioPts(99'000'000, 1024, 48000);
    EXPECT_EQ(pts, (481LL + block * 1024LL) * 10'000'000 / 48000);
  }
}

TEST(RecordingStartBoundary, FirstReadyProgramFrameDefinesSharedZeroWithoutPreroll) {
  corevideo::modules::RecordingStartBoundary start;
  start.begin(10'000'000, 13'000'000);
  EXPECT_FALSE(start.select(12'900'000));
  EXPECT_FALSE(start.epoch().has_value());
  ASSERT_TRUE(start.select(13'166'667));
  corevideo::modules::RecordingPtsClock clock;
  clock.reset(*start.epoch());
  ASSERT_TRUE(clock.videoPts(13'166'667, 10).has_value());
  EXPECT_EQ(clock.lastVideoPts100ns(), 0);
  const auto audio = clock.trimAudioToEpoch(13'066'667, 960, 48000);
  EXPECT_EQ(audio.retainedFrames, 480);
  EXPECT_EQ(clock.audioPts(audio.timestamp100ns, audio.retainedFrames, 48000), 0);
  const auto iso = clock.videoPtsForSource(13'333'334, "iso", 2);
  ASSERT_TRUE(iso.has_value());
  EXPECT_EQ(*iso, 166'667);
}
TEST(RecordingStartBoundary, RestartAndStopBeforeFirstFrameNeverReusePriorEpoch) {
  corevideo::modules::RecordingStartBoundary start;
  start.begin(100, 200);
  ASSERT_TRUE(start.select(300));
  start.begin(400, 500);
  EXPECT_FALSE(start.epoch().has_value());
  EXPECT_FALSE(start.select(499));
  EXPECT_FALSE(start.epoch().has_value());
  ASSERT_TRUE(start.select(600));
  EXPECT_EQ(*start.epoch(), 600);
  EXPECT_FALSE(start.select(599));
  EXPECT_EQ(*start.epoch(), 600);
}
