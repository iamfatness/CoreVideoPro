#include "core/MediaCore.h"
#include "modules/Interfaces.h"

#include <gtest/gtest.h>

#include <vector>

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
#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>

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
