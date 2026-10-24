#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include "modules/AsyncEncoderSink.h"
#include "modules/RecordingProgramContinuity.h"
#include "rpc/JsonRpcServer.h"

#include "EncoderCapacityProbeTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <functional>
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

TEST(EncoderRecordingSession, IdleStopDoesNotInventRecordingFinalization) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  for (int i = 0; i < 3; ++i) {
    const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
        {"type", "stop-recording-session"},
        {"reason", "Recording disabled in production state."},
    });
    const auto* recording = state.get("recording");
    EXPECT_TRUE(recording == nullptr || recording->isNull());
  }
}

TEST(EncoderRecordingSession, AsyncIdleStopLeavesEncoderAvailableForNonRecordingOutput) {
  auto modules = corevideo::modules::createStubModules();
  auto encoder = std::make_unique<corevideo::modules::AsyncEncoderSink>(std::move(modules.encoder));
  auto* observed = encoder.get();
  modules.encoder = std::move(encoder);
  corevideo::core::MediaCore mediaCore(std::move(modules));
  for (int i = 0; i < 3; ++i) {
    const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
        {"type", "stop-recording-session"},
        {"reason", "Recording disabled in production state."},
    });
    const auto* recording = state.get("recording");
    EXPECT_TRUE(recording == nullptr || recording->isNull());
  }
  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"rtmp"}},
  });
  ASSERT_TRUE(observed->drainForTest(std::chrono::seconds(2)));
  const auto session = observed->session();
  EXPECT_EQ(session.destinations, std::vector<std::string>{"rtmp"});
  EXPECT_FALSE(session.lifecycle.has_value());
  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "stop-recording-session"}, {"reason", "Recording disabled in production state."}});
  const auto* recording = state.get("recording");
  EXPECT_TRUE(recording == nullptr || recording->isNull());
  EXPECT_EQ(observed->session().destinations, session.destinations);
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
  ASSERT_NE(stoppedState.get("recording"), nullptr);
  EXPECT_EQ(stoppedState.get("recording")->getString("status"), "stopped");
  EXPECT_EQ(stoppedState.get("recording")->getString("writerStatus"), "stopped");
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
// #482 / T3.8, end to end through a REAL Media Foundation writer. The ISO
// writer opens lazily at its source's first frame and keeps that size; a guest
// whose frame size changes later used to have the NEW dimensions handed to that
// writer, which cropped or padded the picture. #478 made this routine: a guest
// retiers 720P <-> 1080P every time they are cued onto or off a bus.
//
// HONEST SCOPE: this is a GUARD, not a red-green proof, and it was checked --
// it passes with and without the fix. Media Foundation accepts a mismatched
// sample silently, so the frame count, the warning and even ffprobe's view of
// the container (640x360, 9 frames) are identical either way. That is the
// "verify pixels, not stream presence" trap from CLAUDE.md, met again: every
// observable this level offers survives the bug.
//
// What it does earn: the writer keeps accepting frames across two size changes
// and raises no warning, so a conform that silently dropped or failed frames
// would fail here. The PIXEL proof lives in IsoFrameConformTest, which asserts
// the scaled values and the letterbox black exactly.
TEST(EncoderRecordingSession, MediaFoundationIsoSurvivesASourceChangingFrameSize) {
  // About MP4 writers, not capacity: pin an ample machine so the asynchronous,
  // GPU-dependent probe cannot decide the outcome.
  const corevideo::testing::ForcedEncoderCapacity ampleCapacity;
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) {
    return;  // Media Foundation unavailable - nothing to test.
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto targetDir = fs::temp_directory_path() / "corevideo-iso-resize";
  fs::remove_all(targetDir, ec);

  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "iso-resize-show";
  request.targetFolder = targetDir.string();
  request.filenamePrefix = "resize";
  request.format = "mp4";
  request.quality = "high";
  request.width = 640;
  request.height = 360;
  request.fps = 30;
  request.videoCodec = "h264";
  request.audioCodec = "aac";
  request.audioBitrateKbps = 128;
  request.targetBitrateMbps = 4;
  request.isoSources = {{"zoom:A", "Alice"}};
  encoder->configureRecording(request);
  encoder->start({"recording"}, {});

  corevideo::modules::ProgramFrame frame;
  frame.width = 640;
  frame.height = 360;
  frame.preview.width = 640;
  frame.preview.height = 360;
  frame.preview.bgra.assign(static_cast<size_t>(640) * 360 * 4, 0x40);

  // Opens at 640x360 (the 1080P-tier stand-in), then the guest retiers down.
  int frameId = 0;
  for (int i = 0; i < 3; ++i) {
    frame.frameNumber = ++frameId;
    encoder->submit(frame);
    encoder->submitIsoVideo({makeIsoI420("zoom:A", 640, 360, frameId, 120)});
  }
  for (int i = 0; i < 3; ++i) {
    frame.frameNumber = ++frameId;
    encoder->submit(frame);
    encoder->submitIsoVideo({makeIsoI420("zoom:A", 320, 180, frameId, 120)});
  }
  // And back up again, because a guest cued back onto a bus retiers up.
  for (int i = 0; i < 3; ++i) {
    frame.frameNumber = ++frameId;
    encoder->submit(frame);
    encoder->submitIsoVideo({makeIsoI420("zoom:A", 640, 360, frameId, 120)});
  }

  const auto session = encoder->session();
  ASSERT_EQ(session.isoStreams.size(), 1u);
  const auto& iso = session.isoStreams[0];
  EXPECT_TRUE(iso.trackOpen) << iso.warning;
  EXPECT_TRUE(iso.warning.empty()) << iso.warning;
  // EVERY frame is muxed, across both size changes. This is the assertion the
  // defect fails: frames at the new size never reached the file.
  EXPECT_EQ(iso.videoFrameCount, 9) << iso.warning;
  EXPECT_TRUE(session.recordingWarning.empty()) << session.recordingWarning;

  encoder->stopRecording();

  ASSERT_FALSE(session.recordingSessionDir.empty());
  const fs::path isoPath = fs::path(session.recordingSessionDir) / "ISO-01-Alice.mp4";
  ASSERT_TRUE(fs::exists(isoPath));
  EXPECT_GT(fs::file_size(isoPath), 0u);
}

TEST(EncoderRecordingSession, MediaFoundationIsoWritersProduceIndependentPlayableFiles) {
  // This test is about MP4 writers, not capacity: pin an ample machine so the
  // live probe (asynchronous, GPU-dependent) cannot decide the outcome.
  const corevideo::testing::ForcedEncoderCapacity ampleCapacity;
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

  for (const auto& iso : encoder->session().isoStreams) {
    ASSERT_TRUE(iso.muxVideoFrameCount.has_value());
    EXPECT_GE(*iso.muxVideoFrameCount, 4); // committed source samples plus explicit tail padding
  }

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
TEST(EncoderRecordingSession, MediaFoundationIsoAudioTrimsCaptureBeforeProgramEpoch) {
  const corevideo::testing::ForcedEncoderCapacity capacity;
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) return;
  namespace fs = std::filesystem;
  const auto dir = fs::temp_directory_path() / "corevideo-iso-audio-epoch";
  std::error_code ec; fs::remove_all(dir, ec);
  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "iso-audio-epoch"; request.targetFolder = dir.string();
  request.filenamePrefix = "epoch"; request.width = 640; request.height = 360;
  request.fps = 30; request.isoSources = {{"zoom:A", "Guest", true}};
  encoder->configureRecording(request); encoder->start({"recording"}, {});
  corevideo::modules::ProgramFrame frame;
  frame.frameNumber = 1; frame.width = 640; frame.height = 360;
  frame.preview.width = 640; frame.preview.height = 360;
  frame.preview.bgra.assign(640 * 360 * 4, 0x40);
  encoder->submit(frame);
  encoder->submitIsoVideo({makeIsoI420("zoom:A", 640, 360, 1, 90)});
  const auto epoch = encoder->session().recordingMuxEpoch100ns;
  ASSERT_GT(epoch, 0);
  auto audio = makeIsoTone("zoom:A", 960, 1, 220, 0);
  audio.timelineTimestamp100ns = epoch - 200000; // Entire packet precedes capture.
  encoder->submitIsoAudio({audio});
  ASSERT_EQ(encoder->session().isoStreams.size(), 1u);
  EXPECT_EQ(encoder->session().isoStreams[0].audioSampleCount, 0);
  audio.timelineTimestamp100ns = epoch - 100000; // Retain only the last 10 ms.
  encoder->submitIsoAudio({audio});
  EXPECT_EQ(encoder->session().isoStreams[0].audioSampleCount, 480);
  encoder->stopRecording(); encoder.reset(); fs::remove_all(dir, ec);
}

TEST(EncoderRecordingSession, MediaFoundationProgramGapsKeepElapsedTimelineAcrossFragments) {
#if defined(_WIN32)
  {
    const corevideo::testing::ForcedEncoderCapacity capacity;
    auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
    ASSERT_NE(encoder, nullptr);
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "corevideo-program-gaps-nv12";
    std::error_code ec;
    fs::remove_all(dir, ec);
    corevideo::modules::RecordingSessionRequest request;
    request.targetFolder = dir.string(); request.filenamePrefix = "gaps";
    request.width = 320; request.height = 180; request.fps = 60;
    request.programNv12 = true;
    encoder->configureRecording(request);
    encoder->start({"recording"}, {});
    corevideo::modules::ProgramFrame frame;
    frame.frameNumber = 1; frame.width = 320; frame.height = 180;
    frame.productionAnchorNs = 1; frame.productionSlot = 0;
    frame.programNv12Width = 320; frame.programNv12Height = 180;
    frame.programNv12.assign(320 * 180 * 3 / 2, 128);
    encoder->submit(frame);
    const auto epoch = encoder->session().recordingMuxEpoch100ns;
    ASSERT_GT(epoch, 0);
    std::vector<float> pcm(1600 * 2, 0.05f);
    encoder->submitAudioAt(pcm.data(), 1600, 2, 48000, epoch);
    // Simulate missed Program delivery slots in a 60 Hz show. Preserve the
    // actual timestamps: no duplicate frames may conceal the missing pictures.
    for (int i = 1; i <= 300; ++i) {
      frame.frameNumber = 1 + i * 2;
      frame.productionSlot = i * 2;
      frame.timelineTimestamp100ns = epoch + static_cast<int64_t>(i) * 10'000'000 / 30 +
          (i % 3 == 1 ? 100'000 : 0);
      encoder->submit(frame);
      encoder->submitAudioAt(pcm.data(), 1600, 2, 48000,
          epoch + static_cast<int64_t>(i) * 10'000'000 / 30);
    }
    EXPECT_EQ(encoder->session().recordingProgramMissingFrames, 300);
    EXPECT_TRUE(encoder->session().recordingProgramContinuityObserved);
    EXPECT_EQ(encoder->session().recordingStatus, "warning");
    EXPECT_NE(encoder->session().recordingWarning.find("missed 300"), std::string::npos);
    encoder->stopRecording();
    EXPECT_EQ(encoder->session().recordingProgramMissingFrames, 300);
    const fs::path path = encoder->session().recordingArtifactPath;
    EXPECT_GE(fileAsciiCount(path, "moof"), 2u);
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    ASSERT_TRUE(SUCCEEDED(MFCreateSourceReaderFromURL(path.wstring().c_str(), nullptr, &reader)));
    LONGLONG lastPts = -1;
    int samples = 0;
    for (;;) {
      DWORD flags = 0; LONGLONG pts = 0;
      Microsoft::WRL::ComPtr<IMFSample> sample;
      ASSERT_TRUE(SUCCEEDED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
          0, nullptr, &flags, &pts, &sample)));
      if (sample) { lastPts = pts; ++samples; }
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    EXPECT_EQ(encoder->session().recordingVideoFrameCount, 301);
    EXPECT_EQ(samples, 301 + encoder->session().recordingVideoTailFrameCount);
    EXPECT_GE(lastPts, 99'990'000);
    EXPECT_LE(lastPts, 100'350'000);
    reader.Reset(); encoder.reset();
    // Keep a failing artifact for independent packet-level inspection.
    if (lastPts >= 99'990'000 && lastPts <= 100'350'000) fs::remove_all(dir, ec);
  }
#endif
}

TEST(EncoderRecordingSession, MediaFoundationIsoVariableRateKeepsElapsedTimelineAcrossFragments) {
#if defined(_WIN32)
  for (const int hardwareLimit : {1, 8}) {
  const corevideo::testing::ForcedEncoderCapacity capacity(hardwareLimit);
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  ASSERT_NE(encoder, nullptr);
  namespace fs = std::filesystem;
  const auto dir = fs::temp_directory_path() / "corevideo-iso-vfr-timeline";
  std::error_code ec; fs::remove_all(dir, ec);
  corevideo::modules::RecordingSessionRequest request;
  request.targetFolder = dir.string(); request.filenamePrefix = "vfr";
  request.width = 320; request.height = 180; request.fps = 60;
  request.isoSources = {{"zoom:vfr", "Variable", false}};
  encoder->configureRecording(request); encoder->start({"recording"}, {});
  corevideo::modules::ProgramFrame program;
  program.frameNumber = 1; program.width = 320; program.height = 180;
  program.preview.width = 320; program.preview.height = 180;
  program.preview.bgra.assign(320 * 180 * 4, 0x40);
  encoder->submit(program);
  const auto epoch = encoder->session().recordingMuxEpoch100ns;
  ASSERT_GT(epoch, 0);
  for (int i = 0; i <= 300; ++i) {
    auto frame = makeIsoI420("zoom:vfr", 320, 180, i + 1, 80 + i % 80);
    frame.timelineTimestamp100ns = epoch + static_cast<int64_t>(i) * 10'000'000 / 30 +
        (i % 3 == 1 ? 100'000 : 0);
    encoder->submitIsoVideo({frame});
  }
  encoder->stopRecording();
  const auto session = encoder->session();
  ASSERT_EQ(session.isoStreams.size(), 1u);
  const fs::path path = session.isoStreams[0].path;
  EXPECT_GE(fileAsciiCount(path, "moof"), 2u);
  Microsoft::WRL::ComPtr<IMFSourceReader> reader;
  ASSERT_TRUE(SUCCEEDED(MFCreateSourceReaderFromURL(path.wstring().c_str(), nullptr, &reader)));
  LONGLONG lastPts = -1;
  int samples = 0;
  for (;;) {
    DWORD flags = 0; LONGLONG pts = 0;
    Microsoft::WRL::ComPtr<IMFSample> sample;
    const auto hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &pts, &sample);
    ASSERT_TRUE(SUCCEEDED(hr));
    if (sample) { lastPts = pts; ++samples; }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
  }
  EXPECT_EQ(samples, 301);
  EXPECT_GE(lastPts, 99'990'000);
  EXPECT_LE(lastPts, 100'010'000);
  reader.Reset(); encoder.reset();
  // Preserve the generated file on failure for independent ffprobe inspection.
  if (lastPts >= 99'990'000 && lastPts <= 100'010'000) fs::remove_all(dir, ec);
  }
#endif
}

TEST(EncoderRecordingSession, MediaFoundationIsoWritersMuxOwnAudioStems) {
  // This test is about MP4 writers, not capacity: pin an ample machine so the
  // live probe (asynchronous, GPU-dependent) cannot decide the outcome.
  const corevideo::testing::ForcedEncoderCapacity ampleCapacity;
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
  // Pin EXACTLY the capacity the product used to assume (8 sessions, Program owns
  // one) so the 8th ISO still has to spill — that spill is what this test is for.
  const corevideo::testing::ForcedEncoderCapacity eightSessionMachine(8);
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
  // The spill is REPORTED now, not silent: this used to assert an empty warning
  // while one stem quietly went to the CPU encoder. Program keeps recording and
  // all eight ISOs still arm — the change is that the operator is told.
  EXPECT_NE(session.recordingWarning.find("1 ISO source will record on the CPU software encoder"),
            std::string::npos)
      << session.recordingWarning;
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
  // #935: the same verdict, structured, so no reader parses recording.warning.
  ASSERT_TRUE(session.isoAdmission.has_value());
  EXPECT_EQ(session.isoAdmission->decision, "admit-with-warning");
  EXPECT_EQ(session.isoAdmission->code.rfind("iso-encoder-software-spill", 0), 0u) << session.isoAdmission->code;
  EXPECT_EQ(session.isoAdmission->hardwareCount, 7);
  EXPECT_EQ(session.isoAdmission->softwareCount, 1);
  EXPECT_EQ(session.isoAdmission->unplacedCount, 0);
  EXPECT_EQ(session.isoAdmission->message.rfind("1 ISO source will record on the CPU software encoder", 0), 0u);
  EXPECT_EQ(session.isoAdmission->message, session.recordingWarning);
  EXPECT_FALSE(session.isoAdmission->workload.empty());
  EXPECT_FALSE(session.isoAdmission->probeStatus.empty());
  for (size_t i = 0; i < session.isoStreams.size(); ++i) {
    const auto& iso = session.isoStreams[i];
    EXPECT_EQ(iso.isoIndex, static_cast<int>(i + 1)) << iso.sourceId;  // matches ISO-NN in the file name
    if (iso.encoderPath == "software") EXPECT_EQ(iso.isoIndex, 8) << iso.sourceId;  // the overflow is the last placed
  }

  encoder->stopRecording();
  EXPECT_TRUE(encoder->session().isoAdmission.has_value()) << "the finished take keeps its admission";
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

TEST(EncoderRecordingSession, MediaFoundationProgramOnlyTakeAfterSpillClearsIsoAdmission) {
  // #935 review focus: a Program-only take must not show the previous take's spill.
  const corevideo::testing::ForcedEncoderCapacity eightSessionMachine(8);
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) {
    return;  // Media Foundation unavailable.
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto targetDir = fs::temp_directory_path() / "corevideo-iso935-admission-reset";
  fs::remove_all(targetDir, ec);
  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "iso935-spill";
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
  ASSERT_TRUE(encoder->session().isoAdmission.has_value());
  EXPECT_EQ(encoder->session().isoAdmission->decision, "admit-with-warning");
  encoder->stopRecording();

  request.sessionId = "iso935-program-only";
  request.isoSources.clear();
  request.isoParticipantIds.clear();
  encoder->configureRecording(request);
  encoder->start({"recording"}, {});
  const auto session = encoder->session();
  EXPECT_FALSE(session.isoAdmission.has_value()) << "stale admission from the previous take";
  EXPECT_TRUE(session.isoStreams.empty());
  encoder->stopRecording();
  encoder.reset();
  fs::remove_all(targetDir, ec);
}

TEST(EncoderRecordingSession, MediaFoundationIsoFormatChangeBeforeTakeDoesNotFinalizeEmptyMp4) {
  const corevideo::testing::ForcedEncoderCapacity capacity(8);
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  ASSERT_TRUE(encoder);
  encoder->enableIndependentIsoWriters();
  const auto target = std::filesystem::temp_directory_path() /
      ("corevideo-iso-prepare-resize-" + std::to_string(GetCurrentProcessId()));
  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "prepare-resize";
  request.targetFolder = target.string(); request.filenamePrefix = "show";
  request.format = "mp4"; request.width = 640; request.height = 360;
  request.fps = 30; request.videoCodec = "h264"; request.audioCodec = "aac";
  request.audioBitrateKbps = 128; request.targetBitrateMbps = 8;
  request.isoSources = {{"zoom:1", "Changing", false}, {"zoom:2", "Late", false}};
  encoder->configureRecording(request); encoder->start({"recording"}, {});
  encoder->submitIsoVideo({makeIsoI420("zoom:1", 640, 360, 0, 40)});
  bool firstReady = false;
  for (int i = 0; i < 500 && !firstReady; ++i) {
    const auto state = encoder->session();
    firstReady = std::any_of(state.isoStreams.begin(), state.isoStreams.end(),
        [](const auto& track) { return track.sourceId == "zoom:1" && track.trackOpen; });
    if (!firstReady) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(firstReady);
  // The second source is still absent: no take/epoch exists when Zoom changes size.
  encoder->submitIsoVideo({makeIsoI420("zoom:1", 1280, 720, 1, 80)});
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  encoder->submitIsoVideo({makeIsoI420("zoom:2", 640, 360, 1, 120)});
  corevideo::modules::ProgramFrame frame;
  frame.width = frame.preview.width = 640; frame.height = frame.preview.height = 360;
  frame.preview.bgra.assign(static_cast<size_t>(640) * 360 * 4, 0x40);
  bool started = false;
  for (int i = 0; i < 500 && !started; ++i) {
    frame.frameNumber = i + 1; encoder->submit(frame);
    const auto state = encoder->session();
    ASSERT_TRUE(state.recordingError.empty()) << state.recordingError;
    for (const auto& track : state.isoStreams) EXPECT_TRUE(track.warning.empty()) << track.warning;
    started = state.recordingMuxEpoch100ns > 0;
    if (!started) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(started);
  encoder->stopRecording(); encoder.reset();
  std::error_code cleanupError;
  std::filesystem::remove_all(target, cleanupError);
}

TEST(EncoderRecordingSession, MediaFoundationIndependentIsoWritersDrainAudioVideoBeforeFinalize) {
  // Pin EXACTLY the capacity the product used to assume (8 sessions, Program owns
  // one) so the 8th ISO still has to spill — that spill is what this test is for.
  const corevideo::testing::ForcedEncoderCapacity eightSessionMachine(8);
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) {
    return;  // Media Foundation unavailable.
  }
  encoder->enableIndependentIsoWriters();
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto targetDir = fs::temp_directory_path() / "corevideo-iso8-independent";
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

  // The click requests preparation; the promised take starts after every
  // native source geometry/encoder is ready. No preparation pixels enter files.
  std::vector<corevideo::modules::IsoSourceVideoFrame> preparationSources;
  for (int source = 0; source < 8; ++source)
    preparationSources.push_back(makeIsoI420("zoom:" + std::to_string(source + 1), 640, 360, 0,
                                             static_cast<uint8_t>(40 + source * 20)));
  encoder->submitIsoVideo(preparationSources);
  bool prepared = false;
  for (int attempt = 0; attempt < 500 && !prepared; ++attempt) {
    const auto observed = encoder->session();
    prepared = observed.isoStreams.size() == 8 && std::all_of(observed.isoStreams.begin(), observed.isoStreams.end(),
        [](const auto& track) { return track.trackOpen; });
    if (!prepared) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(prepared);
  EXPECT_FALSE(encoder->progress().allRecordingWritersCommitted);
  frame.frameNumber = 1;
  encoder->submit(frame);
  bool captureStarted = false;
  for (int attempt = 0; attempt < 500 && !captureStarted; ++attempt) {
    captureStarted = encoder->session().recordingMuxEpoch100ns > 0;
    if (!captureStarted) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(captureStarted);

  for (int tick = 0; tick < 60; ++tick) {
    frame.frameNumber = tick + 2;
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
    std::this_thread::sleep_for(std::chrono::milliseconds(34));
  }

  encoder->stopRecording();
  const auto session = encoder->session();
  EXPECT_EQ(session.encoderQueueDroppedVideoFrames, 0);
  EXPECT_EQ(session.encoderQueueDroppedAudioPackets, 0);
  // The spill is REPORTED now, not silent: this used to assert an empty warning
  // while one stem quietly went to the CPU encoder. Program keeps recording and
  // all eight ISOs still arm — the change is that the operator is told.
  EXPECT_NE(session.recordingWarning.find("1 ISO source will record on the CPU software encoder"),
            std::string::npos)
      << session.recordingWarning;
  ASSERT_EQ(session.isoStreams.size(), 8u);
  int hardwareIsoCount = 0;
  int softwareIsoCount = 0;
  for (const auto& iso : session.isoStreams) {
    EXPECT_TRUE(iso.trackOpen) << iso.sourceId << ": " << iso.warning;
    EXPECT_TRUE(iso.warning.empty()) << iso.sourceId << ": " << iso.warning;
    EXPECT_EQ(iso.videoFrameCount, 60) << iso.sourceId;
    EXPECT_GT(iso.audioSampleCount, 0) << iso.sourceId;
    hardwareIsoCount += iso.encoderPath == "hardware" ? 1 : 0;
    softwareIsoCount += iso.encoderPath == "software" ? 1 : 0;
  }
  EXPECT_EQ(hardwareIsoCount, 7);
  EXPECT_EQ(softwareIsoCount, 1);

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
  // This test is about MP4 writers, not capacity: pin an ample machine so the
  // live probe (asynchronous, GPU-dependent) cannot decide the outcome.
  const corevideo::testing::ForcedEncoderCapacity ampleCapacity;
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
  // This test is about MP4 writers, not capacity: pin an ample machine so the
  // live probe (asynchronous, GPU-dependent) cannot decide the outcome.
  const corevideo::testing::ForcedEncoderCapacity ampleCapacity;
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

// #551: the async independent ISO writers (the live app path) must report
// framesWritten MID-recording. Only the synchronous path was covered, so a
// status that stayed 0 until finalize slipped through while the files grew.
TEST(EncoderRecordingSession, MediaFoundationIndependentIsoStatusAdvancesMidRecording) {
  const corevideo::testing::ForcedEncoderCapacity capacity(8);
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) return;  // Media Foundation unavailable.
  encoder->enableIndependentIsoWriters();
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto targetDir = fs::temp_directory_path() /
      ("corevideo-iso-midstatus-" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(targetDir, ec);

  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "iso-midstatus";
  request.targetFolder = targetDir.string();
  request.filenamePrefix = "show";
  request.format = "mp4"; request.quality = "high";
  request.width = 640; request.height = 360; request.fps = 30;
  request.videoCodec = "h264"; request.audioCodec = "aac";
  request.audioBitrateKbps = 128; request.targetBitrateMbps = 8;
  request.isoSources = {{"zoom:1", "Alice", true}};
  encoder->configureRecording(request);
  encoder->start({"recording"}, {});

  corevideo::modules::ProgramFrame frame;
  frame.width = frame.preview.width = 640; frame.height = frame.preview.height = 360;
  frame.preview.bgra.assign(static_cast<size_t>(640) * 360 * 4, 0x40);
  encoder->submitIsoVideo({makeIsoI420("zoom:1", 640, 360, 0, 40)});
  bool prepared = false;
  for (int i = 0; i < 500 && !prepared; ++i) {
    const auto s = encoder->session();
    prepared = s.isoStreams.size() == 1 && s.isoStreams[0].trackOpen;
    if (!prepared) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(prepared);
  frame.frameNumber = 1;
  encoder->submit(frame);
  bool started = false;
  for (int i = 0; i < 500 && !started; ++i) {
    started = encoder->session().recordingMuxEpoch100ns > 0;
    if (!started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(started);

  int64_t sourceFrames = 0;
  for (int tick = 0; tick < 30; ++tick) {
    frame.frameNumber = tick + 2;
    encoder->submit(frame);
    encoder->submitIsoVideo({makeIsoI420("zoom:1", 640, 360, tick, 40)});
    encoder->submitIsoAudio({makeIsoTone("zoom:1", 960, 1, 220.0, sourceFrames)});
    sourceFrames += 960;
    std::this_thread::sleep_for(std::chrono::milliseconds(34));
  }

  // Deliberately NOT stopped: this is the live-recording read.
  int64_t framesMid = 0, bytesMid = 0;
  for (int i = 0; i < 200 && framesMid <= 0; ++i) {
    const auto s = encoder->session();
    ASSERT_EQ(s.isoStreams.size(), 1u);
    framesMid = s.isoStreams[0].videoFrameCount;
    bytesMid = s.isoStreams[0].bytesWritten;
    if (framesMid <= 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GT(framesMid, 0) << "async ISO writer reported framesWritten=0 mid-recording";
  EXPECT_GT(bytesMid, 0) << "async ISO writer reported bytesWritten=0 mid-recording";

  encoder->stopRecording();
  encoder.reset();
  fs::remove_all(targetDir, ec);
}

// #1030 (live show 2026-10-23): a Zoom ISO participant left mid-recording. His
// ISO video stopped, the writer kept muxing audio silence, Media Foundation's
// CMFSinkWriter::ThrottleStream blocked that WriteSample forever (no sibling
// video), and the next Start's closeWriters() joined that worker without a
// bound — the recording file-control lane never completed another call.
namespace {
int64_t steady100ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
}

corevideo::modules::RecordingSessionRequest departureRequest(const std::filesystem::path& dir,
                                                             const std::string& id) {
  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = id;
  request.targetFolder = dir.string();
  request.filenamePrefix = "show";
  request.format = "mp4"; request.quality = "high";
  request.width = 640; request.height = 360; request.fps = 30;
  request.videoCodec = "h264"; request.audioCodec = "aac";
  request.audioBitrateKbps = 128; request.targetBitrateMbps = 8;
  request.isoSources = {{"zoom:1", "Leaver", true}};
  return request;
}

// Opens the take (ISO prepared, then the shared epoch selected from a Program
// frame stamped after writer readiness; that stamp is returned in `base`).
// Returns false when the environment cannot open the writers.
bool openDepartureTake(corevideo::modules::IEncoderSink& encoder, int64_t& base,
                       corevideo::modules::ProgramFrame& frame) {
  encoder.start({"recording"}, {});
  auto first = makeIsoI420("zoom:1", 640, 360, 0, 40);
  first.timelineTimestamp100ns = steady100ns();
  encoder.submitIsoVideo({first});
  bool prepared = false;
  for (int i = 0; i < 500 && !prepared; ++i) {
    const auto s = encoder.session();
    prepared = s.isoStreams.size() == 1 && s.isoStreams[0].trackOpen;
    if (!prepared) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!prepared) return false;
  frame.width = frame.preview.width = 640; frame.height = frame.preview.height = 360;
  frame.preview.bgra.assign(static_cast<size_t>(640) * 360 * 4, 0x40);
  frame.frameNumber = 1;
  base = steady100ns();
  frame.timelineTimestamp100ns = base;
  encoder.submit(frame);
  for (int i = 0; i < 500; ++i) {
    if (encoder.session().recordingMuxEpoch100ns > 0) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

// Program's mix for one 20 ms tick. Without it the Program file's audio track
// stays empty and its Finalize drain legitimately waits out its own 2 s bound.
void submitProgramTick(corevideo::modules::IEncoderSink& encoder, int64_t at) {
  static const std::vector<float> pcm(static_cast<size_t>(960) * 2, 0.05f);
  encoder.submitAudioAt(pcm.data(), 960, 2, 48000, at);
}

// One second of a live participant (video + tone every 20 ms tick), then
// `silentTicks` audio-only ticks with NO video: the participant has left. The
// timeline runs ahead of the wall clock so 30 s of departure costs ~1.5 s.
void driveDeparture(corevideo::modules::IEncoderSink& encoder, int64_t base,
                    corevideo::modules::ProgramFrame& frame, int silentTicks, bool markDeparted,
                    const std::function<void()>& afterLivePhase = {}) {
  constexpr int kLiveTicks = 50;
  constexpr int64_t kTick100ns = 200'000;  // 20 ms
  int64_t sourceFrames = 0;
  for (int tick = 1; tick <= kLiveTicks; ++tick) {
    const int64_t at = base + tick * kTick100ns;
    frame.frameNumber = tick + 1;
    frame.timelineTimestamp100ns = at;
    encoder.submit(frame);
    submitProgramTick(encoder, at);
    auto video = makeIsoI420("zoom:1", 640, 360, tick, 40);
    video.timelineTimestamp100ns = at;
    encoder.submitIsoVideo({video});
    auto tone = makeIsoTone("zoom:1", 960, 1, 220.0, sourceFrames);
    tone.timelineTimestamp100ns = at;
    encoder.submitIsoAudio({tone});
    sourceFrames += 960;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (afterLivePhase) afterLivePhase();
  for (int tick = kLiveTicks + 1; tick <= kLiveTicks + silentTicks; ++tick) {
    corevideo::modules::IsoSourceAudio silent;  // the core's empty tick: no PCM
    silent.sourceId = "zoom:1";
    silent.sampleRate = 48000;
    silent.timelineTimestamp100ns = base + tick * kTick100ns;
    silent.departed = markDeparted;
    encoder.submitIsoAudio({silent});
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// Waits until the ISO's file worker has drained, then returns the on-disk
// size of its fragmented MP4 (fragments are written as the sink emits them).
uint64_t drainedIsoFileBytes(corevideo::modules::IEncoderSink& encoder) {
  std::string path;
  for (int i = 0; i < 500; ++i) {
    const auto s = encoder.session();
    if (s.isoStreams.size() == 1 && s.isoStreams[0].queuedAudioPackets == 0 &&
        s.isoStreams[0].queuedVideoFrames == 0) { path = s.isoStreams[0].path; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Give the fragment writer (>= 1 s fragments of media time) its moment.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  std::error_code ec;
  const auto size = path.empty() ? 0 : std::filesystem::file_size(path, ec);
  return ec ? 0 : static_cast<uint64_t>(size);
}

int64_t elapsedMs(std::chrono::steady_clock::time_point begin) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
}
}  // namespace

// Part 1 (MF_SINK_WRITER_DISABLE_THROTTLING) + part 3 (bounded close): an ISO
// whose video stops while its audio keeps arriving must still close and
// finalize promptly, never by abandonment. Red before the fix: the silence
// writes blocked in ThrottleStream and Stop hung (or, with only the bounded
// join, the writer was abandoned and the file never finalized).
TEST(EncoderRecordingSession, MediaFoundationIsoWhoseVideoStopsWhileAudioContinuesClosesWithinBound) {
  const corevideo::testing::ForcedEncoderCapacity capacity(8);
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) return;  // Media Foundation unavailable.
  encoder->enableIndependentIsoWriters();
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto targetDir = fs::temp_directory_path() /
      ("corevideo-iso-video-stops-" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(targetDir, ec);
  encoder->configureRecording(departureRequest(targetDir, "iso-video-stops"));
  corevideo::modules::ProgramFrame frame;
  int64_t base = 0;
  ASSERT_TRUE(openDepartureTake(*encoder, base, frame));

  uint64_t bytesAtDeparture = 0;
  driveDeparture(*encoder, base, frame, 1500, /*markDeparted=*/false,
                 [&] { bytesAtDeparture = drainedIsoFileBytes(*encoder); });
  // Let the file worker reach the silence; a throttled writer stops here.
  const uint64_t bytesAfterSilence = drainedIsoFileBytes(*encoder);
  // Blocker 2 of the #1030 review: with throttling off nothing blocks audio
  // that runs ahead of stopped video, so prove it is not merely accumulating
  // inside the sink writer: 30 s of AAC (128 kbps nominal) must reach disk as
  // audio-only fragments BEFORE Stop, not appear only at Finalize.
  std::printf("[iso-departure-growth] bytes_at_departure=%llu bytes_after_30s_silence=%llu growth=%llu\n",
              static_cast<unsigned long long>(bytesAtDeparture),
              static_cast<unsigned long long>(bytesAfterSilence),
              static_cast<unsigned long long>(bytesAfterSilence - (std::min)(bytesAfterSilence, bytesAtDeparture)));
  EXPECT_GT(bytesAtDeparture, 0u);
  EXPECT_GT(bytesAfterSilence, bytesAtDeparture + 100'000)
      << "audio after the video stopped did not reach disk; the sink writer is holding it";

  // The camera comes back after the gap: frames stamped after the held
  // frames are accepted (no dropped-video warning) and muxed.
  constexpr int64_t kTick = 200'000;
  for (int tick = 1551; tick <= 1560; ++tick) {
    const int64_t at = base + tick * kTick;
    auto video = makeIsoI420("zoom:1", 640, 360, tick, 40);
    video.timelineTimestamp100ns = at;
    encoder->submitIsoVideo({video});
    auto tone = makeIsoTone("zoom:1", 960, 1, 220.0, tick * 960LL);
    tone.timelineTimestamp100ns = at;
    encoder->submitIsoAudio({tone});
  }
  (void)drainedIsoFileBytes(*encoder);

  const auto stopBegin = std::chrono::steady_clock::now();
  encoder->stopRecording();
  const auto stopMs = elapsedMs(stopBegin);
  EXPECT_LT(stopMs, 3500) << "closing a session whose ISO video stopped must not wait out the join bound";

  const auto s = encoder->session();
  ASSERT_EQ(s.isoStreams.size(), 1u);
  EXPECT_EQ(s.isoStreams[0].warning.find("abandoned"), std::string::npos) << s.isoStreams[0].warning;
  EXPECT_EQ(s.recordingError.find("abandoned"), std::string::npos) << s.recordingError;
  EXPECT_GT(s.isoStreams[0].bytesWritten, 0);
  // The scenario really reached the writer: ~30 s of silence after one second
  // of real audio was muxed, ahead of a video track that had stopped.
  EXPECT_GT(s.isoStreams[0].audioSampleCount, 48000LL * 20);
  EXPECT_EQ(s.isoStreams[0].warning.find("dropped video"), std::string::npos) << s.isoStreams[0].warning;
  EXPECT_GE(s.isoStreams[0].videoFrameCount, 60) << "resumed camera frames must be muxed after the stall";
  encoder.reset();
  fs::remove_all(targetDir, ec);
}

// #1030 re-review: the camera returns with a frame stamped BEHIND the last
// held video frame. While video is stalled the writer holds the last picture
// once per second of media, 0.5 s behind the audio, so after a silent run
// ending at A the last hold lies within [A - 1.52 s, A - 0.5 s]; a frame at
// A - 1.8 s is therefore at least 0.28 s behind it (and after the last real
// frame at ~1 s). Pinned behaviour: the late frame is muxed just after that
// hold (never dropped, PTS strictly monotonic) and the resumed camera lands on
// its own capture times, so A/V sync holds. This test is what rejected the
// first design (IMFSinkWriter::SendStreamTick): the fMP4 sink lays video out
// by accumulated sample durations and the tick collapsed the 28 s gap, so the
// resumed frames muxed at ~2.8 s against audio at 31 s (monotonic, wrong).
TEST(EncoderRecordingSession, MediaFoundationIsoFrameStampedBehindTheLastHeldFrame) {
  const corevideo::testing::ForcedEncoderCapacity capacity(8);
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) return;  // Media Foundation unavailable.
  encoder->enableIndependentIsoWriters();
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto targetDir = fs::temp_directory_path() /
      ("corevideo-iso-behind-tick-" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(targetDir, ec);
  encoder->configureRecording(departureRequest(targetDir, "iso-behind-tick"));
  corevideo::modules::ProgramFrame frame;
  int64_t base = 0;
  ASSERT_TRUE(openDepartureTake(*encoder, base, frame));

  driveDeparture(*encoder, base, frame, 1500, /*markDeparted=*/false);
  (void)drainedIsoFileBytes(*encoder);  // the holds have been written by now
  constexpr int64_t kTick = 200'000;
  const int64_t audioEnd = base + 1550 * kTick;  // A = 31.0 s of media
  // The returning camera's first frame, behind the last held frame.
  auto late = makeIsoI420("zoom:1", 640, 360, 5000, 40);
  late.timelineTimestamp100ns = audioEnd - 18'000'000;
  encoder->submitIsoVideo({late});
  // Then the camera runs normally again with its audio.
  for (int i = 1; i <= 10; ++i) {
    const int64_t at = audioEnd + i * kTick;
    auto video = makeIsoI420("zoom:1", 640, 360, 5000 + i, 40);
    video.timelineTimestamp100ns = at;
    encoder->submitIsoVideo({video});
    auto tone = makeIsoTone("zoom:1", 960, 1, 220.0, (1550 + i) * 960LL);
    tone.timelineTimestamp100ns = at;
    encoder->submitIsoAudio({tone});
  }
  (void)drainedIsoFileBytes(*encoder);
  encoder->stopRecording();

  const auto s = encoder->session();
  ASSERT_EQ(s.isoStreams.size(), 1u);
  const auto& iso = s.isoStreams[0];
  const fs::path path = iso.path;
  Microsoft::WRL::ComPtr<IMFSourceReader> reader;
  ASSERT_TRUE(SUCCEEDED(MFCreateSourceReaderFromURL(path.wstring().c_str(), nullptr, &reader)));
  std::vector<LONGLONG> pts;
  for (;;) {
    DWORD flags = 0; LONGLONG at = 0;
    Microsoft::WRL::ComPtr<IMFSample> sample;
    ASSERT_TRUE(SUCCEEDED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &at, &sample)));
    if (sample) pts.push_back(at);
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
  }
  PROPVARIANT duration; PropVariantInit(&duration);
  ASSERT_TRUE(SUCCEEDED(reader->GetPresentationAttribute(
      static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)));
  const auto fileDuration100ns = static_cast<int64_t>(duration.uhVal.QuadPart);
  PropVariantClear(&duration);
  bool monotonic = true;
  for (size_t i = 1; i < pts.size(); ++i) {
    if (pts[i] <= pts[i - 1]) {
      monotonic = false;
      std::printf("[iso-behind-tick] non-monotonic at %zu: %lld then %lld%c", i,
                  static_cast<long long>(pts[i - 1]), static_cast<long long>(pts[i]), 10);
    }
  }
  const auto latePts = audioEnd - 18'000'000 - base;
  // The last hold lies in [A - 1.52 s, A - 0.5 s]; the late frame follows it.
  const bool lateMuxed = std::any_of(pts.begin(), pts.end(),
      [&](LONGLONG at) { return at >= latePts && at <= latePts + 13'200'000; });
  std::printf("[iso-behind-tick] samples=%zu framesWritten=%lld late_muxed=%d monotonic=%d last_pts=%.3f "
              "file_duration=%.3f warning='%s'\n",
              pts.size(), static_cast<long long>(iso.videoFrameCount), lateMuxed ? 1 : 0, monotonic ? 1 : 0,
              pts.empty() ? -1.0 : pts.back() / 1e7, fileDuration100ns / 1e7, iso.warning.c_str());

  // Pinned behaviour (measured on the dev box, see iso-recording.md #1030):
  EXPECT_TRUE(monotonic) << "video PTS in the file went backwards";
  EXPECT_TRUE(lateMuxed) << "the frame stamped behind the held frame was not muxed after it";
  // A/V sync after the gap: the resumed frames sit on their capture times.
  const auto nearest = [&](int64_t want) {
    int64_t best = (std::numeric_limits<int64_t>::max)();
    for (const auto at : pts) best = (std::min)(best, std::abs(static_cast<int64_t>(at) - want));
    return best;
  };
  // Within one 30 fps frame (the VFR writer's existing placement is 20 ms
  // early here with or without a stall); the defect this guards was 28 s.
  EXPECT_LE(nearest(audioEnd - base + kTick), 333'333) << "the first resumed frame is not at its capture time";
  EXPECT_LE(nearest(audioEnd - base + 10 * kTick), 333'333) << "the last resumed frame is not at its capture time";
  EXPECT_NEAR(static_cast<double>(pts.back()), static_cast<double>(audioEnd - base + 10 * kTick), 333'333.0)
      << "the timeline after the gap is wrong (gap collapsed?)";
  // The stall itself was filled by held pictures, about one per second.
  EXPECT_GE(pts.size(), 61u + 25u) << "no held frames bridged the 30 s video stall";
  EXPECT_EQ(iso.warning.find("dropped video"), std::string::npos) << iso.warning;
  EXPECT_EQ(iso.videoFrameCount, 61) << "50 live + the late frame + 10 resumed";
  // The file still finalizes to its real end: A + 0.2 s of resumed media.
  EXPECT_GE(fileDuration100ns, audioEnd - base + 1'900'000);
  EXPECT_LE(fileDuration100ns, audioEnd - base + 3'000'000);
  reader.Reset(); encoder.reset();
  if (monotonic && lateMuxed) fs::remove_all(targetDir, ec);
}

// Part 2: once the core marks the participant departed, the stem is no
// longer silence-filled, so the file ends at the departure (its real
// duration) instead of growing with silence until Stop.
TEST(EncoderRecordingSession, MediaFoundationDepartedIsoParticipantIsNotSilenceFilled) {
  const corevideo::testing::ForcedEncoderCapacity capacity(8);
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) return;  // Media Foundation unavailable.
  encoder->enableIndependentIsoWriters();
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto targetDir = fs::temp_directory_path() /
      ("corevideo-iso-departed-" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(targetDir, ec);
  encoder->configureRecording(departureRequest(targetDir, "iso-departed"));
  corevideo::modules::ProgramFrame frame;
  int64_t base = 0;
  ASSERT_TRUE(openDepartureTake(*encoder, base, frame));

  driveDeparture(*encoder, base, frame, 1500, /*markDeparted=*/true);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto stopBegin = std::chrono::steady_clock::now();
  encoder->stopRecording();
  EXPECT_LT(elapsedMs(stopBegin), 3500);

  const auto s = encoder->session();
  ASSERT_EQ(s.isoStreams.size(), 1u);
  EXPECT_EQ(s.isoStreams[0].warning.find("abandoned"), std::string::npos) << s.isoStreams[0].warning;
  EXPECT_GT(s.isoStreams[0].audioSampleCount, 0);
  // One second of real audio (plus the close-time tail reconcile to the last
  // video frame), never the 30 s of post-departure silence.
  EXPECT_LT(s.isoStreams[0].audioSampleCount, 48000LL * 3) << "departed stem kept growing with silence";
  EXPECT_GT(s.isoStreams[0].bytesWritten, 0);
  encoder.reset();
  fs::remove_all(targetDir, ec);
}

// Part 3 through the real sink: an ISO writer that does not return within the
// join bound is abandoned, Stop returns inside the bound, the operator is told
// which file, the lane serves the next take, and the late writer finalizes
// and is released without crashing. The stall is the existing headless QA
// seam (RecordingWriteStallQa), held longer than the test's join bound.
TEST(EncoderRecordingSession, MediaFoundationStopAbandonsAnIsoWriterThatMissesTheJoinBound) {
  struct ScopedEnv {
    const char* name;
    ScopedEnv(const char* n, const char* v) : name(n) { ::SetEnvironmentVariableA(n, v); _putenv_s(n, v); }
    ~ScopedEnv() { ::SetEnvironmentVariableA(name, nullptr); _putenv_s(name, ""); }
  };
  // Long enough for Program's own finalize (its encoder drain may take its
  // full 2 s), far shorter than the ISO stall.
  const ScopedEnv bound("COREVIDEO_RECORDING_TRACK_JOIN_BOUND_MS", "3000");
  const ScopedEnv source("COREVIDEO_QA_RECORDING_STALL_SOURCE", "zoom:1");
  const ScopedEnv stall("COREVIDEO_QA_RECORDING_STALL_MS", "8000");
  const ScopedEnv after("COREVIDEO_QA_RECORDING_STALL_AFTER_FRAMES", "30");
  const corevideo::testing::ForcedEncoderCapacity capacity(8);
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) return;  // Media Foundation unavailable.
  encoder->enableIndependentIsoWriters();
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto targetDir = fs::temp_directory_path() /
      ("corevideo-iso-abandon-" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(targetDir, ec);
  encoder->configureRecording(departureRequest(targetDir, "iso-abandon"));
  corevideo::modules::ProgramFrame frame;
  int64_t base = 0;
  ASSERT_TRUE(openDepartureTake(*encoder, base, frame));

  int64_t sourceFrames = 0;
  for (int tick = 1; tick <= 45; ++tick) {
    frame.frameNumber = tick + 1;
    frame.timelineTimestamp100ns = steady100ns();
    encoder->submit(frame);
    submitProgramTick(*encoder, frame.timelineTimestamp100ns);
    auto video = makeIsoI420("zoom:1", 640, 360, tick, 40);
    video.timelineTimestamp100ns = frame.timelineTimestamp100ns;
    encoder->submitIsoVideo({video});
    auto tone = makeIsoTone("zoom:1", 960, 1, 220.0, sourceFrames);
    tone.timelineTimestamp100ns = frame.timelineTimestamp100ns;
    encoder->submitIsoAudio({tone});
    sourceFrames += 960;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  // The ISO writer is now inside its 2 s stall (it began at written frame 30).
  const auto stopBegin = std::chrono::steady_clock::now();
  encoder->stopRecording();
  const auto stopMs = elapsedMs(stopBegin);
  EXPECT_LT(stopMs, 4500) << "Stop must return at the join bound, not wait out the blocked writer";
  const auto stopped = encoder->session();
  ASSERT_EQ(stopped.isoStreams.size(), 1u);
  EXPECT_NE(stopped.isoStreams[0].warning.find("abandoned"), std::string::npos) << stopped.isoStreams[0].warning;
  EXPECT_NE(stopped.recordingError.find("Leaver"), std::string::npos) << stopped.recordingError;

  // The file-control lane is free: a Program-only take starts and stops.
  auto next = departureRequest(targetDir, "after-abandon");
  next.isoSources.clear();
  encoder->configureRecording(next);
  const auto nextBegin = std::chrono::steady_clock::now();
  bool started = true;
  try { encoder->start({"recording"}, {}); } catch (...) { started = false; }
  ASSERT_TRUE(started) << "only the ISO was abandoned; Program must accept the next take";
  for (int tick = 0; tick < 10; ++tick) {
    frame.frameNumber = 100 + tick;
    frame.timelineTimestamp100ns = steady100ns();
    encoder->submit(frame);
    submitProgramTick(*encoder, frame.timelineTimestamp100ns);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  encoder->stopRecording();
  EXPECT_LT(elapsedMs(nextBegin), 5000);
  EXPECT_TRUE(encoder->session().isoStreams.empty());

  // The stalled write returns; the abandoned worker finalizes late and exits.
  std::this_thread::sleep_for(std::chrono::milliseconds(8000));
  encoder.reset();  // reaps the exited writer; must not crash or hang
  fs::remove_all(targetDir, ec);
}

// Part 3, Program side: a Program writer that misses the join bound is
// abandoned too. Its state still belongs to that thread, so the next Start is
// refused with a named error (never a hang, never a reopen under the live
// thread); once the late write returns and Program finalizes, recording works
// again without a core restart.
TEST(EncoderRecordingSession, MediaFoundationAbandonedProgramWriterRefusesThenRecovers) {
  struct ScopedEnv {
    const char* name;
    ScopedEnv(const char* n, const char* v) : name(n) { ::SetEnvironmentVariableA(n, v); _putenv_s(n, v); }
    ~ScopedEnv() { ::SetEnvironmentVariableA(name, nullptr); _putenv_s(name, ""); }
  };
  // Above Program's normal finalize (encoder drain up to 2 s), below the stall.
  const ScopedEnv bound("COREVIDEO_RECORDING_TRACK_JOIN_BOUND_MS", "3000");
  const ScopedEnv source("COREVIDEO_QA_RECORDING_STALL_SOURCE", "program");
  const ScopedEnv stall("COREVIDEO_QA_RECORDING_STALL_MS", "6000");
  const ScopedEnv after("COREVIDEO_QA_RECORDING_STALL_AFTER_FRAMES", "30");
  const corevideo::testing::ForcedEncoderCapacity capacity(8);
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  if (!encoder) return;  // Media Foundation unavailable.
  encoder->enableIndependentIsoWriters();
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto targetDir = fs::temp_directory_path() /
      ("corevideo-program-abandon-" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(targetDir, ec);
  auto request = departureRequest(targetDir, "program-abandon");
  request.isoSources.clear();
  encoder->configureRecording(request);
  encoder->start({"recording"}, {});
  corevideo::modules::ProgramFrame frame;
  frame.width = frame.preview.width = 640; frame.height = frame.preview.height = 360;
  frame.preview.bgra.assign(static_cast<size_t>(640) * 360 * 4, 0x40);
  for (int tick = 1; tick <= 40; ++tick) {
    frame.frameNumber = tick;
    frame.timelineTimestamp100ns = steady100ns();
    encoder->submit(frame);
    submitProgramTick(*encoder, frame.timelineTimestamp100ns);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  // Program's worker is now inside its 2 s stall (it began at frame 30).
  const auto stopBegin = std::chrono::steady_clock::now();
  encoder->stopRecording();
  EXPECT_LT(elapsedMs(stopBegin), 4000) << "Stop must return at the join bound";
  EXPECT_NE(encoder->session().recordingError.find("abandoned"), std::string::npos)
      << encoder->session().recordingError;

  bool refused = false;
  try { encoder->start({"recording"}, {}); }
  catch (const std::exception& ex) {
    refused = std::string(ex.what()).find("restart the media core") != std::string::npos;
  }
  EXPECT_TRUE(refused) << "a new take must be refused while the abandoned Program writer still runs";

  // The stall ends; Program finalizes late and the sink recovers on its own.
  std::this_thread::sleep_for(std::chrono::milliseconds(7000));
  // The late finalize settles the take: "stopped", and it says what happened.
  const auto settled = encoder->session();
  EXPECT_EQ(settled.recordingStatus, "stopped");
  EXPECT_NE(settled.recordingWarning.find("finalized late"), std::string::npos) << settled.recordingWarning;
  bool recovered = true;
  try { encoder->start({"recording"}, {}); } catch (...) { recovered = false; }
  ASSERT_TRUE(recovered) << "recording must recover once the abandoned writer has exited";
  for (int tick = 0; tick < 10; ++tick) {
    frame.frameNumber = 200 + tick;
    frame.timelineTimestamp100ns = steady100ns();
    encoder->submit(frame);
    submitProgramTick(*encoder, frame.timelineTimestamp100ns);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  encoder->stopRecording();
  const auto recoveredError = encoder->session().recordingError;
  EXPECT_EQ(recoveredError.find("abandoned"), std::string::npos) << recoveredError;
  // A direct (unwrapped) sink must not be destroyed under a still-running
  // abandoned Program thread; AsyncEncoderSink leaks it in that case.
  if (encoder->mustOutliveTeardown()) { (void)encoder.release(); return; }
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
  void stopRecording() override { ++recordingStopCount; }
  corevideo::modules::OutputSession session() const override { return session_; }

  int startCount = 0;
  int recordingStartCount = 0;
  int recordingStopCount = 0;

 private:
  corevideo::modules::OutputSession session_;
};
}  // namespace

TEST(RecordingProgramContinuity, CountsScheduledGapsWithoutCountingDuplicateOrOlderFrames) {
  corevideo::modules::RecordingProgramContinuity continuity;
  continuity.observe(100, 10);
  continuity.observe(100, 11);
  continuity.observe(100, 15);
  continuity.observe(100, 15);
  continuity.observe(100, 12);
  continuity.observe(100, 16);
  EXPECT_TRUE(continuity.observed());
  EXPECT_EQ(continuity.missingFrames(), 3);
}

TEST(RecordingProgramContinuity, NeverInfersLossAcrossUnknownTimingOrNewAnchors) {
  corevideo::modules::RecordingProgramContinuity continuity;
  continuity.observe(0, -1);
  EXPECT_FALSE(continuity.observed());
  continuity.observe(100, 10);
  continuity.observe(100, 12);
  continuity.observe(200, 500);
  continuity.observe(0, -1);
  continuity.observe(200, 900);
  EXPECT_EQ(continuity.missingFrames(), 1);
}

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

// #1009, STUB CORE ONLY. A recording started with Zoom offline survives zoom-join, zoom-leave
// and a rejoin sent through the real JsonRpcServer handlers to MediaCore: no stop reaches the
// writer, no second writer generation opens, and the session keeps its id, with or without the
// shell's production sync (which carries the Record intent) between the steps. The modules are
// the stub set, so MediaCore takes its stub-meeting branch: ZoomEngineRuntime::leave/join (the
// real-engine path) are NOT exercised here; that they reset only Zoom state was checked by
// reading them.
TEST(EncoderRecordingSession, StubCoreZoomJoinLeaveRejoinDoesNotStopAnOfflineStartedRecording) {
  auto modules = corevideo::modules::createStubModules();
  auto countingSink = std::make_unique<StartCountingEncoderSink>();
  auto* encoder = countingSink.get();
  modules.encoder = std::move(countingSink);
  corevideo::core::MediaCore mediaCore(std::move(modules));
  corevideo::rpc::JsonRpcServer server(mediaCore);

  const corevideo::rpc::Json startProgram = corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"recording"}},
  };
  const corevideo::rpc::Json setTargets = corevideo::rpc::Json::Object{
      {"type", "set-recording-targets"},
      {"targetFolder", "Recordings/CoreVideo Pro/tests"},
      {"filenamePrefix", "offline-show"},
      {"format", "mp4"},
  };
  const corevideo::rpc::Json startRecording = corevideo::rpc::Json::Object{
      {"type", "start-recording-session"},
      {"sessionId", "offline-show-program"},
  };

  // Zoom offline: the core has never joined a meeting.
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{setTargets, startRecording, startProgram});
  ASSERT_EQ(encoder->recordingStartCount, 1);
  ASSERT_EQ(encoder->session().recordingStatus, "recording");

  const auto zoom = [&server](const std::string& id, const std::string& type) {
    corevideo::rpc::Json::Object request{{"id", id}, {"type", type}};
    if (type == "zoom-join") {
      request.emplace("payload", corevideo::rpc::Json::Object{
                                     {"meetingUrl", "https://zoom.us/j/123456789"},
                                     {"displayName", "Operator"},
                                     {"webinar", false},
                                 });
    }
    return server.handle(corevideo::rpc::Json(request));
  };

  const std::vector<std::pair<std::string, std::string>> steps{
      {"join-1", "zoom-join"}, {"leave-1", "zoom-leave"}, {"join-2", "zoom-join"}, {"leave-2", "zoom-leave"}};
  for (const auto& [id, type] : steps) {
    const auto response = zoom(id, type);
    ASSERT_TRUE(response.get("ok") && response.get("ok")->asBool()) << id;
    EXPECT_EQ(response.get("snapshot")->getString("meetingState"),
              type == "zoom-join" ? "in_meeting" : "idle") << id;
    EXPECT_EQ(encoder->recordingStopCount, 0) << id << " must not stop the recording";
    EXPECT_EQ(encoder->session().recordingStatus, "recording") << id;

    (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{setTargets, startRecording, startProgram});
    EXPECT_EQ(encoder->recordingStopCount, 0) << id;
  }

  EXPECT_EQ(encoder->recordingStartCount, 1) << "leave/rejoin must not open a new writer generation";
  EXPECT_EQ(encoder->session().recordingSessionId, "offline-show-program");
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

namespace {
// #935: a sink whose session carries the structured admission + an ISO index.
class AdmissionEncoderSink final : public corevideo::modules::IEncoderSink {
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
  corevideo::modules::OutputSession session_;
};
}  // namespace

TEST(EncoderRecordingSession, IsoAdmissionAndIsoIndexSurfaceInSnapshotRecordingSection) {
  auto modules = corevideo::modules::createStubModules();
  auto owned = std::make_unique<AdmissionEncoderSink>();
  auto* sink = owned.get();
  modules.encoder = std::move(owned);
  corevideo::core::MediaCore mediaCore(std::move(modules));
  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-recording-session"}, {"sessionId", "adm-1"},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}}});

  const std::string note =
      "1 ISO source will record on the CPU software encoder, not the GPU (7 on hardware): x.";
  corevideo::modules::IsoAdmissionStatus adm;
  adm.decision = "admit-with-warning";
  adm.code = "iso-encoder-software-spill";
  adm.message = note;
  adm.hardwareCount = 7;
  adm.softwareCount = 1;
  adm.admissibleIsoCount = -1;
  adm.probed = true;
  adm.probeStatus = "ready";
  adm.capacity = "hw=yes sessions<=8";
  adm.workload = "h264 1920x1080@60";
  sink->session_.isoAdmission = adm;
  sink->session_.recordingWarning = note;
  corevideo::modules::IsoStreamStatus iso;
  iso.sourceId = "zoom:8";
  iso.displayName = "Guest 8";
  iso.path = "ISO-08-Guest-8.mp4";
  iso.encoderPath = "software";
  iso.fallbackReason = "hardware-capacity-exhausted";
  iso.isoIndex = 8;
  sink->session_.isoStreams.push_back(iso);

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{}, 16.0);
  const auto* recording = state.get("recording");
  ASSERT_NE(recording, nullptr);
  const auto* node = recording->get("isoAdmission");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->getString("decision"), "admit-with-warning");
  EXPECT_EQ(node->getString("code"), "iso-encoder-software-spill");
  EXPECT_EQ(node->getString("message"), note);
  EXPECT_EQ(node->getNumber("hardwareCount"), 7);
  EXPECT_EQ(node->getNumber("softwareCount"), 1);
  EXPECT_EQ(node->getNumber("unplacedCount"), 0);
  ASSERT_NE(node->get("admissibleIsoCount"), nullptr);
  EXPECT_TRUE(node->get("admissibleIsoCount")->isNull()) << "unknown must be null, never -1 or 0";
  ASSERT_NE(node->get("probed"), nullptr);
  EXPECT_TRUE(node->get("probed")->asBool(false));
  EXPECT_EQ(node->getString("probeStatus"), "ready");
  EXPECT_EQ(node->getString("capacity"), "hw=yes sessions<=8");
  EXPECT_EQ(node->getString("workload"), "h264 1920x1080@60");
  EXPECT_EQ(recording->getString("warning"), note);        // text unchanged
  EXPECT_EQ(recording->getString("status"), "recording");  // a notice, not a failure

  const auto* streams = recording->get("streams");
  ASSERT_NE(streams, nullptr);
  bool foundIso = false;
  for (const auto& stream : streams->asArray()) {
    if (stream.getString("kind") != "iso") {
      EXPECT_EQ(stream.get("isoIndex"), nullptr) << "only ISO streams carry an index";
      continue;
    }
    foundIso = true;
    EXPECT_EQ(stream.getNumber("isoIndex"), 8);
    EXPECT_EQ(stream.getString("encoderPath"), "software");
  }
  EXPECT_TRUE(foundIso);

  // Program-only: the node is absent, never an empty object.
  sink->session_.isoAdmission.reset();
  sink->session_.isoStreams.clear();
  const auto programOnly = mediaCore.applyCommands(corevideo::rpc::Json::Array{}, 16.0);
  ASSERT_NE(programOnly.get("recording"), nullptr);
  EXPECT_EQ(programOnly.get("recording")->get("isoAdmission"), nullptr);
}

TEST(AsyncEncoderSink, CarriesIsoAdmissionAndIsoIndexFromTheWrappedSink) {
  // #935 review focus: the product wraps the MF sink in AsyncEncoderSink, so the
  // node must survive its snapshot copy, not only the direct-sink path.
  auto inner = std::make_unique<AdmissionEncoderSink>();
  auto* sink = inner.get();
  corevideo::modules::IsoAdmissionStatus adm;
  adm.decision = "admit-with-warning";
  adm.code = "iso-encoder-software-spill";
  adm.softwareCount = 1;
  sink->session_.isoAdmission = adm;
  corevideo::modules::IsoStreamStatus iso;
  iso.sourceId = "zoom:8";
  iso.isoIndex = 8;
  iso.encoderPath = "software";
  sink->session_.isoStreams.push_back(iso);
  auto wrapped = std::make_unique<corevideo::modules::AsyncEncoderSink>(std::move(inner));
  corevideo::modules::RecordingSessionRequest request;
  request.sessionId = "async-935";
  wrapped->configureRecording(request);
  (void)wrapped->start({"recording"}, {});
  corevideo::modules::OutputSession session;
  for (int attempt = 0; attempt < 200; ++attempt) {
    session = wrapped->session();
    if (session.isoAdmission) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(session.isoAdmission.has_value());
  EXPECT_EQ(session.isoAdmission->decision, "admit-with-warning");
  ASSERT_EQ(session.isoStreams.size(), 1u);
  EXPECT_EQ(session.isoStreams[0].isoIndex, 8);
  wrapped->stopRecording();
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

// #466 / T2.9. Stop Record while a STREAM stays up. The stop sync carries
// stop-recording-session followed by start-program-output{destinations:[rtmp]}
// with no "recording" - a repeated desired-state assertion the shell sends on
// every sync. MediaCore treated recordingStatus_ "stopping" as "the recording
// no longer owns the encoder generation", so it called encoder->start(). That
// bumps the sink generation and AsyncEncoderSink resets the snapshot with no
// recording lifecycle, so the old generation's Stop barrier finalized the file
// and had nowhere to publish `completed`: recording.status sat at "stopping"
// for the rest of the show.
//
// The file was always fine - the barrier is FIFO ahead of the new Start. What
// broke is the truthful-lifecycle contract (CLAUDE.md, "Destination lifecycle
// is TRUTHFUL"): neither the operator nor a support bundle could say whether
// the recording finalized.
//
// The restart itself is the defect, so the restart is what this asserts. A
// status assertion cannot see it: the stub encoder publishes no lifecycle, so
// the visible symptom only appears with the real AsyncEncoderSink. Counting
// start() pins the cause at the level the fix lives on, and has the second
// benefit the fix buys - the live stream no longer eats a reconnect and a
// keyframe every time the operator stops Record.
namespace {
class StartCountingEncoder final : public corevideo::modules::IEncoderSink {
 public:
  StartCountingEncoder(std::unique_ptr<corevideo::modules::IEncoderSink> inner, int* starts)
      : inner_(std::move(inner)), starts_(starts) {}
  void configureRecording(const corevideo::modules::RecordingSessionRequest& request) override {
    inner_->configureRecording(request);
  }
  corevideo::modules::OutputSession start(const std::vector<std::string>& destinations,
                                          const std::vector<std::string>& isoParticipantIds) override {
    ++*starts_;
    return inner_->start(destinations, isoParticipantIds);
  }
  void submit(const corevideo::modules::ProgramFrame& frame) override { inner_->submit(frame); }
  corevideo::modules::OutputSession session() const override { return inner_->session(); }

 private:
  std::unique_ptr<corevideo::modules::IEncoderSink> inner_;
  int* starts_;
};
}  // namespace

TEST(EncoderRecordingSession, StoppingRecordWhileAStreamStaysUpDoesNotRestartTheEncoder) {
  int starts = 0;
  auto modules = corevideo::modules::createStubModules();
  modules.encoder = std::make_unique<StartCountingEncoder>(std::move(modules.encoder), &starts);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "set-recording-targets"},
          {"targetFolder", "Recordings/CoreVideo Pro/tests"},
          {"filenamePrefix", "stop-with-stream"},
          {"format", "mp4"},
          {"quality", "medium"},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-program-output"},
          {"destinations", corevideo::rpc::Json::Array{"recording", "rtmp://127.0.0.1/live/key"}},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-recording-session"},
          {"sessionId", "session-stop-with-stream"},
          {"startedAtMs", 100},
      },
  });
  const int startsBeforeStop = starts;

  // The operator stops Record only. The stream is still desired, so the very
  // same sync re-asserts the remaining destination - and every later sync
  // re-asserts it again while the writer is still finalizing.
  mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "stop-recording-session"},
          {"reason", "Operator stopped recording."},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-program-output"},
          {"destinations", corevideo::rpc::Json::Array{"rtmp://127.0.0.1/live/key"}},
      },
  });
  mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "start-program-output"},
          {"destinations", corevideo::rpc::Json::Array{"rtmp://127.0.0.1/live/key"}},
      },
  });

  EXPECT_EQ(starts, startsBeforeStop)
      << "the stream's desired-state re-assertion restarted the encoder while the "
         "recording was still finalizing, which is what erases its lifecycle";
}

TEST(EncoderRecordingSession, RecordingDepthReachesConsumerAndRefusalRetainsPreviousSetting) {
  auto modules = corevideo::modules::createStubModules();
  auto encoder = std::make_unique<RequestCapturingSink>();
  auto* captured = encoder.get();
  modules.encoder = std::move(encoder);
  corevideo::core::MediaCore core(std::move(modules));
  core.applyCommands(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
      {"type", "set-recording-targets"}, {"writeQueueDepth", 14}}});
  EXPECT_EQ(captured->lastRequest.writeQueueDepth, 14);
  core.applyCommands(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
      {"type", "set-recording-targets"}, {"writeQueueDepth", 31}}});
  EXPECT_EQ(captured->lastRequest.writeQueueDepth, 14);
  const auto snapshot = core.sessionState();
  const auto* recording = snapshot.get("recording");
  ASSERT_NE(recording, nullptr);
  EXPECT_NE(recording->getString("queueSettingWarning").find("refused"), std::string::npos);
}
