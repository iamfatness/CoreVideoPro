#include "compositor/CompositorLayout.h"
#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include "modules/ZoomMeetingSdkAdapter.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

bool jsonArrayContains(const corevideo::rpc::Json& array, const std::string& value) {
  return std::any_of(array.asArray().begin(), array.asArray().end(), [&](const corevideo::rpc::Json& entry) {
    return entry.isString() && entry.asString() == value;
  });
}

}  // namespace

TEST(MediaCoreCommand, AppliesSceneGraphTransformsOverlaysAndOutput) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "load-scene-graph"},
          {"sceneId", "interview"},
          {"routes", corevideo::rpc::Json::Array{
                         corevideo::rpc::Json::Object{{"routeId", "a"}, {"mode", "fixed"}, {"audioRole", "mix"}},
                         corevideo::rpc::Json::Object{{"routeId", "b"}, {"mode", "active-speaker"}, {"audioRole", "mix"}},
                     }},
      },
      corevideo::rpc::Json::Object{
          {"type", "set-participant-transform"},
          {"participantId", "participant-1"},
          {"crop", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}},
          {"scale", 1},
      },
      corevideo::rpc::Json::Object{
          {"type", "set-overlay-asset"},
          {"overlayId", "lower-third"},
          {"text", "Speaker"},
          {"position", "lower-third"},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-program-output"},
          {"destinations", corevideo::rpc::Json::Array{"recording"}},
          {"isoParticipantIds", corevideo::rpc::Json::Array{"participant-1"}},
      },
  });

  EXPECT_EQ(state.getString("sceneId"), "interview");
  EXPECT_EQ(state.get("routeCount")->asNumber(), 2);
  EXPECT_EQ(state.get("transformCount")->asNumber(), 1);
  EXPECT_EQ(state.get("overlayCount")->asNumber(), 1);
  EXPECT_TRUE(state.get("active")->asBool());
  EXPECT_EQ(state.get("health")->getString("status"), "live");
}

TEST(MediaCoreCommand, ProfileMirrorsNativeMediaCoreShape) {
  corevideo::core::MediaCore mediaCore;
  const auto profile = mediaCore.profile();

#if COREVIDEO_WITH_D3D11
  EXPECT_EQ(profile.getString("name"), "CoreVideo Pro Native Media Core");
  EXPECT_EQ(profile.getString("renderer"), "d3d11");
#else
  EXPECT_EQ(profile.getString("name"), "CoreVideo Pro Native Media Core Stub");
  EXPECT_EQ(profile.getString("renderer"), "software");
#endif
  EXPECT_EQ(profile.getString("maxProgramResolution"), "1920x1080");
  EXPECT_EQ(profile.get("maxProgramFps")->asNumber(), 30);
  EXPECT_GE(profile.get("maxParticipantFeeds")->asNumber(), 6);
  EXPECT_GE(profile.get("maxIsoRecordings")->asNumber(), 2);
  ASSERT_NE(profile.get("capabilities"), nullptr);
  const auto& capabilities = *profile.get("capabilities");
  EXPECT_TRUE(jsonArrayContains(capabilities, "audio-mixer"));
  EXPECT_TRUE(jsonArrayContains(capabilities, "scene-graph-rendering"));
  EXPECT_TRUE(jsonArrayContains(capabilities, "dynamic-overlays"));
#if COREVIDEO_WITH_D3D11
  EXPECT_TRUE(jsonArrayContains(capabilities, "gpu-compositor"));
  EXPECT_TRUE(jsonArrayContains(capabilities, "chroma-key"));
  EXPECT_TRUE(jsonArrayContains(capabilities, "smart-framing"));
#else
  EXPECT_FALSE(jsonArrayContains(capabilities, "gpu-compositor"));
#endif
#if COREVIDEO_WITH_ZOOM
  EXPECT_TRUE(jsonArrayContains(capabilities, "zoom-raw-video"));
  EXPECT_TRUE(jsonArrayContains(capabilities, "zoom-raw-audio"));
#else
  EXPECT_FALSE(jsonArrayContains(capabilities, "zoom-raw-video"));
#endif
#if COREVIDEO_WITH_MF_ENCODER
  EXPECT_TRUE(jsonArrayContains(capabilities, "program-recording"));
  EXPECT_TRUE(jsonArrayContains(capabilities, "iso-recording"));
#else
  EXPECT_FALSE(jsonArrayContains(capabilities, "program-recording"));
#endif
#if COREVIDEO_WITH_RTMP_OUTPUT
  EXPECT_TRUE(jsonArrayContains(capabilities, "rtmp-output"));
#else
  EXPECT_FALSE(jsonArrayContains(capabilities, "rtmp-output"));
#endif
}

TEST(MediaCoreCommand, DefaultFactoryReportsActiveRendererInHealth) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"recording"}},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}},
  });
  EXPECT_TRUE(state.get("active")->asBool());
  const auto health = mediaCore.health();
#if COREVIDEO_WITH_D3D11
  EXPECT_EQ(health.getString("renderer"), "d3d11");
#else
  EXPECT_EQ(health.getString("renderer"), "software");
#endif
}

TEST(MediaCoreCommand, ReportsEncoderMetadataInHealthAndSession) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"recording"}},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}},
  });

  EXPECT_TRUE(state.get("active")->asBool());
  EXPECT_FALSE(state.getString("encoder").empty());
  EXPECT_EQ(state.getString("codec"), "h264");
  const auto health = mediaCore.health();
#if COREVIDEO_WITH_MF_ENCODER
  EXPECT_EQ(health.getString("encoder"), "media-foundation");
  EXPECT_TRUE(health.get("hardwareEncoder")->asBool());
#else
  EXPECT_EQ(health.getString("encoder"), "software-counting");
  EXPECT_FALSE(health.get("hardwareEncoder")->asBool());
#endif
}

TEST(MediaCoreCommand, AppliesEncoderLifecycleAndRecordingCommands) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{
          {"type", "prepare-encoder-session"},
          {"preparedAtMs", 1000},
          {"reason", "GPU encoder warmup."},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-program-output"},
          {"destinations", corevideo::rpc::Json::Array{"recording", "rtmp"}},
          {"isoParticipantIds", corevideo::rpc::Json::Array{"participant-1"}},
      },
      corevideo::rpc::Json::Object{
          {"type", "set-recording-targets"},
          {"targetFolder", "Recordings/CoreVideo Pro/native-core"},
          {"filenamePrefix", "program"},
          {"format", "mp4"},
          {"quality", "high"},
          {"isoParticipantIds", corevideo::rpc::Json::Array{"participant-1"}},
      },
      corevideo::rpc::Json::Object{
          {"type", "start-recording-session"},
          {"sessionId", "show-1"},
          {"startedAtMs", 1010},
      },
  });

  const auto* encoderSession = state.get("encoderSession");
  ASSERT_NE(encoderSession, nullptr);
  EXPECT_EQ(encoderSession->getString("status"), "encoding");
  EXPECT_EQ(encoderSession->get("lifecycle")->getString("status"), "encoding");
  EXPECT_GE(encoderSession->get("programFrameCount")->asNumber(), 1);
  EXPECT_GE(encoderSession->get("targets")->asArray().size(), 2);

  const auto* recording = state.get("recording");
  ASSERT_NE(recording, nullptr);
  EXPECT_EQ(recording->getString("sessionId"), "show-1");
  EXPECT_EQ(recording->getString("status"), "recording");
  EXPECT_EQ(recording->getString("writerStatus"), "writing");
  EXPECT_EQ(recording->get("encoder")->getString("codec"), "h264");
  EXPECT_GE(recording->get("totalFramesWritten")->asNumber(), 1);
  EXPECT_GE(recording->get("streams")->asArray().size(), 2);
}

TEST(MediaCoreCommand, AppliesRecordingFailureAndRecoveryCommands) {
  corevideo::core::MediaCore mediaCore;
  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-recording-session"},
      {"sessionId", "show-1"},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}},
  });

  auto failed = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "fail-recording-session"},
      {"message", "Encoder process exited."},
  });
  ASSERT_NE(failed.get("recording"), nullptr);
  EXPECT_EQ(failed.get("recording")->getString("status"), "failed");
  EXPECT_EQ(failed.get("recording")->getString("writerStatus"), "failed");
  EXPECT_EQ(failed.get("recording")->getString("error"), "Encoder process exited.");

  auto recovered = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "recover-recording-session"},
      {"reason", "Recording writer recovered."},
  });
  ASSERT_NE(recovered.get("recording"), nullptr);
  EXPECT_EQ(recovered.get("recording")->getString("status"), "recording");
  EXPECT_EQ(recovered.get("recording")->getString("writerStatus"), "writing");
  EXPECT_EQ(recovered.get("recording")->getString("warning"), "Recording writer recovered.");
}

TEST(MediaCoreCommand, SyncsNetworkOutputSenderSession) {
  corevideo::core::MediaCore mediaCore;
  const auto state = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"recording", "rtmp"}},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}},
  });

  const auto* senderSession = state.get("outputSenderSession");
  ASSERT_NE(senderSession, nullptr);
#if COREVIDEO_WITH_RTMP_OUTPUT
  EXPECT_TRUE(senderSession->getString("status") == "live" || senderSession->getString("status") == "warning");
#else
  EXPECT_EQ(senderSession->getString("status"), "live");
#endif
  EXPECT_EQ(senderSession->get("activeSenderCount")->asNumber(), 1);
  ASSERT_TRUE(senderSession->get("senders")->asArray().size() == 1);
  const auto& sender = senderSession->get("senders")->asArray()[0];
  EXPECT_EQ(sender.getString("destination"), "rtmp");
#if COREVIDEO_WITH_RTMP_OUTPUT
  EXPECT_TRUE(sender.getString("status") == "live" || sender.getString("status") == "warning");
#else
  EXPECT_EQ(sender.getString("status"), "live");
  EXPECT_GE(sender.get("framesSent")->asNumber(), 1);
#endif
}

TEST(MediaCoreCommand, AppliesOutputSenderFailureAndRecoveryCommands) {
  corevideo::core::MediaCore mediaCore;
  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"rtmp"}},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}},
  });

  const auto failed = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "fail-output-sender"},
      {"destination", "rtmp"},
      {"message", "RTMP ingest rejected credentials."},
      {"failedAtMs", 2000},
  });
  const auto* failedSession = failed.get("outputSenderSession");
  ASSERT_NE(failedSession, nullptr);
  EXPECT_EQ(failedSession->getString("status"), "failed");
  ASSERT_TRUE(failedSession->get("senders")->asArray().size() == 1);
  EXPECT_EQ(failedSession->get("senders")->asArray()[0].getString("status"), "failed");
  EXPECT_EQ(failedSession->get("senders")->asArray()[0].getString("warning"), "RTMP ingest rejected credentials.");

  const auto recovered = mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "recover-output-sender"},
      {"destination", "rtmp"},
      {"reason", "RTMP sender recovered."},
      {"recoveredAtMs", 2500},
  });
  const auto* recoveredSession = recovered.get("outputSenderSession");
  ASSERT_NE(recoveredSession, nullptr);
  EXPECT_EQ(recoveredSession->getString("status"), "warning");
  ASSERT_TRUE(recoveredSession->get("senders")->asArray().size() == 1);
#if COREVIDEO_WITH_RTMP_OUTPUT
  EXPECT_TRUE(recoveredSession->get("senders")->asArray()[0].getString("status") == "starting" ||
              recoveredSession->get("senders")->asArray()[0].getString("status") == "warning");
#else
  EXPECT_EQ(recoveredSession->get("senders")->asArray()[0].getString("status"), "starting");
#endif
  EXPECT_EQ(recoveredSession->get("senders")->asArray()[0].getString("warning"), "RTMP sender recovered.");
}

TEST(MediaCoreCommand, ReportsCaptureDevicesAndAppliesCaptureControls) {
  corevideo::core::MediaCore mediaCore;
  const auto devices = mediaCore.captureDevices();
  ASSERT_TRUE(devices.asArray().size() >= 2);
  EXPECT_EQ(devices.asArray()[0].getString("vendor"), "blackmagic");
  EXPECT_EQ(devices.asArray()[0].get("inputs")->asArray().size(), 2);
  const auto deckLinkId = devices.asArray()[0].getString("id");
  const auto ajaDevice = std::find_if(devices.asArray().begin(), devices.asArray().end(), [](const corevideo::rpc::Json& device) {
    return device.getString("vendor") == "aja";
  });
  ASSERT_TRUE(ajaDevice != devices.asArray().end());
  const auto ajaId = ajaDevice->getString("id");

  const auto selected = mediaCore.selectCaptureInput(deckLinkId, "hdmi-1");
  ASSERT_TRUE(selected.asArray().size() >= 1);
  EXPECT_EQ(selected.asArray()[0].getString("selectedInputId"), "hdmi-1");

  const auto offset = mediaCore.setCaptureAudioSyncOffset(ajaId, 1200);
  const auto aja = std::find_if(offset.asArray().begin(), offset.asArray().end(), [&](const corevideo::rpc::Json& device) {
    return device.getString("id") == ajaId;
  });
  ASSERT_TRUE(aja != offset.asArray().end());
  EXPECT_EQ(aja->get("audioSyncOffsetMs")->asNumber(), 500);

  const auto connected = mediaCore.connectCaptureDevice(ajaId);
  const auto connectedAja = std::find_if(connected.asArray().begin(), connected.asArray().end(), [&](const corevideo::rpc::Json& device) {
    return device.getString("id") == ajaId;
  });
  ASSERT_TRUE(connectedAja != connected.asArray().end());
  EXPECT_EQ(connectedAja->getString("connectionState"), "connected");
  EXPECT_TRUE(connectedAja->get("signalPresent")->asBool());
}

TEST(GpuCompositorAdapter, FactoryIsDisabledUnlessD3D11GateIsEnabled) {
#if COREVIDEO_WITH_D3D11
  auto compositor = corevideo::modules::createD3D11Compositor();
  ASSERT_NE(compositor, nullptr);
  corevideo::modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = "test-plan";
  renderPlan.sceneId = "interview";
  renderPlan.layers.push_back({"speaker", "participant-video", "zoom:123", "123", 0});

  const auto layout = corevideo::compositor::gridCell(1, 0);
  renderPlan.layers[0].rect = {layout.x, layout.y, layout.width, layout.height};

  const auto frame = compositor->render(renderPlan, {{"123", 1280, 720, 16}});
  EXPECT_EQ(frame.renderer, "d3d11");
  EXPECT_EQ(frame.renderPlanId, "test-plan");
  EXPECT_EQ(frame.layerCount, 1);
  EXPECT_TRUE(frame.gpuComposed);
  EXPECT_NE(frame.programPixelSignature, 0u);
#else
  EXPECT_EQ(corevideo::modules::createD3D11Compositor(), nullptr);
#endif
}

TEST(HardwareEncoderAdapter, FactoryIsDisabledUnlessMediaFoundationGateIsEnabled) {
#if COREVIDEO_WITH_MF_ENCODER
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  ASSERT_NE(encoder, nullptr);
  const auto session = encoder->start({"recording"}, {"participant-1"});
  EXPECT_EQ(session.encoderName, "media-foundation");
  EXPECT_EQ(session.codec, "h264");
  EXPECT_TRUE(session.hardwareAccelerated);
  encoder->submit({1920, 1080, 1, 1, "test-plan", "d3d11"});
  EXPECT_EQ(encoder->session().encodedFrameCount, 1);
#else
  EXPECT_EQ(corevideo::modules::createMediaFoundationEncoderSink(), nullptr);
#endif
}

TEST(HardwareEncoderAdapter, MediaFoundationWritesMp4ArtifactWhenRecordingIsArmed) {
#if COREVIDEO_WITH_MF_ENCODER
  auto encoder = corevideo::modules::createMediaFoundationEncoderSink();
  ASSERT_NE(encoder, nullptr);
  const auto started = encoder->start({"recording"}, {"participant-1"});
  ASSERT_FALSE(started.recordingArtifactPath.empty());
  EXPECT_EQ(std::filesystem::path(started.recordingArtifactPath).extension().string(), ".mp4");

  encoder->submit({1920, 1080, 2, 42, "mp4-plan", "d3d11"});
  encoder->submit({1920, 1080, 2, 43, "mp4-plan", "d3d11"});
  encoder->submit({1920, 1080, 2, 44, "mp4-plan", "d3d11"});
  const auto session = encoder->session();
  EXPECT_TRUE(session.recordingBytesWritten > 0);
  EXPECT_TRUE(session.recordingWarning.empty()) << session.recordingWarning;
  const auto artifactPath = session.recordingArtifactPath;
  encoder.reset();

  ASSERT_TRUE(std::filesystem::exists(artifactPath));
  EXPECT_TRUE(std::filesystem::file_size(artifactPath) > 1024u);

  std::ifstream input(artifactPath, std::ios::binary);
  std::string header(32, '\0');
  input.read(header.data(), static_cast<std::streamsize>(header.size()));
  header.resize(static_cast<size_t>(input.gcount()));
  EXPECT_NE(header.find("ftyp"), std::string::npos);
  input.close();
  std::filesystem::remove(artifactPath);
#else
  EXPECT_TRUE(true);
#endif
}

TEST(OutputSenderAdapter, FactoryIsDisabledUnlessRtmpGateIsEnabled) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);
  const auto session = sender->sync({"rtmp"}, nullptr, 0);
  EXPECT_GE(session.activeSenderCount, 1);
  ASSERT_FALSE(session.senders.empty());
  EXPECT_EQ(session.senders[0].destination, "rtmp");
#else
  EXPECT_EQ(corevideo::modules::createRtmpOutputSender(), nullptr);
#endif
}

TEST(OutputSenderAdapter, RtmpWritesSendProofArtifactWhenArmed) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  auto sender = corevideo::modules::createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);

  corevideo::modules::ProgramFrame frame{1920, 1080, 2, 7, "rtmp-proof-plan", "d3d11"};
  const auto session = sender->sync({"rtmp"}, &frame, 33);
  ASSERT_FALSE(session.senders.empty());
  ASSERT_FALSE(session.senders[0].sendArtifactPath.empty());
  EXPECT_TRUE(session.senders[0].sendBytesWritten > 0);
  ASSERT_TRUE(std::filesystem::exists(session.senders[0].sendArtifactPath));

  std::ifstream input(session.senders[0].sendArtifactPath);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const auto content = buffer.str();
  EXPECT_NE(content.find("rtmp-send-proof-start"), std::string::npos);
  EXPECT_NE(content.find("rtmp-send-attempt"), std::string::npos);
  input.close();
  const auto artifactPath = session.senders[0].sendArtifactPath;
  sender.reset();
  std::filesystem::remove(artifactPath);
#else
  EXPECT_TRUE(true);
#endif
}

TEST(CaptureDeviceAdapter, FactoriesAreDisabledUnlessHardwareGatesAreEnabled) {
#if COREVIDEO_WITH_DECKLINK
  auto deckLink = corevideo::modules::createDeckLinkCaptureDevice();
  ASSERT_NE(deckLink, nullptr);
  const auto deckLinkDevices = deckLink->enumerate();
  ASSERT_FALSE(deckLinkDevices.empty());
  EXPECT_EQ(deckLinkDevices[0].vendor, "blackmagic");
#else
  EXPECT_EQ(corevideo::modules::createDeckLinkCaptureDevice(), nullptr);
#endif

#if COREVIDEO_WITH_AJA
  auto aja = corevideo::modules::createAjaCaptureDevice();
  ASSERT_NE(aja, nullptr);
  const auto ajaDevices = aja->enumerate();
  ASSERT_FALSE(ajaDevices.empty());
  EXPECT_EQ(ajaDevices[0].vendor, "aja");
#else
  EXPECT_EQ(corevideo::modules::createAjaCaptureDevice(), nullptr);
#endif
}

TEST(ZoomMeetingSdkAdapter, FactoryIsDisabledInPortableStubBuild) {
#if COREVIDEO_WITH_ZOOM
  EXPECT_TRUE(true);
#else
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({});
  EXPECT_EQ(source, nullptr);
#endif
}

TEST(ZoomMeetingSdkAdapter, DevGateRejectsMissingJoinCredentials) {
#if COREVIDEO_WITH_ZOOM
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({
      "sdk-root",
      "7.0.5",
      "https://zoom.us",
      true,
      true,
      true,
      true,
      true,
      true,
  });
  ASSERT_NE(source, nullptr);

  const bool joined = source->join({
      "123456789",
      "CoreVideo Producer",
  });

  EXPECT_FALSE(joined);
  EXPECT_EQ(source->meetingState(), "join-ready");
  EXPECT_FALSE(source->warnings().empty());
  source->leave();
  EXPECT_EQ(source->meetingState(), "idle");
#else
  EXPECT_TRUE(true);
#endif
}

TEST(ZoomMeetingSdkAdapter, DevGateTracksDeferredRawSubscriptions) {
#if COREVIDEO_WITH_ZOOM
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({
      "sdk-root",
      "7.0.5",
      "https://zoom.us",
      true,
      true,
      true,
      true,
      true,
      true,
  });
  ASSERT_NE(source, nullptr);

  source->syncSubscriptions({
      {"12345", "participant-video", "program", 1},
      {"12345", "participant-audio", "mix", 2},
  });

  const auto states = source->subscriptionStates();
  ASSERT_TRUE(states.size() == 2);
  EXPECT_EQ(states[0].status, "failed");
  EXPECT_EQ(states[0].lastResultCode, "not-in-meeting");
  EXPECT_EQ(states[1].status, "failed");
  EXPECT_EQ(states[1].lastResultCode, "not-in-meeting");
#else
  EXPECT_TRUE(true);
#endif
}

TEST(ZoomMeetingSdkAdapter, DevGateReturnsEmptyRosterBeforeSdkJoin) {
#if COREVIDEO_WITH_ZOOM
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({
      "sdk-root",
      "7.0.5",
      "https://zoom.us",
      true,
      true,
      true,
      true,
      true,
      true,
  });
  ASSERT_NE(source, nullptr);

  EXPECT_TRUE(source->participants().empty());
#else
  EXPECT_TRUE(true);
#endif
}

TEST(ZoomMeetingSdkAdapter, DevGateRejectsRecordingProofBeforeMeeting) {
#if COREVIDEO_WITH_ZOOM
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({
      "sdk-root",
      "7.0.5",
      "https://zoom.us",
      true,
      true,
      true,
      true,
      true,
      true,
  });
  ASSERT_NE(source, nullptr);

  EXPECT_FALSE(source->startRecordingProof());
  const auto proof = source->recordingProof();
  EXPECT_FALSE(proof.active);
  EXPECT_EQ(proof.status, "failed");
  EXPECT_EQ(proof.lastResultCode, "not-in-meeting");
  EXPECT_FALSE(proof.warning.empty());
#else
  EXPECT_TRUE(true);
#endif
}

TEST(ZoomMeetingSdkAdapter, DevGateDoesNotEmitFramesForDeferredRawSubscriptions) {
#if COREVIDEO_WITH_ZOOM
  auto source = corevideo::modules::createZoomMeetingSdkCaptureSource({
      "sdk-root",
      "7.0.5",
      "https://zoom.us",
      true,
      true,
      true,
      true,
      true,
      true,
  });
  ASSERT_NE(source, nullptr);

  source->syncSubscriptions({
      {"12345", "participant-video", "program", 1},
      {"12345", "participant-audio", "mix", 2},
      {"12345", "screen-share", "program", 3},
  });

  EXPECT_TRUE(source->pollVideoFrames().empty());
  EXPECT_TRUE(source->pollAudioFrames().empty());
#else
  EXPECT_TRUE(true);
#endif
}
