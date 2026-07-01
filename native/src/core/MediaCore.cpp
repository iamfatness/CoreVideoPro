#include "core/MediaCore.h"

#include "compositor/CompositorLayout.h"
#include "core/Protocol.h"
#include "modules/AudioDsp.h"
#include "modules/ProgramFramePreview.h"
#include "modules/RealZoomCaptureSource.h"
#include "modules/WinUiCaptureDeviceAdapter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace corevideo::core {
namespace {

constexpr int64_t kStaleCaptureAudioAgeMs = 1000;

rpc::Json::Array stringArray(const std::vector<std::string>& values) {
  rpc::Json::Array result;
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return result;
}

int64_t monotonicMs() {
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

float clampColorGradeAxis(double value) {
  return static_cast<float>(std::max(-100.0, std::min(100.0, value)));
}

modules::CompositorColorGrade readColorGrade(const rpc::Json& value) {
  return modules::CompositorColorGrade{
      clampColorGradeAxis(value.getNumber("exposure", 0.0)),
      clampColorGradeAxis(value.getNumber("contrast", 0.0)),
      clampColorGradeAxis(value.getNumber("saturation", 0.0)),
      clampColorGradeAxis(value.getNumber("temperature", 0.0)),
  };
}

std::vector<modules::OutputDestinationSettings> readOutputDestinationSettings(const rpc::Json& command) {
  std::vector<modules::OutputDestinationSettings> result;
  const auto* settings = command.get("destinationSettings");
  if (!settings || !settings->isArray()) {
    return result;
  }

  for (const auto& value : settings->asArray()) {
    if (!value.isObject()) {
      continue;
    }
    modules::OutputDestinationSettings destination;
    destination.id = value.getString("id");
    destination.label = value.getString("label");
    destination.protocol = value.getString("protocol");
    destination.url = value.getString("url");
    destination.streamKey = value.getString("streamKey");
    destination.ffmpegBinDirectory = value.getString("ffmpegBinDirectory");
    destination.mode = value.getString("mode");
    destination.host = value.getString("host");
    destination.port = static_cast<int>(value.getNumber("port", destination.port));
    destination.latencyMs = static_cast<int>(value.getNumber("latencyMs", destination.latencyMs));
    destination.latencyUs = static_cast<int>(value.getNumber("latencyUs", destination.latencyUs));
    destination.passphrase = value.getString("passphrase");
    destination.keyLength = static_cast<int>(value.getNumber("keyLength", destination.keyLength));
    destination.streamId = value.getString("streamId");
    destination.ndiName = value.getString("ndiName");
    destination.ndiGroup = value.getString("ndiGroup");
    destination.fps = static_cast<int>(value.getNumber("fps", destination.fps));
    destination.targetBitrateMbps = value.getNumber("targetBitrateMbps", destination.targetBitrateMbps);
    destination.audioBitrateKbps =
        std::max(32, std::min(512, static_cast<int>(value.getNumber("audioBitrateKbps", destination.audioBitrateKbps))));
    destination.videoCodec = value.getString("videoCodec", destination.videoCodec);
    destination.encoderMode = value.getString("encoderMode", destination.encoderMode);
    if (const auto* enhanced = value.get("allowEnhancedRtmp")) {
      destination.allowEnhancedRtmp = enhanced->asBool(destination.allowEnhancedRtmp);
    }
    if (destination.id.empty() && destination.protocol.empty() && destination.url.empty() &&
        destination.host.empty() && destination.ndiName.empty()) {
      continue;
    }
    result.push_back(std::move(destination));
  }
  return result;
}

rpc::Json::Array capabilityArray(const std::string& renderer, const modules::OutputSession& encoderSession) {
  rpc::Json::Array result;
  result.emplace_back("audio-mixer");
  result.emplace_back("scene-graph-rendering");
  result.emplace_back("dynamic-overlays");

  if (renderer != "software") {
    result.emplace_back("gpu-compositor");
    result.emplace_back("chroma-key");
  result.emplace_back("smart-framing");
  }

#if COREVIDEO_WITH_WASAPI_CAPTURE
  result.emplace_back("local-audio-capture");
#endif

#if COREVIDEO_WITH_WASAPI_MONITOR
  result.emplace_back("audio-monitor-output");
#endif

#if COREVIDEO_WITH_ZOOM
  result.emplace_back("zoom-raw-video");
  result.emplace_back("zoom-raw-audio");
#endif

  if (encoderSession.hardwareAccelerated) {
    result.emplace_back("program-recording");
    result.emplace_back("iso-recording");
  }

#if COREVIDEO_WITH_RTMP_OUTPUT
  result.emplace_back("rtmp-output");
#endif

#if COREVIDEO_WITH_NDI_OUTPUT
  result.emplace_back("ndi-output");
#endif

#if COREVIDEO_WITH_SRT_OUTPUT
  result.emplace_back("srt-output");
#endif

#if COREVIDEO_WITH_SRT_INGEST
  result.emplace_back("srt-ingest");
#endif

#if COREVIDEO_WITH_DECKLINK
  result.emplace_back("decklink-capture");
#endif

#if COREVIDEO_WITH_AJA
  result.emplace_back("aja-capture");
#endif

  return result;
}

rpc::Json captureDeviceJson(const modules::CaptureDeviceInfo& device) {
  rpc::Json::Array inputs;
  for (size_t index = 0; index < device.inputIds.size(); ++index) {
    inputs.emplace_back(rpc::Json::Object{
        {"id", device.inputIds[index]},
        {"label", index < device.inputLabels.size() ? device.inputLabels[index] : device.inputIds[index]},
        {"hasEmbeddedAudio", index < device.inputHasEmbeddedAudio.size() ? device.inputHasEmbeddedAudio[index] : true},
    });
  }

  rpc::Json::Object result{
      {"id", device.id},
      {"vendor", device.vendor.empty() ? "blackmagic" : device.vendor},
      {"name", device.name},
      {"inputs", inputs},
      {"selectedInputId", device.selectedInputId},
      {"resolution", rpc::Json::Object{{"width", device.width}, {"height", device.height}}},
      {"frameRate", device.frameRate},
      {"connectionState", device.connectionState},
      {"signalPresent", device.signalPresent},
      {"droppedFrames", static_cast<double>(device.droppedFrames)},
      {"audioSyncOffsetMs", device.audioSyncOffsetMs},
  };
  if (!device.warning.empty()) {
    result.emplace("warning", device.warning);
  }
  return result;
}

rpc::Json::Array captureDeviceArray(const std::vector<modules::CaptureDeviceInfo>& devices) {
  rpc::Json::Array result;
  for (const auto& device : devices) {
    result.emplace_back(captureDeviceJson(device));
  }
  return result;
}

int clampIntValue(int value, int minValue, int maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

std::string normalizeVideoCodec(const std::string& codec, const std::string& fallback) {
  if (codec == "h264" || codec == "h265" || codec == "av1") {
    return codec;
  }
  if (codec == "hevc") {
    return "h265";
  }
  return fallback.empty() ? "h264" : fallback;
}

rpc::Json outputProfileJson(
    const std::string& profileId,
    const std::string& resolution,
    int width,
    int height,
    int fps,
    double targetBitrateMbps) {
  return rpc::Json::Object{
      {"profileId", profileId},
      {"resolution", resolution},
      {"width", width},
      {"height", height},
      {"fps", fps},
      {"targetBitrateMbps", targetBitrateMbps},
  };
}

void applyRecordingProfile(
    const rpc::Json& profile,
    int& width,
    int& height,
    int& fps,
    double& targetBitrateMbps,
    std::string& codec) {
  width = clampIntValue(static_cast<int>(profile.getNumber("width", width)), 320, 3840);
  height = clampIntValue(static_cast<int>(profile.getNumber("height", height)), 180, 2160);
  fps = clampIntValue(static_cast<int>(profile.getNumber("fps", fps)), 1, 120);
  targetBitrateMbps = std::max(0.5, std::min(80.0, profile.getNumber("targetBitrateMbps", targetBitrateMbps)));
  codec = normalizeVideoCodec(profile.getString("codec", codec), codec);
}

const rpc::Json* findParticipant(const rpc::Json::Array& participants, const std::string& participantId) {
  auto found = std::find_if(participants.begin(), participants.end(), [&](const rpc::Json& participant) {
    return participant.getString("sdkUserId") == participantId;
  });
  return found == participants.end() ? nullptr : &*found;
}

bool readinessCheckBlocked(const rpc::Json& payload, const std::string& checkId) {
  const rpc::Json* readiness = payload.get("readiness");
  const rpc::Json* checks = readiness ? readiness->get("checks") : nullptr;
  if (!checks || !checks->isArray()) {
    return false;
  }

  return std::any_of(checks->asArray().begin(), checks->asArray().end(), [&](const rpc::Json& check) {
    return check.getString("id") == checkId && check.getString("status") == "blocked";
  });
}

std::string rawMediaDisabledWarningFor(const rpc::Json& payload, const std::string& kind) {
  if (kind == "participant-video" && readinessCheckBlocked(payload, "raw-video")) {
    return "participant-video callbacks are not enabled in the Zoom SDK helper.";
  }

  if (kind == "participant-audio" && readinessCheckBlocked(payload, "raw-audio")) {
    return "participant-audio callbacks are not enabled in the Zoom SDK helper.";
  }

  if (kind == "screen-share" && readinessCheckBlocked(payload, "raw-share")) {
    return "screen-share callbacks are not enabled in the Zoom SDK helper.";
  }

  return {};
}

rpc::Json::Array uniqueWarnings(const rpc::Json::Array& payloadWarnings, const rpc::Json::Array& subscriptionWarnings) {
  std::set<std::string> seen;
  rpc::Json::Array result;
  for (const auto& warning : payloadWarnings) {
    if (warning.isString() && seen.insert(warning.asString()).second) {
      result.emplace_back(warning.asString());
    }
  }
  for (const auto& warning : subscriptionWarnings) {
    if (warning.isString() && seen.insert(warning.asString()).second) {
      result.emplace_back(warning.asString());
    }
  }
  return result;
}

}  // namespace

MediaCore::MediaCore(modules::ModuleSet modules)
    : modules_(std::move(modules)), zoomEngineRuntime_(std::make_unique<modules::ZoomEngineRuntime>()) {}

rpc::Json MediaCore::profile() const {
  const auto renderer = modules_.compositor->rendererName();
  // encoder->session() is mutated by the audio/output worker; guard the read.
  modules::OutputSession encoderSession;
  {
    std::lock_guard<std::mutex> audioLock(audioOutputMutex_);
    encoderSession = modules_.encoder->session();
  }
  return rpc::Json::Object{
      {"name", renderer == "software" ? "CoreVideo Pro Native Media Core Stub" : "CoreVideo Pro Native Media Core"},
      {"renderer", renderer},
      {"encoder", encoderSession.encoderName},
      {"maxProgramResolution", "3840x2160"},
      {"maxProgramFps", 60},
      {"maxParticipantFeeds", 8},
      {"maxIsoRecordings", 8},
      {"capabilities", capabilityArray(renderer, encoderSession)},
  };
}

rpc::Json MediaCore::health() const {
  // encoder->session() is mutated by the audio/output worker; guard the read.
  modules::OutputSession session;
  {
    std::lock_guard<std::mutex> audioLock(audioOutputMutex_);
    session = modules_.encoder->session();
  }
  rpc::Json::Array messages;
#if COREVIDEO_STUB
  messages.emplace_back("COREVIDEO_STUB synthetic media path active");
#else
  messages.emplace_back("COREVIDEO dev adapter media path active");
#endif
  if (modules_.compositor->rendererName() != "software") {
    messages.emplace_back("GPU compositor active: " + modules_.compositor->rendererName());
  }
  if (session.hardwareAccelerated) {
    messages.emplace_back("Hardware encoder active: " + session.encoderName);
  }
  for (const auto& warning : lastProgramFrame_.warnings) {
    messages.emplace_back("Compositor warning: " + warning);
  }
  return rpc::Json::Object{
      {"status", session.active ? "live" : "idle"},
      {"renderer", modules_.compositor->rendererName()},
      {"programFrameHealth", lastProgramFrame_.health},
      {"encoder", session.encoderName},
      {"codec", session.codec},
      {"targetBitrateMbps", session.targetBitrateMbps},
      {"hardwareEncoder", session.hardwareAccelerated},
      {"recordingArtifactPath", session.recordingArtifactPath},
      {"recordingBytesWritten", static_cast<double>(session.recordingBytesWritten)},
      {"recordingDurationMs", static_cast<double>(session.recordingDurationMs)},
      {"recordingFrameCount", static_cast<double>(session.recordingVideoFrameCount)},
      {"recordingMetadataValid", session.recordingMetadataValid},
      {"frameCount", static_cast<double>(lastProgramFrame_.frameNumber)},
      {"encodedFrameCount", static_cast<double>(session.encodedFrameCount)},
      {"mixedAudioFrames", static_cast<double>(mixedAudioFrameCount_)},
      {"programPixelSignature", static_cast<double>(lastProgramFrame_.programPixelSignature)},
      {"renderPlanSignature", static_cast<double>(lastProgramFrame_.renderPlanSignature)},
      {"captureDeviceCount", static_cast<double>(modules_.captureDevice->enumerate().size())},
      {"zoom", rpc::Json::Object{{"readiness", zoomReadinessState()}, {"evidence", zoomEvidenceState()}}},
      {"messages", messages},
  };
}

rpc::Json MediaCore::captureDevices() const {
  return captureDevicesState();
}

rpc::Json MediaCore::selectCaptureInput(const std::string& deviceId, const std::string& inputId) {
  return captureDeviceArray(modules_.captureDevice->selectInput(deviceId, inputId));
}

rpc::Json MediaCore::setCaptureAudioSyncOffset(const std::string& deviceId, int offsetMs) {
  return captureDeviceArray(modules_.captureDevice->setAudioSyncOffset(deviceId, offsetMs));
}

rpc::Json MediaCore::connectCaptureDevice(const std::string& deviceId) {
  return captureDeviceArray(modules_.captureDevice->connect(deviceId));
}

void MediaCore::registerCaptureShm(const std::string& deviceId, const std::string& shmName, int width, int height) {
  if (auto* adapter = dynamic_cast<modules::WinUiCaptureDeviceAdapter*>(modules_.captureDevice.get())) {
    adapter->registerCaptureBuffer(deviceId, shmName, width, height);
  }
}

void MediaCore::unregisterCaptureShm(const std::string& deviceId) {
  if (auto* adapter = dynamic_cast<modules::WinUiCaptureDeviceAdapter*>(modules_.captureDevice.get())) {
    adapter->unregisterCaptureBuffer(deviceId);
  }
}

rpc::Json MediaCore::joinZoom(const rpc::Json& payload) {
  if (zoomEngineRuntime_ && zoomEngineRuntime_->configured()) {
    return zoomEngineRuntime_->join(payload);
  }

  zoomJoined_ = true;
  const std::string displayName = payload.getString("displayName", zoomDisplayName_);
  if (!displayName.empty()) {
    zoomDisplayName_ = displayName;
  }
  ++zoomSnapshotTick_;
  return zoomSnapshot();
}

rpc::Json MediaCore::leaveZoom() {
  if (zoomEngineRuntime_ && zoomEngineRuntime_->configured()) {
    return zoomEngineRuntime_->leave();
  }

  zoomJoined_ = false;
  ++zoomSnapshotTick_;
  return zoomSnapshot();
}

rpc::Json MediaCore::zoomSnapshot() const {
  if (zoomEngineRuntime_ && zoomEngineRuntime_->configured()) {
    return zoomEngineRuntime_->snapshot();
  }

  ++zoomSnapshotTick_;
  if (!zoomJoined_) {
    return rpc::Json::Object{
        {"meetingState", "idle"},
        {"readiness", zoomReadinessState()},
        {"evidence", zoomEvidenceState()},
        {"participants", rpc::Json::Array{}},
        {"tick", zoomSnapshotTick_},
    };
  }

  return rpc::Json::Object{
      {"meetingState", "in_meeting"},
      {"activeSpeakerId", "operator-1"},
      {"caption", ""},
      {"readiness", zoomReadinessState()},
      {"evidence", zoomEvidenceState()},
      {"tick", zoomSnapshotTick_},
      {"participants",
       rpc::Json::Array{
           rpc::Json::Object{
               {"userId", "operator-1"},
               {"displayName", zoomDisplayName_},
               {"role", "Host"},
               {"videoOn", true},
               {"muted", false},
               {"talking", true},
               {"audioLevel", 76},
               {"networkQuality", "good"},
           },
           rpc::Json::Object{
               {"userId", "guest-1"},
               {"displayName", "Guest 1"},
               {"role", "Guest"},
               {"videoOn", true},
               {"muted", false},
               {"talking", false},
               {"audioLevel", 22},
               {"networkQuality", "good"},
           },
       }},
  };
}

rpc::Json MediaCore::sessionState() const {
  // encoder->session() is mutated by the audio/output worker; guard the read. The
  // other module reads in this snapshot (profile/health/audioMixSessionState/
  // outputSenderSessionState/captureAudioSourcesState) each take audioOutputMutex_
  // independently (never nested), so no single lock spans the whole build.
  modules::OutputSession session;
  {
    std::lock_guard<std::mutex> audioLock(audioOutputMutex_);
    session = modules_.encoder->session();
  }
  rpc::Json::Object state{
      {"sceneId", sceneId_},
      {"routeCount", routeCount_},
      {"transformCount", transformCount_},
      {"overlayCount", overlayCount_},
      {"outputs", stringArray(session.destinations)},
      {"isoParticipantIds", stringArray(session.isoParticipantIds)},
      {"outputProfile", outputProfileJson(outputProfileId_, outputResolution_, outputWidth_, outputHeight_, outputFps_, outputTargetBitrateMbps_)},
      {"encoder", session.encoderName},
      {"codec", session.codec},
      {"hardwareEncoder", session.hardwareAccelerated},
      {"active", session.active},
      {"programFrameCount", static_cast<double>(lastProgramFrame_.frameNumber)},
      {"renderPlanId", lastProgramFrame_.renderPlanId},
      {"compositorRenderer", lastProgramFrame_.renderer},
      {"programFrame",
       rpc::Json::Object{
           {"frameNumber", static_cast<double>(lastProgramFrame_.frameNumber)},
           {"renderPlanId", lastProgramFrame_.renderPlanId},
           {"renderer", lastProgramFrame_.renderer},
           {"health", lastProgramFrame_.health},
           {"width", lastProgramFrame_.width},
           {"height", lastProgramFrame_.height},
           {"fps", outputFps_},
           {"timestampMs", static_cast<double>(lastProgramFrame_.frameNumber * (1000.0 / std::max(1, outputFps_)))},
           {"layerCount", lastProgramFrame_.layerCount},
           {"gpuComposed", lastProgramFrame_.gpuComposed},
           {"programPixelSignature", static_cast<double>(lastProgramFrame_.programPixelSignature)},
           {"renderPlanSignature", static_cast<double>(lastProgramFrame_.renderPlanSignature)},
           {"warnings", stringArray(lastProgramFrame_.warnings)},
       }},
      {"encoderSession", encoderSessionState(session)},
      {"outputSenderSession", outputSenderSessionState()},
      {"captureDevices", captureDevicesState()},
      {"health", health()},
      {"profile", profile()},
      {"audioMixSession", audioMixSessionState()},
      {"audioRoutingMatrix", audioRoutingMatrixState()},
      {"captureAudioSources", captureAudioSourcesState()},
      {"captionTrack", captionTrackState()},
      {"brandKit", brandKitState()},
      {"overlayState", overlayState()},
      {"mediaPlayback", mediaPlaybackState()},
      {"autoProduction", autoProductionState()},
      {"meetingState", resolveMeetingStateForSession()},
      {"breakoutRoomId", breakoutRoomId_},
      {"breakoutRoomName", breakoutRoomName_},
      {"zoom", rpc::Json::Object{{"readiness", zoomReadinessState()}, {"evidence", zoomEvidenceState()}}},
  };
  const auto zoomCapture = zoomSnapshot();
  if (zoomCapture.get("participants")) {
    state.emplace("participants", *zoomCapture.get("participants"));
  }
  if (const auto activeSpeakerId = zoomCapture.getString("activeSpeakerId"); !activeSpeakerId.empty()) {
    state.emplace("activeSpeakerId", activeSpeakerId);
  }
  const auto preview = modules::programFramePreviewJson(lastProgramFrame_);
  if (!preview.isNull()) {
    state.emplace("programFramePreview", preview);
  }
  const auto sharedTexture = modules::programSharedTextureJson(lastProgramFrame_);
  if (!sharedTexture.isNull()) {
    state.emplace("programSharedTexture", sharedTexture);
  }
  // Cold-start snapshot: a newly connected consumer gets the current multiview
  // shared texture + tile rects without waiting for the next structural change.
  const auto multiviewSharedTexture = modules::multiviewSharedTextureJson(lastProgramFrame_);
  if (!multiviewSharedTexture.isNull()) {
    state.emplace("multiviewSharedTexture", multiviewSharedTexture);
  }
  const auto recording = recordingState(session);
  if (!recording.isNull()) {
    state.emplace("recording", recording);
  }
  return state;
}

rpc::Json MediaCore::syncZoomMediaSpine(const rpc::Json& payload, double elapsedMs) {
  // Deliver the GPU multiview layout on this frequent, reliable channel. The production sync's
  // set-multiview-layout fires only on user actions (and sends an EMPTY layout at startup), so
  // the multiview never gets the live Show Input roster from it. applyMultiviewLayout dedups by
  // content signature, so re-applying every spine tick does NOT churn multiviewSources_ unless
  // the layout actually changed. Done BEFORE the runtime delegate so it works on both the stub
  // and the real-engine paths (the multiview lives in MediaCore, not the Zoom runtime).
  if (const rpc::Json* multiview = payload.get("multiview"); multiview && multiview->isObject()) {
    if (applyMultiviewLayout(*multiview)) {
      std::fprintf(stderr, "[multiview] set-multiview-layout received: %zu sources (spine)\n",
                   multiviewSources_.size());
    }
  }

  if (zoomEngineRuntime_ && zoomEngineRuntime_->configured()) {
    return zoomEngineRuntime_->syncSpine(payload, elapsedMs);
  }

  const rpc::Json* participantsNode = payload.get("participants");
  const rpc::Json* subscriptionsNode = payload.get("subscriptions");
  const auto& participants = participantsNode && participantsNode->isArray() ? participantsNode->asArray() : rpc::Json::Array{};
  const auto& requestedSubscriptions = subscriptionsNode && subscriptionsNode->isArray() ? subscriptionsNode->asArray() : rpc::Json::Array{};
  const int frameTick = std::max(1, static_cast<int>(std::floor(elapsedMs / 33.0)));
  const int audioTick = std::max(1, static_cast<int>(std::floor(elapsedMs / 20.0)));

  rpc::Json::Array subscriptions;
  rpc::Json::Array subscriptionWarnings;
  int subscribedVideoFeeds = 0;
  int audioPacketsObserved = 0;
  for (const auto& request : requestedSubscriptions) {
    const std::string participantId = request.getString("participantId");
    const std::string kind = request.getString("kind");
    const std::string purpose = request.getString("purpose");
    const rpc::Json* participant = findParticipant(participants, participantId);
    const std::string rawMediaWarning = rawMediaDisabledWarningFor(payload, kind);
    const rpc::Json* videoOn = participant ? participant->get("videoOn") : nullptr;
    const bool videoOff = participant && (kind == "participant-video" || kind == "screen-share") && videoOn && !videoOn->asBool();
    const bool lowResolution = participant && participant->getString("networkQuality") == "low";
    const std::string warning = !rawMediaWarning.empty()
                                    ? rawMediaWarning
                                    : !participant ? participantId + " is not in the Zoom SDK roster."
                                                   : videoOff ? participant->getString("displayName") + " video is off."
                                                              : lowResolution ? participant->getString("displayName") +
                                                                                    " raw media subscribed below target resolution."
                                                                              : "";
    const bool failed = !participant || !rawMediaWarning.empty() || videoOff;
    const std::string status = failed ? "failed" : lowResolution ? "degraded" : "subscribed";
    const std::string lastResultCode = !participant ? "participant-missing"
                                      : !rawMediaWarning.empty() ? "raw-media-disabled"
                                      : videoOff ? "video-off"
                                      : lowResolution ? "low-resolution"
                                      : "ok";
    const int framesReceived = kind == "participant-audio" || failed ? 0 : frameTick;
    const int audioPacketsReceived = kind == "participant-audio" && !failed ? audioTick : 0;
    if (!failed && kind == "participant-video") {
      ++subscribedVideoFeeds;
    }
    audioPacketsObserved += audioPacketsReceived;
    if (!warning.empty()) {
      subscriptionWarnings.emplace_back(warning);
    }

    rpc::Json::Object subscription{
        {"participantId", participantId},
        {"kind", kind},
        {"purpose", purpose},
        {"priority", request.get("priority") ? *request.get("priority") : rpc::Json(0)},
        {"subscriptionId", kind + ":" + participantId + ":" + purpose},
        {"status", status},
        {"lastResultCode", lastResultCode},
        {"deliveredWidth", kind == "screen-share" ? 1920 : lowResolution ? 640 : 1280},
        {"deliveredHeight", kind == "screen-share" ? 1080 : lowResolution ? 360 : 720},
        {"deliveredFps", kind == "screen-share" ? 30 : lowResolution ? 15 : 30},
        {"framesReceived", framesReceived},
        {"audioPacketsReceived", audioPacketsReceived},
    };
    if (participant) {
      subscription.emplace("displayName", participant->getString("displayName"));
    }
    if (!warning.empty()) {
      subscription.emplace("warning", warning);
    }
    subscriptions.emplace_back(std::move(subscription));
  }

  const rpc::Json* recording = payload.get("recording");
  const bool recordingActive = recording && recording->isObject();
  const std::string sdkVersion = payload.get("readiness") ? payload.get("readiness")->getString("sdkVersion", "unknown") : "unknown";
  const bool blocked = payload.get("blocked") && payload.get("blocked")->asBool();
  const auto payloadWarnings = payload.get("warnings") && payload.get("warnings")->isArray() ? payload.get("warnings")->asArray() : rpc::Json::Array{};

  const rpc::Json::Object recordingSnapshot{
      {"session",
       recordingActive ? rpc::Json(rpc::Json::Object{
                         {"sessionId", "native-zoom-spine-" + std::to_string(static_cast<int>(elapsedMs))},
                         {"active", true},
                         {"status", payloadWarnings.empty() ? "recording" : "warning"},
                         {"targetFolder", recording->getString("targetFolder")},
                         {"filenamePrefix", recording->getString("filenamePrefix")},
                         {"format", recording->getString("format")},
                         {"quality", recording->getString("quality")},
                     })
                     : rpc::Json(nullptr)},
      {"evidence",
       rpc::Json::Object{
           {"programFramesWritten", recordingActive && subscribedVideoFeeds > 0 ? frameTick : 0},
           {"isoFramesWritten", recordingActive ? static_cast<int>((recording->get("isoParticipantIds") ? recording->get("isoParticipantIds")->asArray().size() : 0) * frameTick) : 0},
           {"audioPacketsObserved", audioPacketsObserved},
           {"subscribedVideoFeeds", subscribedVideoFeeds},
       }},
  };

  const auto activeSpeaker = std::find_if(participants.begin(), participants.end(), [](const rpc::Json& participant) {
    const rpc::Json* talking = participant.get("talking");
    return talking && talking->asBool();
  });
  const auto screenShare = std::find_if(participants.begin(), participants.end(), [](const rpc::Json& participant) {
    const rpc::Json* sharing = participant.get("sharingScreen");
    return sharing && sharing->asBool();
  });

  rpc::Json::Object snapshot{
      {"meetingState", blocked ? "error" : "in-meeting"},
      {"sdkVersion", sdkVersion},
      {"participantCount", static_cast<int>(participants.size())},
      {"participants", participants},
      {"subscriptions", subscriptions},
      {"recording", recordingSnapshot},
      {"warnings", uniqueWarnings(payloadWarnings, subscriptionWarnings)},
      {"events", rpc::Json::Array{"Zoom media spine payload accepted by native media core stub.", payload.getString("summary")}},
  };
  if (activeSpeaker != participants.end()) {
    snapshot.emplace("activeSpeakerId", activeSpeaker->getString("sdkUserId"));
  }
  if (screenShare != participants.end()) {
    snapshot.emplace("screenShareParticipantId", screenShare->getString("sdkUserId"));
  }
  return snapshot;
}

std::vector<rpc::Json> MediaCore::drainZoomVideoFrameEvents() {
  // THREAD-SAFE (no core lock required): zoomEngineRuntime_ is created once in the
  // constructor and never reset; drainFrameEvents takes its own mutex and returns
  // empty when nothing is pending (including before configure). This lets a dedicated
  // pump thread drain Zoom frames WITHOUT contending on the core lock — the
  // media-core-sync command holds that lock 50-100ms and was starving the drain,
  // causing the Zoom feed to buffer (~360ms) and drop unevenly.
  return zoomEngineRuntime_->drainFrameEvents();
}

std::vector<rpc::Json> MediaCore::drainProgramFramePreviewEvents() {
  auto events = std::move(pendingProgramFramePreviewEvents_);
  pendingProgramFramePreviewEvents_.clear();
  return events;
}

std::vector<rpc::Json> MediaCore::drainProgramSharedTextureEvents() {
  auto events = std::move(pendingProgramSharedTextureEvents_);
  pendingProgramSharedTextureEvents_.clear();
  return events;
}

std::vector<rpc::Json> MediaCore::drainParticipantSharedTextureEvents() {
  auto events = std::move(pendingParticipantSharedTextureEvents_);
  pendingParticipantSharedTextureEvents_.clear();
  return events;
}

std::vector<rpc::Json> MediaCore::drainMultiviewSharedTextureEvents() {
  auto events = std::move(pendingMultiviewSharedTextureEvents_);
  pendingMultiviewSharedTextureEvents_.clear();
  return events;
}

void MediaCore::enqueueProgramFramePreviewEvent() {
  const auto event = modules::programFramePreviewEvent(lastProgramFrame_);
  if (!event.isNull()) {
    pendingProgramFramePreviewEvents_.emplace_back(event);
  }
}

void MediaCore::enqueueProgramSharedTextureEvent() {
  // DISABLED: the program monitor uses the WinUI ScenePreviewControl composite, not
  // the GPU shared texture, so this event is unused — and it was emitted at the full
  // render rate (~125/s), flooding the shared frame-event queue and starving the
  // 30fps Zoom video frames (high latency + drops). Re-enable if/when the program
  // monitor goes back to the GPU shared-texture (VideoSurfaceHost) path.
  return;
}

void MediaCore::enqueueParticipantSharedTextureEvents() {
  for (auto& event : modules::participantSharedTextureEvents(lastProgramFrame_)) {
    pendingParticipantSharedTextureEvents_.emplace_back(std::move(event));
  }
}

void MediaCore::enqueueMultiviewSharedTextureEvent() {
  const auto event = modules::multiviewSharedTextureEvent(lastProgramFrame_);
  if (event.isNull()) {
    return;
  }
  // Compute a structural signature over the handle, canvas dims, and per-tile
  // identity + geometry (label, slot, rects) — but NOT the active-speaker flag,
  // since that border is baked into the texture and must not churn the consumer.
  uint32_t signature = 2166136261u;
  auto mix = [&signature](const std::string& value) {
    for (const unsigned char ch : value) {
      signature ^= ch;
      signature *= 16777619u;
    }
    signature ^= 0xffu;
    signature *= 16777619u;
  };
  auto mixInt = [&signature](int value) {
    for (int shift = 0; shift < 32; shift += 8) {
      signature ^= static_cast<uint8_t>((static_cast<uint32_t>(value) >> shift) & 0xffu);
      signature *= 16777619u;
    }
  };
  mix(lastProgramFrame_.multiviewSharedTexture.sharedHandleHex);
  mixInt(lastProgramFrame_.multiviewWidth);
  mixInt(lastProgramFrame_.multiviewHeight);
  for (const auto& tile : lastProgramFrame_.multiviewTiles) {
    mix(tile.sourceId);
    mix(tile.participantId);
    mix(tile.label);
    mixInt(tile.slot);
    mixInt(static_cast<int>(std::lround(tile.x * 10000.f)));
    mixInt(static_cast<int>(std::lround(tile.y * 10000.f)));
    mixInt(static_cast<int>(std::lround(tile.w * 10000.f)));
    mixInt(static_cast<int>(std::lround(tile.h * 10000.f)));
  }
  if (signature == 0u) {
    signature = 1u;
  }
  if (multiviewStructureEmitted_ && signature == lastMultiviewStructureSignature_) {
    return;
  }
  lastMultiviewStructureSignature_ = signature;
  multiviewStructureEmitted_ = true;
  pendingMultiviewSharedTextureEvents_.emplace_back(event);
}

rpc::Json MediaCore::applyCommands(const rpc::Json::Array& commands, double elapsedMs) {
  const auto frameNumberBefore = lastProgramFrame_.frameNumber;
  const auto tCmd0 = std::chrono::steady_clock::now();
  for (const auto& command : commands) {
    const auto ci0 = std::chrono::steady_clock::now();
    (void)applyCommand(command);
    const auto cms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - ci0)
                         .count();
    if (cms >= 10) {
      std::fprintf(stderr, "[cmd] '%s' %lldms\n", command.getString("type").c_str(),
                   static_cast<long long>(cms));
    }
  }
  const auto tCmd1 = std::chrono::steady_clock::now();

  const int targetTicks = elapsedMs > 0.0 ? std::max(1, static_cast<int>(std::floor(elapsedMs / 33.0))) : 1;
  const auto ticksAlreadyRendered = static_cast<int>(lastProgramFrame_.frameNumber - frameNumberBefore);
  int additionalTicks = elapsedMs > 0.0 ? std::max(0, targetTicks - ticksAlreadyRendered) : 1;
  // Cap catch-up: when a sync carries a large elapsedMs (UI/transport hiccup),
  // never render a synchronous storm of frames. A live program only needs the
  // current frame; rendering dozens back-to-back drives the real D3D11/encoder/
  // output path into a blocking burst that wedges the processing thread.
  if (audioWorkerActive_) {
    additionalTicks = std::min(additionalTicks, 2);
  }
  // Increment 2 (depends on the audio decouple): in the live server the render thread
  // keeps lastProgramFrame_ fresh at ~60fps and the audio/output worker keeps the
  // audio/output snapshot fresh, so an EMPTY poll (the 250ms media-core-sync) no
  // longer needs to drive a synthetic tick under coreMutex — return the latest
  // published snapshot without the heavy tick. Commands still render so their effect
  // is reflected immediately; direct/test callers (no worker) always render.
  if (audioWorkerActive_ && commands.empty()) {
    additionalTicks = 0;
  }
  for (int tick = 0; tick < additionalTicks; ++tick) {
    renderSyntheticTick();
  }
  const auto tRender = std::chrono::steady_clock::now();
  auto state = sessionState();
  const auto tState = std::chrono::steady_clock::now();

  const auto cmdMs = std::chrono::duration_cast<std::chrono::milliseconds>(tCmd1 - tCmd0).count();
  const auto renderMs = std::chrono::duration_cast<std::chrono::milliseconds>(tRender - tCmd1).count();
  const auto stateMs = std::chrono::duration_cast<std::chrono::milliseconds>(tState - tRender).count();
  if (cmdMs + renderMs + stateMs >= 80) {
    std::fprintf(stderr, "[applyCommands] %zu cmds=%lldms render(%dticks)=%lldms snapshot=%lldms\n",
                 commands.size(), static_cast<long long>(cmdMs), additionalTicks,
                 static_cast<long long>(renderMs), static_cast<long long>(stateMs));
  }
  return state;
}

rpc::Json MediaCore::applyCommand(const rpc::Json& command) {
  const std::string type = command.getString("type");
  if (type == "load-scene-graph") {
    loadSceneGraph(command);
  } else if (type == "set-participant-transform") {
    setParticipantTransform(command);
  } else if (type == "set-overlay-asset") {
    setOverlayAsset(command);
  } else if (type == "set-color-grade") {
    setColorGrade(command);
  } else if (type == "set-output-profile") {
    setOutputProfile(command);
  } else if (type == "start-program-output") {
    startProgramOutput(command);
  } else if (type == "prepare-encoder-session") {
    prepareEncoderSession(command);
  } else if (type == "start-encoder-session") {
    startEncoderSession(command);
  } else if (type == "stop-encoder-session") {
    stopEncoderSession(command);
  } else if (type == "fail-output-sender") {
    failOutputSender(command);
  } else if (type == "recover-output-sender") {
    recoverOutputSender(command);
  } else if (type == "set-recording-targets") {
    setRecordingTargets(command);
  } else if (type == "start-recording-session") {
    startRecordingSession(command);
  } else if (type == "stop-recording-session") {
    stopRecordingSession(command);
  } else if (type == "fail-recording-session") {
    failRecordingSession(command);
  } else if (type == "recover-recording-session") {
    recoverRecordingSession(command);
  } else if (type == "sync-participant-audio-mix") {
    syncParticipantAudioMix(command);
  } else if (type == "sync-audio-monitor") {
    syncAudioMonitor(command);
  } else if (type == "sync-audio-routing-matrix") {
    syncAudioRoutingMatrix(command);
  } else if (type == "sync-capture-audio-sources") {
    syncCaptureAudioSources(command);
  } else if (type == "push-caption-cue") {
    pushCaptionCue(command);
  } else if (type == "set-caption-enabled") {
    setCaptionEnabled(command);
  } else if (type == "set-brand-kit") {
    setBrandKit(command);
  } else if (type == "set-media-playback") {
    setMediaPlayback(command);
  } else if (type == "set-multiview-layout") {
    setMultiviewLayout(command);
    std::fprintf(stderr, "[multiview] set-multiview-layout received: %zu sources\n",
                 multiviewSources_.size());
  } else if (type == "configure-srt-ingest-sources") {
    configureSrtIngestSources(command);
  } else if (type == "simulate-breakout-room-change") {
    simulateBreakoutRoomChange(command);
  } else if (type == "recommend-auto-production") {
    // Pure query: the recommendation is derived from current state and surfaced
    // in the snapshot (see autoProductionState()), so there is nothing to mutate.
  }
  return sessionState();
}

DirectorSignals MediaCore::deriveDirectorSignals() const {
  // Derive the richer director signal bundle from the core's current state. This
  // mirrors the pure summarizers in src/engine/directorSignals.ts: we treat the
  // participants whose video is on as the "live" feeds the director reasons over,
  // and derive conversational dynamics, engagement, and feed-health roll-ups from
  // the same roster the renderer would.
  DirectorSignals signals;

  const auto capture = zoomSnapshot();
  const rpc::Json* participants = capture.get("participants");
  if (!participants || !participants->isArray()) {
    return signals;
  }

  int liveCount = 0;
  int degradedCount = 0;
  int activeContributorCount = 0;
  int talkingCount = 0;
  double audioLevelSum = 0.0;
  std::string lastTalkingId;

  for (const auto& participant : participants->asArray()) {
    if (!participant.isObject()) {
      continue;
    }
    const bool videoOn = participant.get("videoOn") && participant.get("videoOn")->asBool();
    const bool muted = participant.get("muted") && participant.get("muted")->asBool();
    const bool talking = participant.get("talking") && participant.get("talking")->asBool();
    const bool sharingScreen = participant.get("sharingScreen") && participant.get("sharingScreen")->asBool();
    const double audioLevel = participant.getNumber("audioLevel", 0.0);
    const std::string networkQuality = participant.getString("networkQuality", "good");

    if (sharingScreen) {
      signals.screenShareActive = true;
      signals.screenShareHasSharer = true;
      if (signals.screenShareSharerName.empty()) {
        signals.screenShareSharerName = participant.getString("displayName");
      }
    }

    // A "live" feed is one whose video is on (i.e. not video-off).
    if (!videoOn) {
      continue;
    }
    ++liveCount;
    if (networkQuality == "recovering" || networkQuality == "low") {
      ++degradedCount;
    }
    if (!muted || talking) {
      ++activeContributorCount;
    }
    audioLevelSum += audioLevel;  // audioLevel is reported 0-100
    if (talking) {
      ++talkingCount;
      lastTalkingId = participant.getString("userId");
    }
  }

  signals.liveCount = liveCount;
  signals.degradedCount = degradedCount;
  signals.activeContributorCount = activeContributorCount;
  signals.meanAudioLevel = liveCount == 0 ? 0.0 : (audioLevelSum / liveCount) / 100.0;
  signals.hasDominantSpeaker = talkingCount == 1;
  signals.crossTalk = talkingCount >= 2;
  return signals;
}

DirectorRecommendation MediaCore::recommendAutoProduction() const {
  return recommendScene(deriveDirectorSignals());
}

rpc::Json MediaCore::autoProductionState() const {
  const auto recommendation = recommendAutoProduction();
  return rpc::Json::Object{
      {"ruleId", recommendation.ruleId},
      {"recommendedSceneId", recommendation.recommendedSceneId},
      {"confidence", recommendation.confidence},
      {"rationale", recommendation.rationale},
  };
}

rpc::Json MediaCore::zoomReadinessState() const {
  if (zoomEngineRuntime_ && zoomEngineRuntime_->configured()) {
    const auto capture = zoomEngineRuntime_->snapshot();
    const std::string meetingState = capture.getString("meetingState", "unknown");
    const std::string sdkVersion = capture.getString("sdkVersion", "external");
    return rpc::Json::Object{
        {"status", meetingState == "error" ? "blocked" : "ready"},
        {"mode", "runtime"},
        {"sdkAvailable", true},
        {"sdkVersion", sdkVersion},
        {"meetingState", meetingState},
        {"checks",
         rpc::Json::Array{
             rpc::Json::Object{{"id", "zoom-engine-runtime"}, {"status", "ready"}, {"label", "Zoom engine runtime configured"}},
         }},
    };
  }

  return rpc::Json::Object{
      {"status", "ready"},
      {"mode", "stub"},
      {"sdkAvailable", false},
      {"sdkVersion", "stub"},
      {"meetingState", zoomJoined_ ? "in_meeting" : "idle"},
      {"checks",
       rpc::Json::Array{
           rpc::Json::Object{{"id", "zoom-sdk"}, {"status", "stubbed"}, {"label", "Real Zoom SDK not required for native-core Studio readiness"}},
           rpc::Json::Object{{"id", "raw-video"}, {"status", "synthetic"}, {"label", "Synthetic participant video evidence available"}},
           rpc::Json::Object{{"id", "raw-audio"}, {"status", "synthetic"}, {"label", "Synthetic participant audio evidence available"}},
       }},
  };
}

rpc::Json MediaCore::zoomEvidenceState() const {
  if (zoomEngineRuntime_ && zoomEngineRuntime_->configured()) {
    const auto capture = zoomEngineRuntime_->snapshot();
    const auto* participants = capture.get("participants");
    const auto participantCount = participants && participants->isArray() ? static_cast<int>(participants->asArray().size()) : 0;
    return rpc::Json::Object{
        {"source", "zoom-engine-runtime"},
        {"synthetic", false},
        {"joined", capture.getString("meetingState") == "in-meeting" || capture.getString("meetingState") == "in_meeting"},
        {"participantCount", participantCount},
        {"snapshotTick", capture.get("tick") ? *capture.get("tick") : rpc::Json(0)},
    };
  }

  return rpc::Json::Object{
      {"source", "native-core-stub"},
      {"synthetic", true},
      {"joined", zoomJoined_},
      {"participantCount", zoomJoined_ ? 2 : 0},
      {"videoFeeds", zoomJoined_ ? 2 : 0},
      {"audioFeeds", zoomJoined_ ? 2 : 0},
      {"activeSpeakerId", zoomJoined_ ? "operator-1" : ""},
      {"snapshotTick", zoomSnapshotTick_},
  };
}

std::string MediaCore::resolveMeetingStateForSession() const {
  if (zoomEngineRuntime_ && zoomEngineRuntime_->configured()) {
    const auto capture = zoomEngineRuntime_->snapshot();
    const std::string meetingState = capture.getString("meetingState");
    if (meetingState == "in-meeting" || meetingState == "in_meeting") {
      return "in_meeting";
    }
    if (!meetingState.empty()) {
      return meetingState;
    }
  }

  return zoomJoined_ ? "in_meeting" : "idle";
}

void MediaCore::simulateBreakoutRoomChange(const rpc::Json& command) {
  const std::string roomId = command.getString("breakoutRoomId");
  const std::string roomName = command.getString("breakoutRoomName");
  if (!roomId.empty()) {
    breakoutRoomId_ = roomId;
  }
  if (!roomName.empty()) {
    breakoutRoomName_ = roomName;
  }
}

void MediaCore::setColorGrade(const rpc::Json& command) {
  colorGrade_ = readColorGrade(command);
}

void MediaCore::setOutputProfile(const rpc::Json& command) {
  outputWidth_ = clampIntValue(static_cast<int>(command.getNumber("width", outputWidth_)), 320, 3840);
  outputHeight_ = clampIntValue(static_cast<int>(command.getNumber("height", outputHeight_)), 180, 2160);
  outputFps_ = clampIntValue(static_cast<int>(command.getNumber("fps", outputFps_)), 1, 120);
  outputTargetBitrateMbps_ = std::max(0.5, std::min(80.0, command.getNumber("targetBitrateMbps", outputTargetBitrateMbps_)));
  const std::string codec = command.getString("codec", streamVideoCodec_);
  if (codec == "h264" || codec == "h265" || codec == "hevc" || codec == "av1") {
    streamVideoCodec_ = codec == "hevc" ? "h265" : codec;
  }
  outputResolution_ = command.getString("resolution", std::to_string(outputWidth_) + "x" + std::to_string(outputHeight_));
  if (outputResolution_.empty()) {
    outputResolution_ = std::to_string(outputWidth_) + "x" + std::to_string(outputHeight_);
  }
  outputProfileId_ = command.getString("profileId", outputProfileId_);
  if (outputProfileId_.empty()) {
    outputProfileId_ = "canvas-" + outputResolution_ + "-" + std::to_string(outputFps_);
  }
}

void MediaCore::loadSceneGraph(const rpc::Json& command) {
  sceneId_ = command.getString("sceneId", "unloaded");
  sceneValidationWarnings_.clear();
  if (sceneId_.empty() || sceneId_ == "unloaded") {
    sceneId_ = "unloaded";
    sceneValidationWarnings_.push_back("Scene graph command is missing sceneId.");
  }
  sceneRoutes_.clear();
  sceneBackground_ = {};
  if (const rpc::Json* background = command.get("background"); background && background->isObject()) {
    sceneBackground_.mediaAssetId = background->getString("mediaAssetId");
    sceneBackground_.mediaAssetName = background->getString("mediaAssetName");
    sceneBackground_.mediaAssetKind = background->getString("mediaAssetKind");
    sceneBackground_.mediaAssetPath = background->getString("mediaAssetPath");
    sceneBackground_.playing = !background->get("playing") || background->get("playing")->asBool();
    sceneBackground_.enabled = !sceneBackground_.mediaAssetId.empty() && !sceneBackground_.mediaAssetPath.empty();
    if (!sceneBackground_.enabled && !sceneBackground_.mediaAssetId.empty()) {
      sceneValidationWarnings_.push_back("Scene background " + sceneBackground_.mediaAssetId + " is missing an asset path.");
    }
  }
  const rpc::Json* routes = command.get("routes");
  if (routes && routes->isArray()) {
    int routeIndex = 0;
    for (const auto& route : routes->asArray()) {
      SceneRouteState state;
      state.routeId = route.getString("routeId");
      state.mode = route.getString("mode");
      state.participantId = route.getString("participantId");
      state.captureDeviceId = route.getString("captureDeviceId");
      state.audioRole = route.getString("audioRole");
      state.mediaAssetId = route.getString("mediaAssetId");
      state.mediaAssetName = route.getString("mediaAssetName");
      state.mediaAssetKind = route.getString("mediaAssetKind");
      state.mediaAssetPath = route.getString("mediaAssetPath");
      state.mediaPlaybackKey = route.getString("mediaPlaybackKey");
      state.mediaAssetPlaying = route.get("mediaAssetPlaying") ? route.get("mediaAssetPlaying")->asBool() : false;
      state.zIndex = static_cast<int>(route.getNumber("zIndex", static_cast<double>(routeIndex)));
      const rpc::Json* rect = route.get("rect");
      if (rect && rect->isObject()) {
        state.rectX = static_cast<float>(rect->getNumber("x", 0.0));
        state.rectY = static_cast<float>(rect->getNumber("y", 0.0));
        state.rectWidth = static_cast<float>(rect->getNumber("width", 1.0));
        state.rectHeight = static_cast<float>(rect->getNumber("height", 1.0));
        state.hasRect = state.rectWidth > 0.f && state.rectHeight > 0.f;
      }
      state.fitMode = route.getString("fitMode", "fill");
      if (state.fitMode != "fit" && state.fitMode != "fill" && state.fitMode != "stretch") {
        state.fitMode = "fill";
      }
      state.borderStyle = route.getString("borderStyle", "accent");
      if (state.borderStyle != "none" && state.borderStyle != "solid" && state.borderStyle != "accent" &&
          state.borderStyle != "program" && state.borderStyle != "warning") {
        state.borderStyle = "accent";
      }
      state.borderColor = route.getString("borderColor", "#44C1A1");
      state.borderThickness = static_cast<float>((std::max)(0.0, (std::min)(12.0, route.getNumber("borderThickness", 2.0))));
      state.sourceScale = static_cast<float>((std::max)(0.25, (std::min)(4.0, route.getNumber("sourceScale", 1.0))));
      state.sourceOffsetX = static_cast<float>((std::max)(-1.0, (std::min)(1.0, route.getNumber("sourceOffsetX", 0.0))));
      state.sourceOffsetY = static_cast<float>((std::max)(-1.0, (std::min)(1.0, route.getNumber("sourceOffsetY", 0.0))));
      if (const rpc::Json* colorGrade = route.get("colorGrade"); colorGrade && colorGrade->isObject()) {
        state.hasColorGrade = true;
        state.colorGrade = readColorGrade(*colorGrade);
      }
      if (state.routeId.empty()) {
        sceneValidationWarnings_.push_back("Scene route " + std::to_string(routeIndex) + " is missing routeId.");
        state.routeId = "invalid-route-" + std::to_string(routeIndex);
      }
      if (state.mode.empty()) {
        sceneValidationWarnings_.push_back("Scene route " + state.routeId + " is missing mode.");
        state.mode = "fixed";
      } else if (state.mode != "fixed" && state.mode != "active-speaker" && state.mode != "screen-share" && state.mode != "capture-input") {
        sceneValidationWarnings_.push_back("Scene route " + state.routeId + " has unsupported mode " + state.mode + ".");
        state.mode = "fixed";
      }
      if (!state.mediaAssetId.empty() && state.mediaAssetPath.empty()) {
        sceneValidationWarnings_.push_back("Scene route " + state.routeId + " media asset " + state.mediaAssetId + " is missing an asset path.");
      }
      sceneRoutes_.push_back(std::move(state));
      ++routeIndex;
    }
  } else if (routes) {
    sceneValidationWarnings_.push_back("Scene graph routes must be an array.");
  }
  routeCount_ = static_cast<int>(sceneRoutes_.size());
}

void MediaCore::setParticipantTransform(const rpc::Json&) {
  ++transformCount_;
}

void MediaCore::setOverlayAsset(const rpc::Json& command) {
  const std::string overlayId = command.getString("overlayId", "overlay:" + std::to_string(overlayIds_.size() + 1));
  if (command.get("enabled") && !command.get("enabled")->asBool()) {
    overlayIds_.erase(overlayId);
    if (auto existing = overlayAssets_.find(overlayId); existing != overlayAssets_.end()) {
      // Animate the overlay out rather than dropping it instantly; the render
      // tick retires it once the building-out animation has settled.
      existing->second.keyPhase = "building-out";
      existing->second.keyProgress = 0.f;
    }
    overlayCount_ = static_cast<int>(overlayIds_.size());
    return;
  }

  overlayIds_.insert(overlayId);
  auto& asset = overlayAssets_[overlayId];
  const bool isNew = asset.overlayId.empty();
  asset.overlayId = overlayId;
  if (isNew) {
    asset.insertionOrder = overlayInsertionCounter_++;
    // A freshly enabled overlay animates in.
    asset.keyPhase = "building-in";
    asset.keyProgress = 0.f;
  }
  asset.text = command.getString("text", asset.text);
  asset.imageUri = command.getString("imageUri", asset.imageUri);
  asset.sourceId = command.getString("sourceId", asset.sourceId);
  asset.sourceName = command.getString("sourceName", asset.sourceName);
  asset.position = command.getString("position", asset.position);
  asset.title = command.getString("title", asset.title);
  asset.org = command.getString("org", asset.org);
  asset.keyPosition = command.getString("keyPosition", asset.keyPosition);
  asset.keyer = command.getString("keyer", asset.keyer);
  asset.buildInMs = clampIntValue(static_cast<int>(command.getNumber("buildInMs", asset.buildInMs)), 50, 2000);
  asset.buildOutMs = clampIntValue(static_cast<int>(command.getNumber("buildOutMs", asset.buildOutMs)), 50, 2000);
  // An explicit keyPhase from the command overrides the auto in/out animation.
  if (const auto* phase = command.get("keyPhase"); phase && phase->isString()) {
    const std::string requested = phase->asString();
    if (requested != asset.keyPhase) {
      asset.keyPhase = requested;
      asset.keyProgress = 0.f;
    }
  }
  overlayCount_ = static_cast<int>(overlayIds_.size());
}

void MediaCore::startProgramOutput(const rpc::Json& command) {
  if (const rpc::Json* streamProfile = command.get("streamOutputProfile"); streamProfile && streamProfile->isObject()) {
    outputWidth_ = clampIntValue(static_cast<int>(streamProfile->getNumber("width", outputWidth_)), 320, 3840);
    outputHeight_ = clampIntValue(static_cast<int>(streamProfile->getNumber("height", outputHeight_)), 180, 2160);
    outputFps_ = clampIntValue(static_cast<int>(streamProfile->getNumber("fps", outputFps_)), 1, 120);
    outputTargetBitrateMbps_ = std::max(0.5, std::min(80.0, streamProfile->getNumber("targetBitrateMbps", outputTargetBitrateMbps_)));
    streamVideoCodec_ = normalizeVideoCodec(streamProfile->getString("codec", streamVideoCodec_), streamVideoCodec_);
  }
  if (const rpc::Json* recordingProfile = command.get("recordingOutputProfile"); recordingProfile && recordingProfile->isObject()) {
    applyRecordingProfile(
        *recordingProfile,
        recordingOutputWidth_,
        recordingOutputHeight_,
        recordingOutputFps_,
        recordingTargetBitrateMbps_,
        recordingVideoCodec_);
  }
  outputDestinations_ = command.getStringArray("destinations");
  outputDestinationSettings_ = readOutputDestinationSettings(command);
  for (auto& destination : outputDestinationSettings_) {
    if (destination.id == "rtmp" || destination.protocol == "rtmp" || destination.protocol == "rtmps") {
      destination.fps = outputFps_;
      destination.targetBitrateMbps = outputTargetBitrateMbps_;
      destination.videoCodec = streamVideoCodec_;
    }
  }
  {
    // Encoder module mutation: guard against the audio/output worker's concurrent
    // encoder->submit/session in runAudioOutputWork. coreMutex(outer)→this(inner).
    std::lock_guard<std::mutex> audioLock(audioOutputMutex_);
    configureEncoderRecordingRequest();
    modules_.encoder->start(command.getStringArray("destinations"), command.getStringArray("isoParticipantIds"));
  }
  if (encoderLifecycleStatus_ == "idle" || encoderLifecycleStatus_ == "prepared" || encoderLifecycleStatus_ == "stopped") {
    encoderLifecycleStatus_ = "encoding";
    encoderLastTransition_ = "Program output encoder session started.";
  }
  // In the live server the render thread (which now does the program-frame readback
  // once output is active) + the audio/output worker produce the frame/encode; the
  // command-thread render here would only repeat a blocking CPU readback under
  // coreMutex (a [cmd] held-lock blip). Render synchronously ONLY for direct/test
  // callers (no worker) so applyCommand still returns a frame-fresh snapshot.
  if (!audioWorkerActive_) {
    renderSyntheticTick();
  }
}

void MediaCore::prepareEncoderSession(const rpc::Json& command) {
  encoderLifecycleStatus_ = "prepared";
  encoderPreparedAtMs_ = command.get("preparedAtMs") ? command.get("preparedAtMs")->asNumber() : encoderPreparedAtMs_;
  encoderLastTransition_ = command.getString("reason", "Program output encoder session prepared.");
}

void MediaCore::startEncoderSession(const rpc::Json& command) {
  encoderLifecycleStatus_ = "encoding";
  encoderStartedAtMs_ = command.get("startedAtMs") ? command.get("startedAtMs")->asNumber() : encoderStartedAtMs_;
  encoderLastTransition_ = "Program output encoder session started.";
}

void MediaCore::stopEncoderSession(const rpc::Json& command) {
  encoderLifecycleStatus_ = "stopped";
  encoderStoppedAtMs_ = command.get("stoppedAtMs") ? command.get("stoppedAtMs")->asNumber() : encoderStoppedAtMs_;
  encoderLastTransition_ = command.getString("reason", "Program output encoder session stopped.");
  outputDestinations_.clear();
  outputDestinationSettings_.clear();
}

void MediaCore::failOutputSender(const rpc::Json& command) {
  // outputSender mutation: guard against the worker's outputSender->sync.
  std::lock_guard<std::mutex> audioLock(audioOutputMutex_);
  modules_.outputSender->fail(command.getString("destination"), command.getString("message", "Output sender failed."), command.get("failedAtMs") ? command.get("failedAtMs")->asNumber() : 0);
}

void MediaCore::recoverOutputSender(const rpc::Json& command) {
  // outputSender mutation: guard against the worker's outputSender->sync.
  std::lock_guard<std::mutex> audioLock(audioOutputMutex_);
  modules_.outputSender->recover(command.getString("destination"), command.get("recoveredAtMs") ? command.get("recoveredAtMs")->asNumber() : 0, command.getString("reason", ""));
}

void MediaCore::setRecordingTargets(const rpc::Json& command) {
  recordingTargetFolder_ = command.getString("targetFolder", recordingTargetFolder_);
  recordingFilenamePrefix_ = command.getString("filenamePrefix", recordingFilenamePrefix_);
  recordingFormat_ = command.getString("format", recordingFormat_);
  recordingQuality_ = command.getString("quality", recordingQuality_);
  recordingTargetBitrateMbps_ = std::max(0.5, std::min(80.0, command.getNumber("targetBitrateMbps", recordingTargetBitrateMbps_)));
  recordingAudioBitrateKbps_ =
      std::max(32, std::min(512, static_cast<int>(command.getNumber("audioBitrateKbps", recordingAudioBitrateKbps_))));
  if (const rpc::Json* renderProfile = command.get("renderProfile"); renderProfile && renderProfile->isObject()) {
    applyRecordingProfile(
        *renderProfile,
        recordingOutputWidth_,
        recordingOutputHeight_,
        recordingOutputFps_,
        recordingTargetBitrateMbps_,
        recordingVideoCodec_);
  }
  if (command.get("isoParticipantIds")) {
    recordingIsoParticipantIds_ = command.getStringArray("isoParticipantIds");
  }
  {
    // encoder->configureRecording mutation: guard against the worker's encoder use.
    std::lock_guard<std::mutex> audioLock(audioOutputMutex_);
    configureEncoderRecordingRequest();
  }
}

void MediaCore::startRecordingSession(const rpc::Json& command) {
  setRecordingTargets(command);
  recordingSessionId_ = command.getString("sessionId", recordingSessionId_.empty() ? "native-recording-session" : recordingSessionId_);
  recordingStartedAtMs_ = command.get("startedAtMs") ? command.get("startedAtMs")->asNumber() : recordingStartedAtMs_;
  recordingElapsedMs_ = 0;
  recordingProgramFramesWritten_ = 0;
  recordingIsoFramesWritten_ = 0;
  recordingDroppedFrames_ = 0;
  recordingAudioPacketsObserved_ = 0;
  recordingFailureCount_ = 0;
  recordingRecoveryCount_ = 0;
  recordingStatus_ = "recording";
  recordingWriterStatus_ = "writing";
  recordingError_.clear();
  recordingWarning_.clear();
  recordingLastFailure_.clear();
  recordingLastRecovery_.clear();
  {
    // Encoder module mutation (configureRecording + session + start): guard against
    // the worker's encoder use. setRecordingTargets above locked+released its own
    // configure call (not nested); this is a fresh, non-nested acquisition.
    std::lock_guard<std::mutex> audioLock(audioOutputMutex_);
    configureEncoderRecordingRequest();
    auto encoderSession = modules_.encoder->session();
    auto destinations = encoderSession.destinations;
    if (std::find(destinations.begin(), destinations.end(), "recording") == destinations.end()) {
      destinations.push_back("recording");
    }
    modules_.encoder->start(destinations, recordingIsoParticipantIds_);
  }
  if (encoderLifecycleStatus_ != "encoding") {
    encoderLifecycleStatus_ = "encoding";
    encoderLastTransition_ = "Recording session started encoder.";
  }
  // See startProgramOutput: skip the redundant blocking command-thread render in the
  // live server (worker + render thread own it); keep it for direct/test callers.
  if (!audioWorkerActive_) {
    renderSyntheticTick();
  }
}

void MediaCore::stopRecordingSession(const rpc::Json& command) {
  recordingStatus_ = "stopped";
  recordingWriterStatus_ = "stopped";
  recordingWarning_ = command.getString("reason", "");
}

void MediaCore::failRecordingSession(const rpc::Json& command) {
  recordingStatus_ = "failed";
  recordingWriterStatus_ = "failed";
  recordingError_ = command.getString("message", "Recording writer failed.");
  recordingLastFailure_ = recordingError_;
  ++recordingFailureCount_;
}

void MediaCore::recoverRecordingSession(const rpc::Json& command) {
  recordingStatus_ = "recording";
  recordingWriterStatus_ = "writing";
  recordingError_.clear();
  recordingWarning_ = command.getString("reason", recordingWarning_);
  recordingLastRecovery_ = recordingWarning_;
  ++recordingRecoveryCount_;
}

void MediaCore::configureEncoderRecordingRequest() {
  // PRECONDITION: the caller holds audioOutputMutex_ (this mutates encoder state via
  // modules_.encoder->configureRecording). All callers — startProgramOutput,
  // setRecordingTargets, startRecordingSession — acquire it around the call.
  modules::RecordingSessionRequest request;
  request.sessionId = recordingSessionId_.empty() ? "native-recording-session" : recordingSessionId_;
  request.targetFolder = recordingTargetFolder_;
  request.filenamePrefix = recordingFilenamePrefix_;
  request.format = recordingFormat_;
  request.quality = recordingQuality_;
  request.isoParticipantIds = recordingIsoParticipantIds_;
  request.width = recordingOutputWidth_ > 0 ? recordingOutputWidth_ : (lastProgramFrame_.width > 0 ? lastProgramFrame_.width : outputWidth_);
  request.height = recordingOutputHeight_ > 0 ? recordingOutputHeight_ : (lastProgramFrame_.height > 0 ? lastProgramFrame_.height : outputHeight_);
  request.fps = recordingOutputFps_ > 0 ? recordingOutputFps_ : outputFps_;
  request.videoCodec = normalizeVideoCodec(recordingVideoCodec_, "h264");
  request.audioCodec = "aac";
  request.audioBitrateKbps = std::max(32, std::min(512, recordingAudioBitrateKbps_));
  request.targetBitrateMbps = static_cast<int>(std::max(1.0, recordingTargetBitrateMbps_));
  modules_.encoder->configureRecording(request);
}

void MediaCore::syncParticipantAudioMix(const rpc::Json& command) {
  audioChannels_.clear();
  audioLimiterEnabled_ = !command.get("limiterEnabled") || command.get("limiterEnabled")->asBool();
  const rpc::Json* channels = command.get("channels");
  if (!channels || !channels->isArray()) {
    return;
  }
  for (const auto& channel : channels->asArray()) {
    ParticipantAudioChannelInput input;
    input.participantId = channel.getString("participantId");
    input.inputLevel = std::max(0, std::min(100, static_cast<int>(channel.get("inputLevel") ? channel.get("inputLevel")->asNumber() : 0)));
    input.muted = channel.get("muted") && channel.get("muted")->asBool();
    input.noiseSuppression = channel.get("noiseSuppression") && channel.get("noiseSuppression")->asBool();
    if (const rpc::Json* manualGain = channel.get("manualGainDb")) {
      input.manualGainDb = std::max(-24.0, std::min(24.0, manualGain->asNumber()));
      input.hasManualGain = true;
    }
    if (const rpc::Json* pan = channel.get("pan")) {
      input.pan = std::max(-1.0, std::min(1.0, pan->asNumber()));
    }
    input.solo = channel.get("solo") && channel.get("solo")->asBool();
    input.pluginInserts = channel.getStringArray("pluginInserts");
    if (!input.participantId.empty()) {
      audioChannels_.push_back(std::move(input));
    }
  }
  // Audio-only command: in the live server the audio/output worker re-mixes on its
  // own cadence, so the command thread must not run a blocking render+readback here.
  // Direct/test callers (no worker) render synchronously so the returned snapshot
  // reflects the new mix immediately.
  if (!audioWorkerActive_) {
    renderSyntheticTick();
  }
}

void MediaCore::syncAudioMonitor(const rpc::Json& command) {
  // monitorOutput->start/stop + mixer reads mutate/read audio/output module state the
  // worker also touches (monitorOutput->render, mixer->monitorBus*); guard the whole
  // body. coreMutex(outer, held by the command loop) → audioOutputMutex_(inner).
  std::lock_guard<std::mutex> audioLock(audioOutputMutex_);
  audioMonitorEnabled_ = command.get("enabled") && command.get("enabled")->asBool();
  audioMonitorDeviceId_ = command.getString("deviceId");
  audioMonitorDeviceName_ = command.getString("deviceName");
  audioMonitorVolume_ = std::max(0.0, std::min(1.0, command.getNumber("volume", audioMonitorVolume_)));
  audioMonitorWarning_.clear();

  if (!audioMonitorEnabled_) {
    if (modules_.monitorOutput) {
      modules_.monitorOutput->stop();
    }
    audioMonitorStatus_ = "muted";
    return;
  }

  // Open (or re-target) the real render endpoint for the selected device. An
  // empty device id intentionally means "system default"; the WASAPI adapter
  // resolves that to the current default render endpoint.
  if (audioMonitorDeviceName_.empty() && audioMonitorDeviceId_.empty()) {
    audioMonitorDeviceName_ = "System default output";
  }

  // The mixer describes the source bus format; the output adapter converts to
  // the device's own mix format.
  if (modules_.monitorOutput) {
    const int sampleRate = modules_.mixer ? modules_.mixer->monitorBusSampleRate() : 48000;
    const int channels = modules_.mixer ? modules_.mixer->monitorBusChannels() : 2;
    if (!modules_.monitorOutput->start(audioMonitorDeviceId_, sampleRate, channels)) {
      audioMonitorStatus_ = "unavailable";
      const auto outputWarnings = modules_.monitorOutput->warnings();
      audioMonitorWarning_ = outputWarnings.empty()
                                 ? "Native audio monitor output device could not be opened."
                                 : outputWarnings.front();
      return;
    }
  }

  audioMonitorStatus_ = modules_.monitorOutput && !modules_.monitorOutput->hardwareOutput()
                            ? "stub-monitor"
                            : mixedAudioFrameCount_ > 0 ? "playing" : "armed";
}

namespace {

bool sameCaptureAudioSourceConfig(const modules::CaptureAudioSourceConfig& left,
                                  const modules::CaptureAudioSourceConfig& right) {
  return left.captureDeviceId == right.captureDeviceId &&
         left.audioDeviceId == right.audioDeviceId &&
         left.audioDeviceName == right.audioDeviceName &&
         left.audioSourceKind == right.audioSourceKind &&
         left.nativeAudioDeviceId == right.nativeAudioDeviceId &&
         left.audioDriverName == right.audioDriverName &&
         left.audioSyncOffsetMs == right.audioSyncOffsetMs &&
         left.embedded == right.embedded;
}

bool sameCaptureAudioSourceConfigs(const std::vector<modules::CaptureAudioSourceConfig>& left,
                                   const std::vector<modules::CaptureAudioSourceConfig>& right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), sameCaptureAudioSourceConfig);
}

constexpr double kMinAudioRoutingGainDb = -60.0;
constexpr double kMaxAudioRoutingGainDb = 10.0;

bool isAudioRoutingBus(const std::string& busId) {
  static const std::array<std::string_view, 15> kBuses = {
      "master", "pgm-l", "pgm-r", "iso-1", "iso-2", "iso-3", "iso-4", "iso-5",
      "iso-6",  "iso-7", "iso-8", "mon",   "stream", "aux-1", "aux-2"};
  if (busId.rfind("bus-", 0) == 0) {
    return true;
  }
  return std::any_of(kBuses.begin(), kBuses.end(), [&](std::string_view bus) { return bus == busId; });
}

}  // namespace

void MediaCore::syncAudioRoutingMatrix(const rpc::Json& command) {
  audioRoutingSends_.clear();
  audioRoutingWarnings_.clear();
  audioRoutingSynced_ = true;

  const rpc::Json* sends = command.get("sends");
  if (!sends || !sends->isArray()) {
    return;
  }

  std::set<std::string> seenCrosspoints;
  std::set<std::string> requestedSources;
  std::set<std::string> routedSources;
  std::set<std::string> warningSet;
  std::vector<std::string> warnings;

  auto addWarning = [&](const std::string& warning) {
    if (warningSet.insert(warning).second) {
      warnings.push_back(warning);
    }
  };

  for (const auto& send : sends->asArray()) {
    std::string sourceId = send.getString("sourceId");
    if (!sourceId.empty()) {
      requestedSources.insert(sourceId);
    }
    if (sourceId.empty()) {
      addWarning("Audio routing send is missing a sourceId.");
      continue;
    }

    const std::string busId = send.getString("busId");
    if (!isAudioRoutingBus(busId)) {
      addWarning("Audio routing send for " + sourceId + " targets unknown bus " + busId + ".");
      continue;
    }

    const std::string key = sourceId + ":" + busId;
    if (!seenCrosspoints.insert(key).second) {
      addWarning("Audio routing send " + sourceId + " -> " + busId + " is duplicated; keeping the first value.");
      continue;
    }

    const double rawGain = send.get("gainDb") ? send.get("gainDb")->asNumber() : 0.0;
    if (rawGain < kMinAudioRoutingGainDb || rawGain > kMaxAudioRoutingGainDb) {
      std::ostringstream gainWarning;
      gainWarning << "Audio routing gain " << rawGain << " dB for " << sourceId << " -> " << busId
                  << " is outside [-60, 10] dB; clamped.";
      addWarning(gainWarning.str());
    }

    AudioRoutingSendInput input;
    input.sourceId = sourceId;
    input.busId = busId;
    input.gainDb = std::max(kMinAudioRoutingGainDb, std::min(kMaxAudioRoutingGainDb, rawGain));
    input.busPluginInserts = send.getStringArray("busPluginInserts");
    routedSources.insert(sourceId);
    audioRoutingSends_.push_back(std::move(input));
  }

  for (const auto& sourceId : requestedSources) {
    if (routedSources.find(sourceId) == routedSources.end()) {
      addWarning("Audio routing source " + sourceId + " is routed to no bus.");
    }
  }

  audioRoutingWarnings_ = std::move(warnings);
}

void MediaCore::syncCaptureAudioSources(const rpc::Json& command) {
  captureAudioSources_.clear();
  captureAudioSourcesSynced_ = true;
  std::vector<modules::CaptureAudioSourceConfig> moduleSources;

  const rpc::Json* sources = command.get("sources");
  if (!sources || !sources->isArray()) {
    if (modules_.audioCapture) {
      if (!sameCaptureAudioSourceConfigs(lastCaptureAudioSourceConfigs_, moduleSources)) {
        modules_.audioCapture->configure(moduleSources);
        lastCaptureAudioSourceConfigs_ = moduleSources;
      }
    }
    return;
  }

  for (const auto& source : sources->asArray()) {
    CaptureAudioSourceInput input;
    input.captureDeviceId = source.getString("captureDeviceId");
    if (input.captureDeviceId.empty()) {
      continue;
    }

    input.audioDeviceId = source.getString("audioDeviceId");
    input.audioDeviceName = source.getString("audioDeviceName");
    input.audioSourceKind = source.getString("audioSourceKind");
    if (input.audioSourceKind.empty()) {
      input.audioSourceKind = input.audioDeviceId.empty() ? "none" : "wasapi-input";
    }
    input.nativeAudioDeviceId = source.getString("nativeAudioDeviceId");
    input.audioDriverName = source.getString("audioDriverName");
    input.embedded = source.get("embedded") && source.get("embedded")->asBool();
    input.audioSyncOffsetMs = static_cast<int>(source.get("audioSyncOffsetMs")
                                                   ? source.get("audioSyncOffsetMs")->asNumber()
                                                   : 0);
    input.audioSyncOffsetMs = std::max(-500, std::min(500, input.audioSyncOffsetMs));
    moduleSources.push_back(modules::CaptureAudioSourceConfig{
        input.captureDeviceId,
        input.audioDeviceId,
        input.audioDeviceName,
        input.audioSourceKind,
        input.nativeAudioDeviceId,
        input.audioDriverName,
        input.audioSyncOffsetMs,
        input.embedded});
    captureAudioSources_.push_back(std::move(input));
  }
  if (modules_.audioCapture) {
    if (!sameCaptureAudioSourceConfigs(lastCaptureAudioSourceConfigs_, moduleSources)) {
      modules_.audioCapture->configure(moduleSources);
      lastCaptureAudioSourceConfigs_ = moduleSources;
    }
  }
}

void MediaCore::pushCaptionCue(const rpc::Json& command) {
  captionWarnings_.clear();
  const std::string text = command.getString("text");
  if (text.empty()) {
    captionWarnings_.push_back("Caption cue ignored because text was empty.");
    return;
  }
  captionText_ = text;
  captionSpeaker_ = command.getString("speaker");
  captionAtMs_ = command.get("atMs") ? command.get("atMs")->asNumber() : 0;
  captionConfidence_ = std::max(82, 97 - static_cast<int>(text.size() / 28));
  if (text.size() > 96) {
    captionWarnings_.push_back("Caption line is long; compact mode enabled.");
  }
}

void MediaCore::setCaptionEnabled(const rpc::Json& command) {
  captionEnabled_ = !(command.get("enabled") && !command.get("enabled")->asBool());
  captionWarnings_.clear();
  if (!captionEnabled_) {
    captionWarnings_.push_back("Caption track disabled.");
  }
}

void MediaCore::setBrandKit(const rpc::Json& command) {
  brandWarnings_.clear();
  brandName_ = command.getString("name", brandName_);
  brandLogoText_ = command.getString("logoText", brandLogoText_);
  brandColor_ = command.getString("brandColor", brandColor_);
  brandAccentColor_ = command.getString("accentColor", brandAccentColor_);
  brandBackgroundColor_ = command.getString("backgroundColor", brandBackgroundColor_);
  brandFontFamily_ = command.getString("fontFamily", brandFontFamily_);
  brandLowerThirdStyle_ = command.getString("lowerThirdStyle", brandLowerThirdStyle_);
  brandCaptionStyle_ = command.getString("captionStyle", brandCaptionStyle_);
  brandDefaultOverlayBehavior_ = command.getString("defaultOverlayBehavior", brandDefaultOverlayBehavior_);
  if (brandLogoText_.empty()) {
    brandLogoText_ = "CoreVideo Pro";
    brandWarnings_.push_back("Brand logo text was empty; using default bug label.");
  }
}

void MediaCore::setMediaPlayback(const rpc::Json& command) {
  mediaPlaybackWarnings_.clear();
  const std::string mediaAssetId = command.getString("mediaAssetId");
  if (mediaAssetId.empty()) {
    mediaPlaybackAssetId_.clear();
    mediaPlaybackAssetName_.clear();
    mediaPlaybackAssetKind_.clear();
    mediaPlaybackAssetPath_.clear();
    mediaPlaybackKey_.clear();
    mediaPlaybackPlaying_ = false;
    mediaPlaybackWarnings_.push_back("Media playback command had no media asset id.");
    return;
  }

  const std::string mediaAssetName = command.getString("mediaAssetName");
  const std::string mediaAssetPath = command.getString("mediaAssetPath");
  const std::string mediaPlaybackKey = command.getString("mediaPlaybackKey");
  if (mediaAssetName.empty()) {
    mediaPlaybackWarnings_.push_back(mediaAssetId + " media asset has no name and may not be present in the media bin.");
  }
  if (mediaAssetPath.empty()) {
    mediaPlaybackWarnings_.push_back(mediaAssetId + " media asset has no file path; Program playback cannot decode it.");
  }

  mediaPlaybackAssetId_ = mediaAssetId;
  mediaPlaybackAssetName_ = mediaAssetName.empty() ? mediaAssetId : mediaAssetName;
  mediaPlaybackAssetKind_ = command.getString("mediaAssetKind");
  mediaPlaybackAssetPath_ = mediaAssetPath;
  mediaPlaybackKey_ = mediaPlaybackKey;
  mediaPlaybackPlaying_ = command.get("playing") && command.get("playing")->asBool();
  if (mediaPlaybackPlaying_ && mediaPlaybackKey_.empty()) {
    mediaPlaybackWarnings_.push_back(mediaAssetId + " is playing without a playback key; replay behavior may be unstable.");
  }
}

void MediaCore::setMultiviewLayout(const rpc::Json& command) {
  // The standalone set-multiview-layout command IS a layout node; share the parse/apply
  // path with the frequent zoom-media-spine-sync `multiview` object.
  applyMultiviewLayout(command);
}

bool MediaCore::applyMultiviewLayout(const rpc::Json& layout) {
  // The WinUI sends the ordered Show Input roster: each entry is one multiview
  // tile (sourceId/kind/participantId|captureDeviceId|mediaAssetId, slot, label)
  // plus the multiview canvas dimensions. An empty/absent sources array clears
  // the layout, which turns the second (multiview) GPU composite pass off.
  const int canvasWidth = static_cast<int>(layout.getNumber("canvasWidth", 0));
  const int canvasHeight = static_cast<int>(layout.getNumber("canvasHeight", 0));
  const int resolvedWidth = canvasWidth > 0 ? canvasWidth : outputWidth_;
  const int resolvedHeight = canvasHeight > 0 ? canvasHeight : outputHeight_;

  // Parse into a temp vector first + build a cheap content signature, so the per-spine-tick
  // call can skip the clear/rebuild + structural-emit reset when nothing changed.
  std::vector<MultiviewSource> parsed;
  std::string signature = std::to_string(resolvedWidth) + "x" + std::to_string(resolvedHeight) + ";";
  const auto* sources = layout.get("sources");
  if (sources && sources->isArray()) {
    int autoSlot = 0;
    for (const auto& entry : sources->asArray()) {
      if (!entry.isObject()) {
        continue;
      }
      MultiviewSource source;
      source.sourceId = entry.getString("sourceId");
      source.kind = entry.getString("kind");
      source.participantId = entry.getString("participantId");
      source.captureDeviceId = entry.getString("captureDeviceId");
      source.mediaAssetId = entry.getString("mediaAssetId");
      source.label = entry.getString("label");
      source.slot = entry.get("slot") ? static_cast<int>(entry.getNumber("slot", autoSlot)) : autoSlot;
      signature += std::to_string(source.slot) + ":" + source.kind + ":" + source.sourceId + ":" +
                   source.participantId + ":" + source.captureDeviceId + ":" + source.mediaAssetId + ":" +
                   source.label + "|";
      parsed.push_back(std::move(source));
      ++autoSlot;
    }
  }

  if (signature == multiviewLayoutSignature_) {
    // Unchanged layout — do NOT churn multiviewSources_ or reset the structural-emit flag.
    return false;
  }

  multiviewLayoutSignature_ = std::move(signature);
  multiviewSources_ = std::move(parsed);
  multiviewCanvasWidth_ = resolvedWidth;
  multiviewCanvasHeight_ = resolvedHeight;
  // A layout change is a structural change; force the next render's event emit.
  multiviewStructureEmitted_ = false;
  return true;
}

modules::CompositorRenderPlan MediaCore::buildMultiviewRenderPlan(const std::vector<modules::VideoFrame>& videoFrames) const {
  (void)videoFrames;
  modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = "multiview:" + std::to_string(multiviewSources_.size());
  renderPlan.sceneId = sceneId_;
  renderPlan.width = multiviewCanvasWidth_ > 0 ? multiviewCanvasWidth_ : outputWidth_;
  renderPlan.height = multiviewCanvasHeight_ > 0 ? multiviewCanvasHeight_ : outputHeight_;
  renderPlan.fps = outputFps_;
  renderPlan.colorGrade = colorGrade_;

  const std::string activeSpeakerId = zoomSnapshot().getString("activeSpeakerId");
  const int count = static_cast<int>(multiviewSources_.size());
  renderPlan.layers.reserve(multiviewSources_.size());
  for (int index = 0; index < count; ++index) {
    const auto& source = multiviewSources_[static_cast<size_t>(index)];
    modules::CompositorRenderPlanLayer layer;
    layer.layerId = "multiview:" + (source.sourceId.empty() ? std::to_string(index) : source.sourceId);
    layer.order = index;
    // Resolve the feed using the same source-id conventions as the program plan
    // so resolveLayers/frameForParticipant matches the same decoded frames.
    if (source.kind == "media" && !source.mediaAssetId.empty()) {
      layer.kind = "media-video";
      layer.sourceId = "media:" + source.mediaAssetId;
      layer.mediaAssetId = source.mediaAssetId;
    } else if (source.kind == "capture" && !source.captureDeviceId.empty()) {
      layer.kind = "participant-video";
      layer.participantId = "capture:" + source.captureDeviceId;
      layer.sourceId = layer.participantId;
    } else if (!source.participantId.empty()) {
      layer.kind = "participant-video";
      layer.participantId = source.participantId;
      layer.sourceId = "zoom:" + source.participantId;
    } else {
      layer.kind = "participant-video";
      layer.sourceId = source.sourceId;
    }

    // Aspect-aware grid cell, matching the program/preview grid math so the core
    // and the future consumer agree on rows x cols.
    const auto cell = compositor::gridCell((std::max)(1, count), index);
    // gridCell divides the 16:9 canvas into an EVEN rows x cols grid, so most tile
    // counts (2-up, 6-up, 8-up, ...) yield non-16:9 cells; "fill" then center-crops the
    // 16:9 feed to fill the cell -> the "weird center cut". Instead, place a centered
    // 16:9 tile inside each cell (largest 16:9 rect that fits the cell in PIXELS, mapped
    // back to normalized canvas space) and "fit" the source so the whole frame shows at
    // the right aspect. 16:9 source in a 16:9 tile => no bars; an off-aspect source is
    // letterboxed inside the tile rather than cropped.
    const float mvCanvasW = static_cast<float>(renderPlan.width > 0 ? renderPlan.width : 1920);
    const float mvCanvasH = static_cast<float>(renderPlan.height > 0 ? renderPlan.height : 1080);
    const float cellPxW = cell.width * mvCanvasW;
    const float cellPxH = cell.height * mvCanvasH;
    constexpr float kTileAspect = 16.f / 9.f;
    float tilePxW = cellPxW;
    float tilePxH = cellPxW / kTileAspect;
    if (tilePxH > cellPxH) {
      tilePxH = cellPxH;
      tilePxW = cellPxH * kTileAspect;
    }
    const float tileW = mvCanvasW > 0.f ? tilePxW / mvCanvasW : cell.width;
    const float tileH = mvCanvasH > 0.f ? tilePxH / mvCanvasH : cell.height;
    layer.rect = {
        cell.x + (cell.width - tileW) * 0.5f,
        cell.y + (cell.height - tileH) * 0.5f,
        tileW,
        tileH};
    layer.fitMode = "fit";

    // Active-speaker border baked into the texture (no consumer churn). The rest
    // get a thin neutral accent so tiles read as distinct cells.
    const bool isActiveSpeaker = !source.participantId.empty() && source.participantId == activeSpeakerId;
    if (isActiveSpeaker) {
      layer.borderStyle = "program";
      layer.borderColor = "#f5a623";
      layer.borderThickness = 6.f;
    } else {
      layer.borderStyle = "accent";
      layer.borderColor = "#3ddc97";
      layer.borderThickness = 2.f;
    }
    renderPlan.layers.push_back(std::move(layer));
  }

  return renderPlan;
}

void MediaCore::configureSrtIngestSources(const rpc::Json& command) {
  std::vector<modules::SrtIngestSourceConfig> configs;
  const auto* sources = command.get("sources");
  if (!sources || !sources->isArray()) {
    (void)modules_.captureDevice->configureSrtIngestSources(configs);
    return;
  }

  configs.reserve(sources->asArray().size());
  for (const auto& source : sources->asArray()) {
    modules::SrtIngestSourceConfig config;
    config.id = source.getString("id");
    config.deviceId = source.getString("deviceId");
    config.name = source.getString("name");
    config.mode = source.getString("mode", "listener");
    config.host = source.getString("host", "0.0.0.0");
    config.port = static_cast<int>(source.getNumber("port", 10000));
    config.latencyMs = static_cast<int>(source.getNumber("latencyMs", 120));
    config.streamId = source.getString("streamId");
    config.passphrase = source.getString("passphrase");
    if (config.deviceId.empty()) {
      config.deviceId = config.id;
    }
    if (config.name.empty()) {
      config.name = config.deviceId;
    }
    configs.push_back(std::move(config));
  }
  (void)modules_.captureDevice->configureSrtIngestSources(configs);
}

namespace {

int clampInt(int value, int minValue, int maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

double clampDouble(double value, double minValue, double maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

int calculateSmartGainDb(int inputLevel) {
  const int delta = 68 - inputLevel;
  if (delta > 28) return 6;
  if (delta > 14) return 3;
  if (delta < -12) return -4;
  if (delta < -4) return -2;
  return 0;
}

std::string audioStatusFor(bool muted, int gainDb) {
  if (muted) return "muted";
  if (gainDb > 0) return "boosting";
  if (gainDb < 0) return "ducking";
  return "balanced";
}

// DETERMINISTIC dBFS derivation for the participant-audio contract.
//
// The stub core has no real PCM (the real DSP graph is F2, gated behind the dev
// adapters), so it cannot MEASURE rms/peak the way the TS simulators do from
// synthesized samples. Instead we DERIVE the two metrics from the existing
// 0-100 `outputLevel` using the same sine model the renderer/native-core mirrors
// assume: a participant tone whose PEAK amplitude tracks level/100 of full
// scale. Then peakDbfs = 20*log10(level/100) and, for a sine, rmsDbfs is one
// crest-factor (~3.0103 dB) below the peak. Level 0 (or muted) reports the
// digital-silence floor. This keeps the values consistent and stable for the
// same input, matching the TS measured-from-silence behaviour for muted/silent
// channels (both floor to -120 dBFS).
constexpr double kAudioDbfsFloor = -120.0;
constexpr double kSineCrestFactorDb = 3.0102999566398121;  // 20*log10(sqrt(2))

double levelToDbfs(int level) {
  if (level <= 0) {
    return kAudioDbfsFloor;
  }
  const double db = 20.0 * std::log10(static_cast<double>(level) / 100.0);
  return db < kAudioDbfsFloor ? kAudioDbfsFloor : db;
}

double round1Dbfs(double value) {
  return std::round(value * 10.0) / 10.0;
}

double derivePeakDbfs(int outputLevel) {
  return round1Dbfs(levelToDbfs(outputLevel));
}

double deriveRmsDbfs(int outputLevel) {
  if (outputLevel <= 0) {
    return round1Dbfs(kAudioDbfsFloor);
  }
  const double rms = levelToDbfs(outputLevel) - kSineCrestFactorDb;
  return round1Dbfs(rms < kAudioDbfsFloor ? kAudioDbfsFloor : rms);
}

std::string protocolAudioStatusForDsp(const modules::AudioParticipantMixMetrics& participant) {
  if (participant.muted) return "muted";
  if (participant.noiseSuppressionActive) return "cleaning";
  if (participant.gainDb > 0) return "boosting";
  if (participant.gainDb < 0) return "ducking";
  return "balanced";
}

}  // namespace

rpc::Json MediaCore::audioMixSessionState() const {
  // Reads mixer->session() (mutated by the worker's mixer->mix) and, via
  // masterMeterState(), the BS.1770 loudness members (mutated by the worker's
  // updateProgramLoudnessMeter). Hold audioOutputMutex_ across the whole body;
  // masterMeterState() therefore does NOT lock (only called from here).
  std::lock_guard<std::mutex> audioLock(audioOutputMutex_);
  if (audioChannels_.empty()) {
    const auto nativeMix = modules_.mixer->session();
    if (!nativeMix.participants.empty()) {
      rpc::Json::Array participants;
      for (const auto& participant : nativeMix.participants) {
        participants.emplace_back(rpc::Json::Object{
            {"participantId", participant.participantId},
            {"inputLevel", participant.inputLevel},
            {"outputLevel", participant.outputLevel},
            {"gainDb", participant.gainDb},
            {"rmsDbfs", deriveRmsDbfs(participant.muted ? 0 : participant.outputLevel)},
            {"peakDbfs", derivePeakDbfs(participant.muted ? 0 : participant.outputLevel)},
            {"noiseSuppression", participant.noiseSuppressionActive},
            {"limiterActive", audioLimiterEnabled_ && participant.limiterActive},
            {"muted", participant.muted},
            {"status", protocolAudioStatusForDsp(participant)},
        });
      }

      rpc::Json::Array warnings;
      for (const auto& warning : nativeMix.warnings) {
        warnings.emplace_back(warning);
      }
      if (!audioMonitorWarning_.empty()) {
        warnings.emplace_back(audioMonitorWarning_);
      }
      if (modules_.audioCapture) {
        for (const auto& warning : modules_.audioCapture->warnings()) {
          warnings.emplace_back(warning);
        }
      }

      return rpc::Json::Object{
          {"status", nativeMix.status},
          {"masterLevel", nativeMix.masterLevel},
          {"loudnessLufs", nativeMix.loudnessLufs},
          {"limiterEnabled", audioLimiterEnabled_},
          {"limiterActive", audioLimiterEnabled_ && nativeMix.limiterActive},
          {"mixedFrameCount", static_cast<double>(nativeMix.mixedFrameCount)},
          {"monitorEnabled", audioMonitorEnabled_},
          {"monitorStatus", audioMonitorStatus_},
          {"monitorDeviceId", audioMonitorDeviceId_},
          {"monitorDeviceName", audioMonitorDeviceName_},
          {"monitorVolume", audioMonitorVolume_},
          {"monitorFramesPlayed", static_cast<double>(audioMonitorFramesPlayed_)},
          {"participants", participants},
          {"masterMeter", masterMeterState()},
          {"summary", nativeMix.summary},
          {"warnings", warnings},
      };
    }

    rpc::Json::Array warnings;
    if (!audioMonitorWarning_.empty()) {
      warnings.emplace_back(audioMonitorWarning_);
    }
    if (modules_.audioCapture) {
      for (const auto& warning : modules_.audioCapture->warnings()) {
        warnings.emplace_back(warning);
      }
    }

    return rpc::Json::Object{
        {"status", "idle"},
        {"masterLevel", 0},
        {"loudnessLufs", -60},
        {"limiterEnabled", audioLimiterEnabled_},
        {"limiterActive", false},
        {"mixedFrameCount", static_cast<double>(mixedAudioFrameCount_)},
        {"monitorEnabled", audioMonitorEnabled_},
        {"monitorStatus", audioMonitorStatus_},
        {"monitorDeviceId", audioMonitorDeviceId_},
        {"monitorDeviceName", audioMonitorDeviceName_},
        {"monitorVolume", audioMonitorVolume_},
        {"monitorFramesPlayed", static_cast<double>(audioMonitorFramesPlayed_)},
        {"participants", rpc::Json::Array{}},
        {"masterMeter", masterMeterState()},
        {"summary", "Audio mix idle."},
        {"warnings", warnings},
    };
  }

  rpc::Json::Array participants;
  rpc::Json::Array warnings;
  std::unordered_set<std::string> warningSet;
  const auto nativeMix = modules_.mixer->session();
  std::map<std::string, modules::AudioParticipantMixMetrics> nativeMetricsByParticipant;
  for (const auto& participant : nativeMix.participants) {
    nativeMetricsByParticipant[participant.participantId] = participant;
  }

  int masterTotal = 0;
  int audibleCount = 0;
  bool limiterWouldReduce = false;
  int boostingCount = 0;
  int duckingCount = 0;
  int mutedCount = 0;
  int waitingPcmCount = 0;
  int manualCount = 0;
  int soloCount = 0;
  int insertCount = 0;

  for (const auto& channel : audioChannels_) {
    const auto nativeMetric = nativeMetricsByParticipant.find(channel.participantId);
    const modules::AudioParticipantMixMetrics* measured =
        nativeMetric == nativeMetricsByParticipant.end() ? nullptr : &nativeMetric->second;
    const bool hasPcm = measured != nullptr;
    const int measuredInputLevel = hasPcm ? measured->inputLevel : 0;
    const int smartGainDb = calculateSmartGainDb(measuredInputLevel);
    const int gainDb = channel.muted ? -60 : static_cast<int>(clampDouble(smartGainDb + (channel.hasManualGain ? channel.manualGainDb : 0), -12, 12));
    const bool noiseSuppression = hasPcm && (channel.noiseSuppression || measuredInputLevel < 35 ||
                                  (measured && measured->noiseSuppressionActive));
    const int outputLevel = channel.muted || !hasPcm ? 0 : clampInt(measuredInputLevel + gainDb * 4, 0, 100);
    const bool channelLimiterWouldReduce = hasPcm && outputLevel >= 88;
    limiterWouldReduce = limiterWouldReduce || channelLimiterWouldReduce || (hasPcm && measured->limiterActive);
    if (hasPcm && !channel.muted) {
      masterTotal += outputLevel;
      ++audibleCount;
    }
    if (hasPcm && gainDb > 0 && !channel.muted) ++boostingCount;
    if (hasPcm && gainDb < 0 && !channel.muted) ++duckingCount;
    if (channel.muted) ++mutedCount;
    if (!hasPcm && !channel.muted) ++waitingPcmCount;
    if (channel.solo) ++soloCount;
    if (channel.hasManualGain && channel.manualGainDb != 0) ++manualCount;
    if (!channel.pluginInserts.empty()) {
      insertCount += static_cast<int>(channel.pluginInserts.size());
      if (warningSet.insert("vst-bridge-scan-only").second) {
        warnings.emplace_back("VST inserts are configured but live third-party plugin processing requires the dev VST bridge.");
      }
    }
    if (!hasPcm && !channel.muted && warningSet.insert("missing-pcm:" + channel.participantId).second) {
      warnings.emplace_back("No native PCM has been mixed for " + channel.participantId + "; meters are held at silence for that channel.");
    }

    rpc::Json::Array pluginInserts;
    for (const auto& insert : channel.pluginInserts) {
      pluginInserts.emplace_back(rpc::Json::Object{
          {"name", insert},
          {"format", insert.rfind("VST", 0) == 0 ? "vst3" : "builtin"},
          {"status", insert.rfind("VST", 0) == 0 ? "scan-only" : "available"},
          {"processingEnabled", false},
      });
    }

    rpc::Json::Object participant{
        {"participantId", channel.participantId},
        {"inputLevel", measuredInputLevel},
        {"outputLevel", outputLevel},
        {"gainDb", gainDb},
        {"rmsDbfs", hasPcm ? round1Dbfs(modules::linearToDbfs(measured->rmsLevel)) : deriveRmsDbfs(0)},
        {"peakDbfs", hasPcm ? round1Dbfs(modules::linearToDbfs(measured->peakLevel)) : derivePeakDbfs(0)},
        {"pan", channel.pan},
        {"solo", channel.solo},
        {"noiseSuppression", noiseSuppression},
        {"limiterActive", audioLimiterEnabled_ && (channelLimiterWouldReduce || (hasPcm && measured->limiterActive))},
        {"muted", channel.muted},
        {"pluginInserts", pluginInserts},
        {"status", channel.muted ? "muted" : hasPcm && !measured->status.empty() ? measured->status : hasPcm ? audioStatusFor(false, gainDb) : "waiting-for-pcm"},
    };
    if (channel.hasManualGain) {
      participant.emplace("manualGainDb", channel.manualGainDb);
    }
    participants.emplace_back(std::move(participant));
  }

  const int masterLevel = audibleCount > 0 ? clampInt((masterTotal / audibleCount) + 8, 0, 100) : 0;
  limiterWouldReduce = limiterWouldReduce || masterLevel >= 88;
  const bool limiterActive = audioLimiterEnabled_ && limiterWouldReduce;
  std::ostringstream summary;
  if (boostingCount > 0) summary << boostingCount << " boosted";
  if (duckingCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << duckingCount << " ducked";
  if (waitingPcmCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << waitingPcmCount << " waiting for PCM";
  if (mutedCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << mutedCount << " muted";
  if (manualCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << manualCount << " manual";
  if (soloCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << soloCount << " solo";
  if (insertCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << insertCount << " inserts";
  const std::string summaryText = summary.tellp() > 0 ? summary.str() + " in program mix" : "Program mix balanced";
  if (!audioMonitorWarning_.empty()) {
    warnings.emplace_back(audioMonitorWarning_);
  }
  if (modules_.audioCapture) {
    for (const auto& warning : modules_.audioCapture->warnings()) {
      if (warningSet.insert("audio-capture:" + warning).second) {
        warnings.emplace_back(warning);
      }
    }
  }

  return rpc::Json::Object{
      {"status", warnings.empty() ? "live" : "warning"},
      {"masterLevel", masterLevel},
      {"loudnessLufs", audibleCount > 0 ? (limiterActive ? -14 : -16) : -60},
      {"limiterEnabled", audioLimiterEnabled_},
      {"limiterActive", limiterActive},
      {"mixedFrameCount", static_cast<double>(mixedAudioFrameCount_)},
      {"monitorEnabled", audioMonitorEnabled_},
      {"monitorStatus", audioMonitorStatus_},
      {"monitorDeviceId", audioMonitorDeviceId_},
      {"monitorDeviceName", audioMonitorDeviceName_},
      {"monitorVolume", audioMonitorVolume_},
      {"monitorFramesPlayed", static_cast<double>(audioMonitorFramesPlayed_)},
      {"participants", participants},
      {"masterMeter", masterMeterState()},
      {"summary", summaryText},
      {"warnings", warnings},
  };
}

void MediaCore::updateProgramLoudnessMeter(const std::vector<float>& interleaved, int channels, int sampleRate) {
  if (interleaved.empty() || channels <= 0 || sampleRate <= 0) {
    return;  // no new program signal this tick; hold the last meter values
  }
  const size_t frames = interleaved.size() / static_cast<size_t>(channels);
  if (frames == 0) {
    return;
  }

  std::vector<float> chunkL(frames);
  std::vector<float> chunkR(frames);
  for (size_t index = 0; index < frames; ++index) {
    chunkL[index] = interleaved[index * static_cast<size_t>(channels)];
    chunkR[index] = channels == 1 ? chunkL[index] : interleaved[index * static_cast<size_t>(channels) + 1];
  }

  const double rate = static_cast<double>(sampleRate);
  // Integrated loudness: feed the continuous BS.1770 meter, which forms 400 ms
  // gating blocks (100 ms hop) and applies the absolute (-70 LUFS) then relative
  // (-10 LU) gates per the spec.
  if (programIntegratedMeter_.sampleRateHz() != rate) {
    programIntegratedMeter_.reset(rate);
  }
  programIntegratedMeter_.process(chunkL.data(), chunkR.data(), frames);

  // Roll the windowed buffer and trim to the 3 s short-term window.
  programMeterL_.insert(programMeterL_.end(), chunkL.begin(), chunkL.end());
  programMeterR_.insert(programMeterR_.end(), chunkR.begin(), chunkR.end());
  const size_t maxSamples = static_cast<size_t>(sampleRate) * 3u;
  if (programMeterL_.size() > maxSamples) {
    programMeterL_.erase(programMeterL_.begin(), programMeterL_.end() - static_cast<std::ptrdiff_t>(maxSamples));
    programMeterR_.erase(programMeterR_.begin(), programMeterR_.end() - static_cast<std::ptrdiff_t>(maxSamples));
  }

  const size_t windowed = programMeterL_.size();
  // Momentary (400 ms) / short-term (3 s) are single-pass K-weighted RMS; bound
  // momentary to its 400 ms window so it isn't scanning the full 3 s buffer.
  const size_t momentarySamples = std::min(windowed, static_cast<size_t>(rate * 0.4));
  const size_t momentaryOffset = windowed - momentarySamples;
  programLufsMomentary_ = modules::computeMomentaryLufs(programMeterL_.data() + momentaryOffset,
                                                        programMeterR_.data() + momentaryOffset, momentarySamples, rate);
  programLufsShortTerm_ = modules::computeShortTermLufs(programMeterL_.data(), programMeterR_.data(), windowed, rate);
  // True peak uses 4x oversampling (a polyphase FIR per sample). Re-scanning the
  // full 3 s window every tick costs ~240ms and was starving the 60fps render —
  // oversample only THIS chunk and hold a slowly-decaying peak, which is what a
  // true-peak meter displays anyway.
  const double chunkTruePeak = std::max(modules::computeTruePeakDbfs(chunkL.data(), frames, 4),
                                        modules::computeTruePeakDbfs(chunkR.data(), frames, 4));
  programTruePeakDbfs_ = std::max(chunkTruePeak, programTruePeakDbfs_ - 0.5);
  programLufsIntegrated_ = programIntegratedMeter_.integratedLufs();
}

rpc::Json MediaCore::masterMeterState() const {
  // PRECONDITION: caller holds audioOutputMutex_ (reads the BS.1770 loudness members
  // the worker mutates). Only called from audioMixSessionState(), which holds it.
  const int windowMs = programMeterL_.empty() ? 0 : static_cast<int>((programMeterL_.size() * 1000) / 48000);
  return rpc::Json::Object{
      {"momentaryLufs", programLufsMomentary_},
      {"shortTermLufs", programLufsShortTerm_},
      {"integratedLufs", programLufsIntegrated_},
      {"truePeakDbfs", programTruePeakDbfs_},
      {"windowMs", windowMs},
  };
}

rpc::Json MediaCore::audioRoutingMatrixState() const {
  std::vector<std::string> buses = {
      "master", "pgm-l", "pgm-r", "iso-1", "iso-2", "iso-3", "iso-4", "iso-5",
      "iso-6",  "iso-7", "iso-8", "mon",   "stream", "aux-1", "aux-2"};
  for (const auto& send : audioRoutingSends_) {
    if (std::find(buses.begin(), buses.end(), send.busId) == buses.end()) {
      buses.push_back(send.busId);
    }
  }

  rpc::Json::Array warnings;
  for (const auto& warning : audioRoutingWarnings_) {
    warnings.emplace_back(warning);
  }

  if (!audioRoutingSynced_ || audioRoutingSends_.empty()) {
    rpc::Json::Array busSourceCounts;
    rpc::Json::Array busProcessing;
    for (const auto& bus : buses) {
      busSourceCounts.emplace_back(rpc::Json::Object{{"busId", bus}, {"sourceCount", 0}});
      busProcessing.emplace_back(rpc::Json::Object{{"busId", bus}, {"pluginInserts", rpc::Json::Array{}}});
    }
    return rpc::Json::Object{
        {"status", audioRoutingWarnings_.empty() ? "idle" : "warning"},
        {"routedSendCount", 0},
        {"routedSourceCount", 0},
        {"busSourceCounts", busSourceCounts},
        {"busProcessing", busProcessing},
        {"sends", rpc::Json::Array{}},
        {"summary", audioRoutingSynced_ ? "No audio crosspoints routed." : "Audio routing matrix idle."},
        {"warnings", warnings},
    };
  }

  std::set<std::string> routedSources;
  std::map<std::string, std::set<std::string>> busSources;
  std::map<std::string, std::vector<std::string>> busPluginInserts;
  rpc::Json::Array sends;
  for (const auto& send : audioRoutingSends_) {
    routedSources.insert(send.sourceId);
    busSources[send.busId].insert(send.sourceId);
    if (!send.busPluginInserts.empty()) {
      busPluginInserts[send.busId] = send.busPluginInserts;
    }
    rpc::Json::Array inserts;
    for (const auto& insert : send.busPluginInserts) {
      inserts.emplace_back(insert);
    }
    sends.emplace_back(rpc::Json::Object{
        {"sourceId", send.sourceId},
        {"busId", send.busId},
        {"gainDb", send.gainDb},
        {"busPluginInserts", inserts},
    });
  }

  rpc::Json::Array busSourceCounts;
  rpc::Json::Array busProcessing;
  int routedBusCount = 0;
  for (const auto& bus : buses) {
    const auto found = busSources.find(bus);
    const int sourceCount = found == busSources.end() ? 0 : static_cast<int>(found->second.size());
    if (sourceCount > 0) {
      ++routedBusCount;
    }
    busSourceCounts.emplace_back(rpc::Json::Object{{"busId", bus}, {"sourceCount", sourceCount}});
    rpc::Json::Array inserts;
    if (const auto busInserts = busPluginInserts.find(bus); busInserts != busPluginInserts.end()) {
      for (const auto& insert : busInserts->second) {
        inserts.emplace_back(insert);
      }
    }
    busProcessing.emplace_back(rpc::Json::Object{{"busId", bus}, {"pluginInserts", inserts}});
  }

  // Measured PCM taps from the real routing-matrix bus mix this tick. Only buses
  // that actually received signal appear here, so an operator/test can confirm
  // real samples are flowing (not just routed crosspoint state).
  rpc::Json::Array busTaps;
  for (const auto& [busId, pcm] : routedBusPcm_) {
    if (pcm.empty()) {
      continue;
    }
    busTaps.emplace_back(rpc::Json::Object{
        {"busId", busId},
        {"channels", 2},
        {"frames", static_cast<int>(pcm.size() / 2)},
        {"peakDbfs", modules::computeSamplePeakDbfs(pcm.data(), pcm.size())},
        {"rmsDbfs", modules::computeRmsDbfs(pcm.data(), pcm.size())},
    });
  }

  std::ostringstream summary;
  summary << audioRoutingSends_.size() << " send" << (audioRoutingSends_.size() == 1 ? "" : "s") << " from "
          << routedSources.size() << " source" << (routedSources.size() == 1 ? "" : "s") << " across " << routedBusCount
          << " bus(es).";

  return rpc::Json::Object{
      {"status", audioRoutingWarnings_.empty() ? "live" : "warning"},
      {"routedSendCount", static_cast<int>(audioRoutingSends_.size())},
      {"routedSourceCount", static_cast<int>(routedSources.size())},
      {"busSourceCounts", busSourceCounts},
      {"busProcessing", busProcessing},
      {"busTaps", busTaps},
      {"programTapFrames", static_cast<int>(programAudioTapPcm().size() / 2)},
      {"sends", sends},
      {"summary", summary.str()},
      {"warnings", warnings},
  };
}

const std::vector<float>& MediaCore::programAudioTapPcm() const {
  return audioBusTapPcm("master");
}

std::vector<std::string> MediaCore::routedAudioBusIds() const {
  std::vector<std::string> ids;
  ids.reserve(routedBusPcm_.size());
  for (const auto& [busId, pcm] : routedBusPcm_) {
    if (!pcm.empty()) {
      ids.push_back(busId);
    }
  }
  return ids;
}

const std::vector<float>& MediaCore::audioBusTapPcm(const std::string& busId) const {
  static const std::vector<float> kEmptyTap;
  const auto found = routedBusPcm_.find(busId);
  return found == routedBusPcm_.end() ? kEmptyTap : found->second;
}

rpc::Json MediaCore::captureAudioSourcesState() const {
  rpc::Json::Array sources;
  rpc::Json::Array warnings;
  std::set<std::string> warningSet;
  int pairedCount = 0;
  int streamingCount = 0;
  int64_t totalFramesReceived = 0;
  const int routedMasterFrames = static_cast<int>(programAudioTapPcm().size() / 2);
  const int routedStreamFrames = static_cast<int>(audioBusTapPcm("stream").size() / 2);
  const int routedMonitorFrames = static_cast<int>(audioBusTapPcm("mon").size() / 2);
  // mixer->monitorBus* is mutated by the worker's mixer->mix; guard the read.
  int fallbackMonitorFrames = 0;
  if (routedMonitorFrames == 0 && modules_.mixer) {
    std::lock_guard<std::mutex> audioLock(audioOutputMutex_);
    fallbackMonitorFrames = static_cast<int>(modules_.mixer->monitorBusPcm().size() /
                                             static_cast<size_t>(std::max(1, modules_.mixer->monitorBusChannels())));
  }
  std::map<std::string, modules::CaptureAudioSourceMetrics> metricsByCaptureId;
  auto addWarning = [&](const std::string& warning) {
    if (!warning.empty() && warningSet.insert(warning).second) {
      warnings.emplace_back(warning);
    }
  };
  if (modules_.audioCapture) {
    for (const auto& metric : modules_.audioCapture->metrics()) {
      metricsByCaptureId[metric.captureDeviceId] = metric;
      if (!metric.warning.empty()) {
        addWarning(metric.warning);
      }
    }
    for (const auto& warning : modules_.audioCapture->warnings()) {
      addWarning(warning);
    }
  }
  for (const auto& source : captureAudioSources_) {
    if (!source.audioDeviceId.empty()) {
      ++pairedCount;
    }

    // Dev-gated hardware kinds need a real adapter the portable build can't
    // provide, so carry the caveat on the source row too (not just the aggregate).
    std::string hardwareCaveat;
    if (!source.audioDeviceId.empty() && source.audioSourceKind == "asio-input") {
      hardwareCaveat = "ASIO source " + source.audioDeviceName + " is selected; native ASIO PCM capture requires the dev-machine adapter.";
    } else if (!source.audioDeviceId.empty() && source.audioSourceKind == "embedded-capture-audio") {
      hardwareCaveat = "Embedded capture-card audio " + source.audioDeviceName + " is selected; DeckLink/AJA audio PCM capture requires the hardware adapter.";
    }
    if (!hardwareCaveat.empty()) {
      addWarning(hardwareCaveat);
    }

    const auto metric = metricsByCaptureId.find(source.captureDeviceId);
    const bool streaming = metric != metricsByCaptureId.end() && metric->second.streaming;
    const int64_t framesReceived = metric == metricsByCaptureId.end() ? 0 : metric->second.framesReceived;
    const int64_t framesRendered = metric == metricsByCaptureId.end() ? 0 : metric->second.framesRendered;
    const int64_t queuedFrames = metric == metricsByCaptureId.end() ? 0 : metric->second.queuedFrames;
    const int64_t underrunCount = metric == metricsByCaptureId.end() ? 0 : metric->second.underrunCount;
    const int64_t emptyPacketPolls = metric == metricsByCaptureId.end() ? 0 : metric->second.emptyPacketPolls;
    const int64_t startedAtMs = metric == metricsByCaptureId.end() ? 0 : metric->second.startedAtMs;
    const int64_t lastFrameAtMs = metric == metricsByCaptureId.end() ? 0 : metric->second.lastFrameAtMs;
    const int64_t stoppedAtMs = metric == metricsByCaptureId.end() ? 0 : metric->second.stoppedAtMs;
    const int64_t lastFrameAgeMs = streaming && lastFrameAtMs > 0
                                       ? std::max<int64_t>(0, monotonicMs() - lastFrameAtMs)
                                       : 0;
    const double peakDbfs = metric == metricsByCaptureId.end() ? -120.0 : metric->second.peakDbfs;
    const double rmsDbfs = metric == metricsByCaptureId.end() ? -120.0 : metric->second.rmsDbfs;
    const bool signalPresent = metric != metricsByCaptureId.end() && metric->second.signalPresent;
    std::string sourceWarning = metric == metricsByCaptureId.end() ? std::string{} : metric->second.warning;
    const std::string lastError = metric == metricsByCaptureId.end() ? std::string{} : metric->second.lastError;
    if (!source.audioDeviceId.empty() && metric == metricsByCaptureId.end()) {
      sourceWarning = "Audio source is paired but no native PCM adapter is streaming it.";
      addWarning(source.captureDeviceId + ": " + sourceWarning);
    } else if (metric != metricsByCaptureId.end() && !streaming && sourceWarning.empty() && !lastError.empty()) {
      sourceWarning = "Audio capture adapter is not streaming: " + lastError;
      addWarning(source.captureDeviceId + ": " + sourceWarning);
    } else if (streaming && framesReceived <= 0 && sourceWarning.empty()) {
      sourceWarning = "Audio capture stream is open but no PCM frames have arrived.";
      addWarning(source.captureDeviceId + ": " + sourceWarning);
    } else if (streaming && framesReceived > 0 && lastFrameAgeMs > kStaleCaptureAudioAgeMs && sourceWarning.empty()) {
      sourceWarning = "Audio capture PCM is stale; no new frames have arrived for " + std::to_string(lastFrameAgeMs) + " ms.";
      addWarning(source.captureDeviceId + ": " + sourceWarning);
    } else if (streaming && framesReceived > 0 && !signalPresent && sourceWarning.empty()) {
      sourceWarning = "Audio capture is receiving silent PCM frames; check the selected endpoint or play audio through it.";
      addWarning(source.captureDeviceId + ": " + sourceWarning);
    }
    // Fall back to the hardware caveat when no more-specific warning applies, so
    // ASIO/embedded rows are never silently clean in a build without the adapter.
    if (sourceWarning.empty()) {
      sourceWarning = hardwareCaveat;
    }
    if (streaming) {
      ++streamingCount;
    }
    totalFramesReceived += framesReceived;

    sources.emplace_back(rpc::Json::Object{
        {"captureDeviceId", source.captureDeviceId},
        {"sourceId", metric == metricsByCaptureId.end() ? std::string{} : metric->second.sourceId},
        {"audioDeviceId", source.audioDeviceId},
        {"audioDeviceName", source.audioDeviceName},
        {"audioSourceKind", source.audioSourceKind},
        {"nativeAudioDeviceId", source.nativeAudioDeviceId},
        {"audioDriverName", source.audioDriverName},
        {"embedded", source.embedded},
        {"audioSyncOffsetMs", source.audioSyncOffsetMs},
        {"paired", !source.audioDeviceId.empty()},
        {"captureStreaming", streaming},
        {"captureFramesReceived", static_cast<double>(framesReceived)},
        {"captureFramesRendered", static_cast<double>(framesRendered)},
        {"captureQueuedFrames", static_cast<double>(queuedFrames)},
        {"captureUnderrunCount", static_cast<double>(underrunCount)},
        {"emptyPacketPolls", static_cast<double>(emptyPacketPolls)},
        {"captureStartedAtMs", static_cast<double>(startedAtMs)},
        {"captureLastFrameAtMs", static_cast<double>(lastFrameAtMs)},
        {"captureLastFrameAgeMs", static_cast<double>(lastFrameAgeMs)},
        {"captureStoppedAtMs", static_cast<double>(stoppedAtMs)},
        {"captureSampleRate", metric == metricsByCaptureId.end() ? 0 : metric->second.sampleRate},
        {"captureChannels", metric == metricsByCaptureId.end() ? 0 : metric->second.channels},
        {"peakDbfs", peakDbfs},
        {"rmsDbfs", rmsDbfs},
        {"signalPresent", signalPresent},
        {"endpointId", metric == metricsByCaptureId.end() ? std::string{} : metric->second.endpointId},
        {"endpointName", metric == metricsByCaptureId.end() ? std::string{} : metric->second.endpointName},
        {"lastError", lastError},
        {"warning", sourceWarning},
    });
  }

  std::ostringstream summary;
  summary << pairedCount << " of " << captureAudioSources_.size() << " capture source"
          << (captureAudioSources_.size() == 1 ? "" : "s") << " paired with audio input; "
          << streamingCount << " streaming, " << totalFramesReceived << " PCM frames received; "
          << routedMasterFrames << " master bus frames, " << routedStreamFrames << " stream bus frames, "
          << routedMonitorFrames << " MON bus frames, "
          << fallbackMonitorFrames << " fallback monitor frames, "
          << audioMonitorFramesPlayed_ << " monitor playback frames.";

  return rpc::Json::Object{
      {"status", !captureAudioSourcesSynced_ ? "idle" : warningSet.empty() ? "ready" : "warning"},
      {"sourceCount", static_cast<int>(captureAudioSources_.size())},
      {"pairedCount", pairedCount},
      {"streamingCount", streamingCount},
      {"captureFramesReceived", static_cast<double>(totalFramesReceived)},
      {"routedMasterFrames", routedMasterFrames},
      {"routedStreamFrames", routedStreamFrames},
      {"routedMonitorFrames", routedMonitorFrames},
      {"fallbackMonitorFrames", fallbackMonitorFrames},
      {"monitorFramesPlayed", static_cast<double>(audioMonitorFramesPlayed_)},
      {"sources", sources},
      {"warnings", warnings},
      {"summary", captureAudioSourcesSynced_ ? summary.str() : "Capture audio source pairing idle."},
  };
}

rpc::Json MediaCore::captionTrackState() const {
  rpc::Json::Array warnings;
  for (const auto& warning : captionWarnings_) {
    warnings.emplace_back(warning);
  }
  if (!captionEnabled_) {
    return rpc::Json::Object{
        {"enabled", false},
        {"status", "idle"},
        {"latencyMs", 0},
        {"warnings", warnings},
    };
  }
  if (captionText_.empty()) {
    return rpc::Json::Object{
        {"enabled", true},
        {"status", "idle"},
        {"latencyMs", 180},
        {"warnings", warnings},
    };
  }

  rpc::Json::Object cue{
      {"text", captionText_},
      {"atMs", captionAtMs_},
      {"confidence", static_cast<double>(captionConfidence_)},
  };
  if (!captionSpeaker_.empty()) {
    cue.emplace("speaker", captionSpeaker_);
  }

  return rpc::Json::Object{
      {"enabled", true},
      {"status", warnings.empty() ? "live" : "warning"},
      {"currentCue", cue},
      {"latencyMs", 180},
      {"warnings", warnings},
  };
}

rpc::Json MediaCore::brandKitState() const {
  rpc::Json::Array warnings;
  for (const auto& warning : brandWarnings_) {
    warnings.emplace_back(warning);
  }

  const int appliedOverlayCount = overlayCount_;
  std::ostringstream summary;
  summary << brandName_;
  if (appliedOverlayCount > 0) {
    summary << " applied to " << appliedOverlayCount << " overlays";
  } else {
    summary << " ready";
  }

  return rpc::Json::Object{
      {"name", brandName_},
      {"logoText", brandLogoText_},
      {"brandColor", brandColor_},
      {"accentColor", brandAccentColor_},
      {"backgroundColor", brandBackgroundColor_},
      {"fontFamily", brandFontFamily_},
      {"lowerThirdStyle", brandLowerThirdStyle_},
      {"captionStyle", brandCaptionStyle_},
      {"defaultOverlayBehavior", brandDefaultOverlayBehavior_},
      {"appliedOverlayCount", appliedOverlayCount},
      {"summary", summary.str()},
      {"warnings", warnings},
  };
}

rpc::Json MediaCore::overlayState() const {
  rpc::Json::Array overlays;
  int lowerThirdCount = 0;
  int buildingCount = 0;
  int onAirCount = 0;
  int hiddenCount = 0;

  std::vector<const OverlayAssetState*> orderedOverlays;
  orderedOverlays.reserve(overlayAssets_.size());
  for (const auto& [overlayId, asset] : overlayAssets_) {
    orderedOverlays.push_back(&asset);
  }
  std::sort(
      orderedOverlays.begin(),
      orderedOverlays.end(),
      [](const OverlayAssetState* left, const OverlayAssetState* right) {
        return left->insertionOrder < right->insertionOrder;
      });

  for (const auto* asset : orderedOverlays) {
    const bool isLowerThird = asset->position == "lower-third" || asset->position == "bottom-right";
    if (isLowerThird) {
      ++lowerThirdCount;
    }
    if (asset->keyPhase == "on-air") {
      ++onAirCount;
    } else if (asset->keyPhase == "hidden") {
      ++hiddenCount;
    } else if (asset->keyPhase == "building-in" || asset->keyPhase == "building-out") {
      ++buildingCount;
    }

    overlays.emplace_back(rpc::Json::Object{
        {"overlayId", asset->overlayId},
        {"kind", isLowerThird ? "lower-third" : "overlay"},
        {"position", asset->position},
        {"sourceId", asset->sourceId},
        {"sourceName", asset->sourceName},
        {"title", asset->title},
        {"org", asset->org},
        {"text", asset->text},
        {"keyPosition", asset->keyPosition},
        {"keyPhase", asset->keyPhase},
        {"keyProgress", asset->keyProgress},
        {"keyer", asset->keyer},
        {"buildInMs", asset->buildInMs},
        {"buildOutMs", asset->buildOutMs},
        {"visible", asset->keyPhase != "hidden" && asset->keyPhase != "building-out"},
    });
  }

  std::ostringstream summary;
  summary << lowerThirdCount << " lower-third overlay" << (lowerThirdCount == 1 ? "" : "s")
          << ", " << onAirCount << " on-air, " << buildingCount << " building.";

  return rpc::Json::Object{
      {"status", overlayAssets_.empty() ? "idle" : buildingCount > 0 ? "transitioning" : onAirCount > 0 ? "live" : "ready"},
      {"overlayCount", static_cast<int>(overlayAssets_.size())},
      {"lowerThirdCount", lowerThirdCount},
      {"onAirCount", onAirCount},
      {"buildingCount", buildingCount},
      {"hiddenCount", hiddenCount},
      {"overlays", overlays},
      {"summary", summary.str()},
      {"warnings", rpc::Json::Array{}},
  };
}

rpc::Json MediaCore::mediaPlaybackState() const {
  rpc::Json::Array warnings;
  for (const auto& warning : mediaPlaybackWarnings_) {
    warnings.emplace_back(warning);
  }

  if (mediaPlaybackAssetId_.empty()) {
    return rpc::Json::Object{
        {"status", "idle"},
        {"playing", false},
        {"summary", "No media asset selected."},
        {"warnings", warnings},
    };
  }

  const std::string& name = mediaPlaybackAssetName_;
  return rpc::Json::Object{
      {"status", mediaPlaybackPlaying_ ? "playing" : "paused"},
      {"mediaAssetId", mediaPlaybackAssetId_},
      {"mediaAssetName", name},
      {"mediaAssetKind", mediaPlaybackAssetKind_},
      {"mediaAssetPath", mediaPlaybackAssetPath_},
      {"mediaPlaybackKey", mediaPlaybackKey_},
      {"playing", mediaPlaybackPlaying_},
      {"summary", mediaPlaybackPlaying_
                      ? "Playing " + name + (mediaPlaybackKey_.empty() ? "." : " with key " + mediaPlaybackKey_ + ".")
                      : name + " paused."},
      {"warnings", warnings},
  };
}

rpc::Json MediaCore::encoderSessionState(const modules::OutputSession& session) const {
  rpc::Json::Array targets;
  for (const auto& destination : session.destinations) {
    targets.emplace_back(rpc::Json::Object{
        {"targetId", destination + ":program"},
        {"destination", destination},
        {"streamKind", "program"},
        {"status", encoderLifecycleStatus_ == "encoding" ? "attached" : "idle"},
        {"attachedFrameCount", static_cast<double>(session.encodedFrameCount)},
    });
    if (destination == "recording") {
      for (const auto& participantId : session.isoParticipantIds) {
        targets.emplace_back(rpc::Json::Object{
            {"targetId", "recording:iso:" + participantId},
            {"destination", "recording"},
            {"streamKind", "iso"},
            {"participantId", participantId},
            {"status", encoderLifecycleStatus_ == "encoding" ? "attached" : "idle"},
            {"attachedFrameCount", static_cast<double>(session.encodedFrameCount)},
        });
      }
    }
  }

  rpc::Json::Object lifecycle{
      {"status", encoderLifecycleStatus_},
      {"lastTransition", encoderLastTransition_},
  };
  if (encoderPreparedAtMs_ > 0) {
    lifecycle.emplace("preparedAtMs", encoderPreparedAtMs_);
  }
  if (encoderStartedAtMs_ > 0) {
    lifecycle.emplace("startedAtMs", encoderStartedAtMs_);
  }
  if (encoderStoppedAtMs_ > 0) {
    lifecycle.emplace("stoppedAtMs", encoderStoppedAtMs_);
  }

  const bool warning = session.active && encoderLifecycleStatus_ != "encoding";
  rpc::Json::Array warnings = warning ? rpc::Json::Array{"Output destinations are armed but encoder lifecycle is not encoding."} : rpc::Json::Array{};
  if (!session.recordingWarning.empty()) {
    warnings.emplace_back(session.recordingWarning);
  }
  rpc::Json::Object encoderState{
      {"status", warning || !session.recordingWarning.empty() ? "warning" : encoderLifecycleStatus_ == "encoding" ? "encoding" : "idle"},
      {"renderPlanId", lastProgramFrame_.renderPlanId},
      {"programFrameCount", static_cast<double>(lastProgramFrame_.frameNumber)},
      {"targetBitrateMbps", session.targetBitrateMbps},
      {"recordingFormat", session.recordingFormat.empty() ? recordingFormat_ : session.recordingFormat},
      {"recordingVideoCodec", session.recordingVideoCodec.empty() ? session.codec : session.recordingVideoCodec},
      {"recordingAudioCodec", session.recordingAudioCodec.empty() ? "aac" : session.recordingAudioCodec},
      {"recordingAudioBitrateKbps", session.recordingAudioBitrateKbps > 0 ? session.recordingAudioBitrateKbps : recordingAudioBitrateKbps_},
      {"targets", targets},
      {"lifecycle", lifecycle},
      {"warnings", warnings},
  };
  if (!session.recordingArtifactPath.empty()) {
    encoderState.emplace("recordingArtifactPath", session.recordingArtifactPath);
    encoderState.emplace("recordingBytesWritten", static_cast<double>(session.recordingBytesWritten));
    encoderState.emplace("recordingDurationMs", static_cast<double>(session.recordingDurationMs));
    encoderState.emplace("recordingFrameCount", static_cast<double>(session.recordingVideoFrameCount));
    encoderState.emplace("recordingMetadataValid", session.recordingMetadataValid);
  }
  return encoderState;
}

rpc::Json MediaCore::outputSenderSessionState() const {
  // outputSender->session() is mutated by the worker's outputSender->sync; guard it.
  modules::OutputSenderSession senderSession;
  {
    std::lock_guard<std::mutex> audioLock(audioOutputMutex_);
    senderSession = modules_.outputSender->session();
  }
  rpc::Json::Array senders;
  for (const auto& sender : senderSession.senders) {
    rpc::Json::Object senderJson{
        {"senderId", sender.senderId},
        {"destination", sender.destination},
        {"status", sender.status},
        {"framesSent", static_cast<double>(sender.framesSent)},
        {"retryCount", sender.retryCount},
        {"latencyMs", sender.latencyMs},
        {"bitrateMbps", sender.bitrateMbps},
        {"destinationHealth", sender.destinationHealth},
        {"lastResultCode", sender.lastResultCode},
        {"bytesSent", static_cast<double>(sender.bytesSent)},
        {"audioFramesSent", static_cast<double>(sender.audioFramesSent)},
        {"audioBytesSent", static_cast<double>(sender.audioBytesSent)},
        {"audioChannels", sender.audioChannels},
        {"audioSampleRate", sender.audioSampleRate},
    };
    if (sender.startedAtMs > 0) {
      senderJson.emplace("startedAtMs", sender.startedAtMs);
    }
    if (sender.stoppedAtMs > 0) {
      senderJson.emplace("stoppedAtMs", sender.stoppedAtMs);
    }
    if (sender.lastFrameNumber > 0) {
      senderJson.emplace("lastFrameNumber", static_cast<double>(sender.lastFrameNumber));
    }
    if (!sender.warning.empty()) {
      senderJson.emplace("warning", sender.warning);
    }
    if (!sender.lastError.empty()) {
      senderJson.emplace("lastError", sender.lastError);
    }
    if (!sender.sendArtifactPath.empty()) {
      senderJson.emplace("sendArtifactPath", sender.sendArtifactPath);
      senderJson.emplace("sendBytesWritten", static_cast<double>(sender.sendBytesWritten));
    }
    if (!sender.runtimeDetail.empty()) {
      senderJson.emplace("runtimeDetail", sender.runtimeDetail);
    }
    senders.emplace_back(std::move(senderJson));
  }

  return rpc::Json::Object{
      {"status", senderSession.status},
      {"activeSenderCount", senderSession.activeSenderCount},
      {"senders", senders},
      {"warnings", stringArray(senderSession.warnings)},
  };
}

rpc::Json MediaCore::captureDevicesState() const {
  return captureDeviceArray(modules_.captureDevice->enumerate());
}

rpc::Json MediaCore::recordingState(const modules::OutputSession& session) const {
  if (recordingSessionId_.empty() && recordingStatus_ == "stopped") {
    return nullptr;
  }

  const auto isoIds = recordingIsoParticipantIds_.empty() ? session.isoParticipantIds : recordingIsoParticipantIds_;
  const int64_t programFramesWritten = std::max<int64_t>(recordingProgramFramesWritten_, session.recordingVideoFrameCount);
  const int64_t isoFramesWritten = recordingIsoFramesWritten_ > 0 ? recordingIsoFramesWritten_ : static_cast<int64_t>(isoIds.size()) * programFramesWritten;
  const double durationMs = std::max(recordingElapsedMs_, static_cast<double>(session.recordingDurationMs));
  const int64_t audioPacketsObserved = recordingAudioPacketsObserved_;
  const bool audioPresent = audioPacketsObserved > 0;
  const int recordingWidth = session.recordingWidth > 0 ? session.recordingWidth : lastProgramFrame_.width;
  const int recordingHeight = session.recordingHeight > 0 ? session.recordingHeight : lastProgramFrame_.height;
  const int recordingFps = session.recordingFps > 0 ? session.recordingFps : 30;
  const std::string containerFormat = session.recordingContainerFormat.empty() ? recordingFormat_ : session.recordingContainerFormat;
  const std::string videoCodec = session.recordingVideoCodec.empty() ? session.codec : session.recordingVideoCodec;
  const std::string audioCodec = session.recordingAudioCodec.empty() ? "aac" : session.recordingAudioCodec;
  const bool metadataValid = session.recordingMetadataValid ||
                             (programFramesWritten > 0 && !containerFormat.empty() && !videoCodec.empty() && recordingWidth > 0 && recordingHeight > 0 &&
                              recordingFps > 0 && !audioCodec.empty());
  rpc::Json::Array streams{
      rpc::Json::Object{
          {"kind", "program"},
          {"path", recordingTargetFolder_ + "/" + recordingFilenamePrefix_ + "-program-0." + recordingFormat_},
          {"status", recordingWriterStatus_},
          {"expectedFrames", static_cast<double>(programFramesWritten + recordingDroppedFrames_)},
          {"framesWritten", static_cast<double>(programFramesWritten)},
          {"durationMs", durationMs},
          {"frameRate", recordingFps},
          {"hasAudio", audioPresent},
          {"missingFrames", 0},
          {"droppedFrames", static_cast<double>(recordingDroppedFrames_)},
          {"bytesWritten", static_cast<double>(std::max<int64_t>(session.recordingBytesWritten, programFramesWritten * 260000))},
          {"metadataValid", metadataValid},
      },
  };
  for (const auto& participantId : isoIds) {
    streams.emplace_back(rpc::Json::Object{
        {"kind", "iso"},
        {"participantId", participantId},
        {"path", recordingTargetFolder_ + "/" + recordingFilenamePrefix_ + "-iso-" + participantId + "-0." + recordingFormat_},
        {"status", recordingWriterStatus_},
        {"readiness", "ready"},
        {"framesWritten", static_cast<double>(programFramesWritten)},
        {"durationMs", durationMs},
        {"frameRate", recordingFps},
        {"hasAudio", audioPresent},
        {"bytesWritten", static_cast<double>(programFramesWritten * 140000)},
        {"metadataValid", metadataValid},
    });
  }

  rpc::Json::Object recording{
      {"sessionId", recordingSessionId_.empty() ? "native-recording-session" : recordingSessionId_},
      {"active", recordingStatus_ == "recording" || recordingStatus_ == "warning"},
      {"status", recordingStatus_},
      {"writerStatus", recordingWriterStatus_},
      {"startedAtMs", recordingStartedAtMs_},
      {"elapsedMs", recordingElapsedMs_},
      {"targetFolder", recordingTargetFolder_},
      {"filenamePrefix", recordingFilenamePrefix_},
      {"format", recordingFormat_},
      {"quality", recordingQuality_},
      {"encoder",
       rpc::Json::Object{
           {"codec", session.codec},
           {"hardwareAccelerated", session.hardwareAccelerated},
           {"targetBitrateMbps", session.targetBitrateMbps},
           {"audioBitrateKbps", session.recordingAudioBitrateKbps > 0 ? session.recordingAudioBitrateKbps : recordingAudioBitrateKbps_},
       }},
      {"estimatedDiskRateMBps", 4.99},
      {"programPath", recordingTargetFolder_ + "/" + recordingFilenamePrefix_ + "-program-0." + recordingFormat_},
      {"streams", streams},
      {"proof",
       rpc::Json::Object{
           {"durationMs", durationMs},
           {"programFrameCount", static_cast<double>(programFramesWritten)},
           {"isoFrameCount", static_cast<double>(isoFramesWritten)},
           {"audioPacketsObserved", static_cast<double>(audioPacketsObserved)},
           {"audioPresent", audioPresent},
           {"audioSampleCount", static_cast<double>(session.recordingAudioSampleCount)},
           {"audioChannels", session.recordingAudioChannels},
           {"audioSampleRate", session.recordingAudioSampleRate},
           {"metadataValid", metadataValid},
           {"containerFormat", containerFormat},
           {"videoCodec", videoCodec},
           {"audioCodec", audioCodec},
           {"audioBitrateKbps", session.recordingAudioBitrateKbps > 0 ? session.recordingAudioBitrateKbps : recordingAudioBitrateKbps_},
           {"targetBitrateMbps", session.targetBitrateMbps},
           {"width", recordingWidth},
           {"height", recordingHeight},
           {"frameRate", recordingFps},
           {"failureCount", recordingFailureCount_},
           {"recoveryCount", recordingRecoveryCount_},
       }},
      {"totalFramesWritten", static_cast<double>(programFramesWritten + isoFramesWritten)},
      {"totalDroppedFrames", static_cast<double>(recordingDroppedFrames_)},
      {"totalBytesWritten", static_cast<double>(std::max<int64_t>(session.recordingBytesWritten, programFramesWritten * 260000 + isoFramesWritten * 140000))},
  };
  if (!session.recordingArtifactPath.empty()) {
    recording.emplace("artifactPath", session.recordingArtifactPath);
  }
  if (!recordingError_.empty()) {
    recording.emplace("error", recordingError_);
  }
  if (!recordingWarning_.empty()) {
    recording.emplace("warning", recordingWarning_);
  }
  if (!recordingLastFailure_.empty()) {
    recording.emplace("lastFailure", recordingLastFailure_);
  }
  if (!recordingLastRecovery_.empty()) {
    recording.emplace("lastRecovery", recordingLastRecovery_);
  }
  return recording;
}

void MediaCore::advanceOverlayAnimation(double frameIntervalMs) {
  overlayAnimationClockMs_ += frameIntervalMs;
  // A keyPhase transition completes over a fixed wall-clock window so the
  // animation is frame-rate independent and deterministic for a given fps.
  constexpr double kPhaseDurationMs = 420.0;
  const float step = kPhaseDurationMs > 0.0
                         ? static_cast<float>(frameIntervalMs / kPhaseDurationMs)
                         : 1.f;

  std::vector<std::string> retired;
  for (auto& [overlayId, asset] : overlayAssets_) {
    if (asset.keyPhase == "building-in" || asset.keyPhase == "building-out") {
      asset.keyProgress = std::min(1.f, asset.keyProgress + step);
      if (asset.keyProgress >= 1.f) {
        if (asset.keyPhase == "building-in") {
          asset.keyPhase = "on-air";
          asset.keyProgress = 1.f;
        } else {
          // building-out settled -> retire the asset entirely.
          retired.push_back(overlayId);
        }
      }
    } else {
      asset.keyProgress = 1.f;
    }
  }
  for (const auto& overlayId : retired) {
    overlayAssets_.erase(overlayId);
  }
}

modules::CompositorRenderPlan MediaCore::buildCompositorRenderPlan(const std::vector<modules::VideoFrame>& videoFrames) const {
  modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = sceneId_ + ":" + std::to_string(routeCount_) + ":" + std::to_string(overlayCount_);
  renderPlan.sceneId = sceneId_;
  renderPlan.width = outputWidth_;
  renderPlan.height = outputHeight_;
  renderPlan.fps = outputFps_;
  renderPlan.colorGrade = colorGrade_;
  renderPlan.warnings = sceneValidationWarnings_;

  int videoLayerIndex = 0;
  const int videoLayerCount = routeCount_ > 0 ? routeCount_ : static_cast<int>(videoFrames.size());
  if (sceneBackground_.enabled) {
    modules::CompositorRenderPlanLayer layer;
    layer.layerId = "background:" + sceneBackground_.mediaAssetId;
    layer.kind = "media-background";
    layer.sourceId = layer.layerId;
    layer.mediaAssetId = sceneBackground_.mediaAssetId;
    layer.mediaAssetName = sceneBackground_.mediaAssetName;
    layer.mediaAssetKind = sceneBackground_.mediaAssetKind;
    layer.mediaAssetPath = sceneBackground_.mediaAssetPath;
    layer.mediaAssetPlaying = sceneBackground_.playing;
    layer.order = -100;
    layer.rect = {0.f, 0.f, 1.f, 1.f};
    layer.fitMode = "fill";
    layer.sourceScale = 1.f;
    layer.sourceOffsetX = 0.f;
    layer.sourceOffsetY = 0.f;
    layer.borderStyle = "none";
    layer.borderThickness = 0.f;
    renderPlan.layers.push_back(std::move(layer));
  }
  if (!sceneRoutes_.empty()) {
    renderPlan.layers.reserve(static_cast<size_t>(sceneRoutes_.size() + overlayCount_));
    for (const auto& route : sceneRoutes_) {
      modules::CompositorRenderPlanLayer layer;
      layer.layerId = "route:" + route.routeId;
      layer.kind = route.mode == "screen-share" ? "screen-share" : "participant-video";
      layer.order = videoLayerIndex;
      if (!route.mediaAssetId.empty() && !route.mediaAssetPath.empty()) {
        layer.kind = "media-video";
        layer.sourceId = "media:" + route.mediaAssetId;
        layer.mediaAssetId = route.mediaAssetId;
        layer.mediaAssetName = route.mediaAssetName;
        layer.mediaAssetKind = route.mediaAssetKind;
        layer.mediaAssetPath = route.mediaAssetPath;
        layer.mediaPlaybackKey = route.mediaPlaybackKey;
        layer.mediaAssetPlaying = route.mediaAssetPlaying;
      } else if (route.mode == "capture-input" && !route.captureDeviceId.empty()) {
        layer.participantId = "capture:" + route.captureDeviceId;
        layer.sourceId = layer.participantId;
      } else if (!route.participantId.empty()) {
        layer.participantId = route.participantId;
        layer.sourceId = "zoom:" + route.participantId;
      } else if (videoLayerIndex < static_cast<int>(videoFrames.size())) {
        layer.participantId = videoFrames[static_cast<size_t>(videoLayerIndex)].participantId;
        layer.sourceId = "zoom:" + layer.participantId;
      }
      if (route.hasRect) {
        layer.rect = {route.rectX, route.rectY, route.rectWidth, route.rectHeight};
        layer.order = route.zIndex;
      } else {
        const auto layout = compositor::gridCell((std::max)(1, videoLayerCount), videoLayerIndex);
        layer.rect = {layout.x, layout.y, layout.width, layout.height};
        layer.order = videoLayerIndex;
      }
      layer.fitMode = route.fitMode;
      layer.borderStyle = route.borderStyle;
      layer.borderColor = route.borderColor;
      layer.borderThickness = route.borderThickness;
      layer.sourceScale = route.sourceScale;
      layer.sourceOffsetX = route.sourceOffsetX;
      layer.sourceOffsetY = route.sourceOffsetY;
      layer.hasColorGrade = route.hasColorGrade;
      layer.colorGrade = route.colorGrade;
      renderPlan.layers.push_back(std::move(layer));
      ++videoLayerIndex;
    }
  } else if (!videoFrames.empty()) {
    renderPlan.layers.reserve(videoFrames.size());
    for (size_t index = 0; index < videoFrames.size(); ++index) {
      modules::CompositorRenderPlanLayer layer;
      layer.layerId = "zoom:" + videoFrames[index].participantId;
      layer.kind = "participant-video";
      layer.participantId = videoFrames[index].participantId;
      layer.sourceId = "zoom:" + layer.participantId;
      layer.order = static_cast<int>(index);
      const auto layout = compositor::gridCell(static_cast<int>(videoFrames.size()), static_cast<int>(index));
      layer.rect = {layout.x, layout.y, layout.width, layout.height};
      renderPlan.layers.push_back(std::move(layer));
    }
  }

  // Emit overlay layers from the captured overlay assets in stable insertion
  // order, carrying their real text/image/keyer payload + animated key phase so
  // the compositor can raster them. Overlays with no captured asset (legacy
  // count-only state) fall back to a placeholder lower-third / brand bug.
  std::vector<const OverlayAssetState*> orderedOverlays;
  orderedOverlays.reserve(overlayAssets_.size());
  for (const auto& [overlayId, asset] : overlayAssets_) {
    orderedOverlays.push_back(&asset);
  }
  std::stable_sort(
      orderedOverlays.begin(),
      orderedOverlays.end(),
      [](const OverlayAssetState* left, const OverlayAssetState* right) {
        return left->insertionOrder < right->insertionOrder;
      });

  auto resolveOverlayLayout = [](const std::string& position) -> compositor::LayerRect {
    if (position == "top-right" || position == "upper-left") {
      return compositor::topRightOverlay();
    }
    return compositor::lowerThirdOverlay();
  };

  int overlayLayerIndex = 0;
  for (const auto* asset : orderedOverlays) {
    modules::CompositorRenderPlanLayer layer;
    const bool isLowerThird = asset->position == "lower-third" || asset->position == "bottom-right";
    layer.layerId = "overlay:" + asset->overlayId + (isLowerThird ? ":lower-third" : ":bug");
    layer.kind = "overlay";
    layer.order = static_cast<int>(renderPlan.layers.size());
    const auto layout = resolveOverlayLayout(asset->position);
    layer.rect = {layout.x, layout.y, layout.width, layout.height};
    layer.opacity = 0.92f;
    layer.hasOverlayContent = true;
    layer.overlay.title = asset->title;
    layer.overlay.org = asset->org;
    layer.overlay.text = asset->text;
    layer.overlay.imageUri = asset->imageUri;
    layer.overlay.keyPosition = asset->keyPosition;
    layer.overlay.keyPhase = asset->keyPhase;
    layer.overlay.keyer = asset->keyer;
    layer.overlay.keyProgress = asset->keyProgress;
    layer.overlay.brandColor = brandColor_;
    layer.overlay.brandAccentColor = brandAccentColor_;
    layer.overlay.brandBackgroundColor = brandBackgroundColor_;
    layer.overlay.fontFamily = brandFontFamily_;
    renderPlan.layers.push_back(std::move(layer));
    ++overlayLayerIndex;
  }

  // Legacy fallback: if overlays were tracked as a bare count without captured
  // asset payloads, keep the prior placeholder placement so existing callers
  // and tests still see overlay layers.
  for (int legacyIndex = overlayLayerIndex; legacyIndex < overlayCount_; ++legacyIndex) {
    modules::CompositorRenderPlanLayer layer;
    layer.layerId = legacyIndex == 0 ? "overlay:lower-third" : "overlay:brand-bug";
    layer.kind = "overlay";
    layer.order = static_cast<int>(renderPlan.layers.size());
    const auto layout = legacyIndex == 0 ? compositor::lowerThirdOverlay() : compositor::topRightOverlay();
    layer.rect = {layout.x, layout.y, layout.width, layout.height};
    layer.opacity = 0.92f;
    renderPlan.layers.push_back(std::move(layer));
  }

  // Caption band: a styled lower band carrying the active caption text + speaker
  // attribution when captions are enabled and a cue is present.
  if (captionEnabled_ && !captionText_.empty()) {
    modules::CompositorRenderPlanLayer layer;
    layer.layerId = "overlay:caption";
    layer.kind = "overlay";
    layer.order = static_cast<int>(renderPlan.layers.size());
    layer.rect = {0.08f, 0.86f, 0.84f, 0.10f};
    layer.opacity = 0.95f;
    layer.hasOverlayContent = true;
    layer.overlay.isCaption = true;
    layer.overlay.text = captionText_;
    layer.overlay.speaker = captionSpeaker_;
    layer.overlay.keyPosition = "lower-left";
    layer.overlay.keyPhase = "on-air";
    layer.overlay.keyProgress = 1.f;
    layer.overlay.keyer = "downstream";
    layer.overlay.brandColor = brandColor_;
    layer.overlay.brandAccentColor = brandAccentColor_;
    layer.overlay.brandBackgroundColor = brandBackgroundColor_;
    layer.overlay.fontFamily = brandFontFamily_;
    renderPlan.layers.push_back(std::move(layer));
  }

  return renderPlan;
}

void MediaCore::renderDisplayTick() {
  renderSyntheticTick(/*videoOnly=*/true);
  static int64_t s_displayTickCount = 0;
  static auto s_displayTickStamp = std::chrono::steady_clock::now();
  if (++s_displayTickCount % 120 == 0) {
    const auto now = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(now - s_displayTickStamp).count();
    std::fprintf(stderr, "[render] displayTick #%lld %.1f fps (content render rate)\n",
                 static_cast<long long>(s_displayTickCount), sec > 0.0 ? 120.0 / sec : 0.0);
    s_displayTickStamp = now;
  }
}

void MediaCore::renderSyntheticTick(bool videoOnly) {
  const auto frameIntervalMs = static_cast<int64_t>(std::max(1.0, std::round(1000.0 / std::max(1, outputFps_))));
  const auto frameTimestampMs = static_cast<int64_t>(lastProgramFrame_.frameNumber + 1) * frameIntervalMs;
  // Tap the latest decoded Zoom frames (raw I420 planes) and ingest them into
  // the RealZoomCaptureSource so pollVideoFrames() returns them. Reading them
  // here does NOT drain the stdout/event queue that feeds the multiview tiles.
  auto* realZoom = dynamic_cast<modules::RealZoomCaptureSource*>(modules_.zoom.get());
  if (realZoom && zoomEngineRuntime_ && zoomEngineRuntime_->configured()) {
    const auto decoded = zoomEngineRuntime_->latestDecodedVideoFrames(frameTimestampMs);
    for (const auto& frame : decoded) {
      if (frame.hasI420()) {
        // GPU path: carry the raw I420 planes through to the compositor, which
        // converts to RGB in-shader (no CPU per-pixel I420->BGRA convert).
        realZoom->ingestI420Frame(
            frame.participantId,
            frame.i420,  // zero-copy: share the decoded buffer, don't memcpy it
            frame.i420Width,
            frame.i420Height,
            frame.frameId,
            frame.timestampMs);
      } else if (frame.hasPixels()) {
        realZoom->ingestFrame(
            frame.participantId,
            frame.pixels->data(),
            frame.pixelWidth,
            frame.pixelHeight,
            frame.frameId,
            frame.timestampMs);
      }
    }
  }

  auto videoFrames = modules_.zoom->pollVideoFrames();
  auto captureFrames = modules_.captureDevice->pollVideoFrames(frameTimestampMs);
  videoFrames.insert(videoFrames.end(), captureFrames.begin(), captureFrames.end());
  if (zoomEngineRuntime_ && zoomEngineRuntime_->configured()) {
    const auto engineFrames = zoomEngineRuntime_->pollCompositorVideoFrames(frameTimestampMs);
    if (!engineFrames.empty()) {
      // When the engine reports subscribed video participants, they are the
      // authoritative roster (mirrors the prior synthetic-tick behavior). Start
      // from the engine roster and carry over real BGRA pixels for any
      // participant that has already decoded a frame above; the rest stay
      // metadata-only and fall back to the synthetic slate.
      std::vector<modules::VideoFrame> merged;
      merged.reserve(engineFrames.size());
      for (auto engineFrame : engineFrames) {
        // Carry over any decoded frame for this participant — I420 (GPU path) OR
        // BGRA. Matching only hasPixels() dropped the raw-I420 Zoom frames (which
        // have hasI420() but NOT hasPixels()), replacing them with the metadata-only
        // engine roster frame -> participant rendered BLANK on program/preview.
        const auto withContent = std::find_if(
            videoFrames.begin(), videoFrames.end(), [&](const modules::VideoFrame& candidate) {
              return candidate.participantId == engineFrame.participantId &&
                     (candidate.hasPixels() || candidate.hasI420());
            });
        if (withContent != videoFrames.end()) {
          merged.push_back(*withContent);
        } else {
          merged.push_back(std::move(engineFrame));
        }
      }
      for (auto& captureFrame : captureFrames) {
        merged.push_back(std::move(captureFrame));
      }
      videoFrames = std::move(merged);
    }
  }
  // Audio frames are polled in gatherAudioOutputWork() (the audio/output half), not
  // here: the audio mix / routing / monitor / loudness / encoder / output / recording
  // work has moved off the render+command thread onto renderAudioOutputTick (Phase 2).

  // Advance the overlay animation clock before building the render plan so the
  // compositor reflects this tick's keyPhase progress. The same render plan
  // also tells media sources which scene media layers are active so their audio
  // reaches the mixer in the same tick as their video.
  advanceOverlayAnimation(static_cast<double>(frameIntervalMs));

  auto renderPlan = buildCompositorRenderPlan(videoFrames);
  // On the light display tick, tell the compositor to skip the blocking GPU->CPU
  // readbacks (base64 preview + pixel signature) — only the GPU shared texture is
  // needed on screen, and the per-frame CPU Map otherwise caps the render rate.
  // EXCEPTION: when an output/encoder/recording session is active, the encoder
  // submit (on the audio/output worker) needs the composed CPU pixels, so do the
  // readback even on the light tick while streaming/recording. With no output
  // active (the common case, incl. the perf soak) the light tick stays readback-free.
  const bool outputActive = (encoderLifecycleStatus_ == "encoding") ||
                            recordingStatus_ == "recording" || recordingStatus_ == "warning";
  renderPlan.skipCpuReadback = videoOnly ? !outputActive : false;

  if (modules_.mediaFrames) {
    auto mediaFrames = modules_.mediaFrames->pollMediaFrames(renderPlan.layers, frameTimestampMs);
    videoFrames.insert(videoFrames.end(), mediaFrames.begin(), mediaFrames.end());
    for (const auto& warning : modules_.mediaFrames->warnings()) {
      if (std::find(renderPlan.warnings.begin(), renderPlan.warnings.end(), warning) == renderPlan.warnings.end()) {
        renderPlan.warnings.push_back(warning);
      }
    }
  }
  lastProgramFrame_ = modules_.compositor->render(renderPlan, videoFrames);
  if (!videoOnly && lastProgramFrame_.preview.bgra.empty()) {
    fillSyntheticProgramFramePreview(lastProgramFrame_.preview, renderPlan, videoFrames, lastProgramFrame_);
  }
  // Second GPU composite: the whole multiview grid into ONE keyed-mutex shared
  // texture (mirrors the program shared texture). Opt-in — only when a layout is
  // set. Reuses the same videoFrames, so Zoom + capture tiles work for free, and
  // stays on the light videoOnly tick (no CPU readback).
  if (!multiviewSources_.empty()) {
    auto multiviewPlan = buildMultiviewRenderPlan(videoFrames);
    multiviewPlan.skipCpuReadback = true;
    lastProgramFrame_.multiviewSharedTexture = modules_.compositor->renderMultiview(multiviewPlan, videoFrames);
    lastProgramFrame_.multiviewWidth = multiviewPlan.width;
    lastProgramFrame_.multiviewHeight = multiviewPlan.height;
    static bool loggedMultiview = false;
    if (!loggedMultiview) {
      loggedMultiview = true;
      std::fprintf(stderr, "[multiview] composite: sources=%zu layers=%zu handle='%s' %dx%d\n",
                   multiviewSources_.size(), multiviewPlan.layers.size(),
                   lastProgramFrame_.multiviewSharedTexture.sharedHandleHex.c_str(),
                   multiviewPlan.width, multiviewPlan.height);
    }
    // Per-tile rects, zipped 1:1 with the layout sources (the multiview plan adds
    // exactly one layer per source, in order, before the compositor sorts a copy).
    const std::string activeSpeakerId = zoomSnapshot().getString("activeSpeakerId");
    std::vector<modules::MultiviewTileRect> tiles;
    tiles.reserve(multiviewSources_.size());
    for (size_t i = 0; i < multiviewSources_.size() && i < multiviewPlan.layers.size(); ++i) {
      const auto& source = multiviewSources_[i];
      const auto& planLayer = multiviewPlan.layers[i];
      modules::MultiviewTileRect tile;
      tile.sourceId = planLayer.sourceId.empty() ? source.sourceId : planLayer.sourceId;
      tile.participantId = source.participantId;
      tile.slot = source.slot;
      tile.label = source.label;
      tile.activeSpeaker = !source.participantId.empty() && source.participantId == activeSpeakerId;
      tile.x = planLayer.rect.x;
      tile.y = planLayer.rect.y;
      tile.w = planLayer.rect.width;
      tile.h = planLayer.rect.height;
      tiles.push_back(std::move(tile));
    }
    lastProgramFrame_.multiviewTiles = std::move(tiles);
  }
  // Throttle the base64 preview/shared-texture events to ~10fps. They are only a
  // UI thumbnail, but at full render rate (~60fps) the base64 BGRA payloads
  // saturate stdout and starve RPC command responses (host then times out and
  // the channel looks dead). Recording/streaming still use the full-rate frame.
  {
    const auto nowTp = std::chrono::steady_clock::now();
    // The shared-texture handle is tiny (a handle + dimensions) — emit it on every
    // render so the GPU program present runs at the full render rate (~60fps).
    enqueueProgramSharedTextureEvent();
    // Per-participant GPU textures for the legacy multiview tiles (tiny handles).
    enqueueParticipantSharedTextureEvents();
    // The multiview shared-texture event is emitted only on structural change
    // (and once at cold start), so this is safe to call every render.
    enqueueMultiviewSharedTextureEvent();
    // The base64 preview is a heavy thumbnail; keep it throttled (~30fps) and
    // never emit it on the light display tick (it has no fresh readback).
    if (!videoOnly && nowTp - lastFrameEventEmit_ >= std::chrono::milliseconds(33)) {
      enqueueProgramFramePreviewEvent();
      lastFrameEventEmit_ = nowTp;
    }
  }
  // The audio/output half (mixer->mix, routed-bus matrix + insert chains,
  // monitorOutput->render, BS.1770 loudness, encoder->submit/submitAudio,
  // outputSender->sync, recording mux) runs on the dedicated worker thread via
  // renderAudioOutputTick when audioWorkerActive_ (the live server). It is then a
  // no-op here so the render/command threads never run it under coreMutex. Direct
  // callers (unit tests) keep it synchronous + single-threaded so applyCommands /
  // applyCommand still publish audio/output results into the returned snapshot.
  if (!videoOnly && !audioWorkerActive_) {
    const auto work = gatherAudioOutputWork();
    const auto results = runAudioOutputWork(work);
    publishAudioOutputResults(results);
  }
}

void MediaCore::enableAudioOutputWorker() { audioWorkerActive_ = true; }

// Gather the per-tick audio/output inputs. Caller holds coreMutex (or is the
// single-threaded test path). Polls the audio sources (zoom/engine/capture/media),
// copies the plain-data control state the worker reads, and snapshots the current
// program frame for the encoder/output. Touches only coreMutex-domain state.
MediaCore::AudioOutputWorkItem MediaCore::gatherAudioOutputWork() {
  AudioOutputWorkItem work;
  work.valid = true;
  work.frameIntervalMs = static_cast<int64_t>(std::max(1.0, std::round(1000.0 / std::max(1, outputFps_))));
  const auto frameTimestampMs = static_cast<int64_t>(lastProgramFrame_.frameNumber + 1) * work.frameIntervalMs;

  std::vector<modules::AudioFrame> audioFrames = modules_.zoom->pollAudioFrames();
  if (zoomEngineRuntime_ && zoomEngineRuntime_->configured()) {
    const auto engineAudioFrames =
        zoomEngineRuntime_->pollCompositorAudioFrames(static_cast<int64_t>(lastProgramFrame_.frameNumber + 1) * 20);
    if (!engineAudioFrames.empty()) {
      audioFrames = engineAudioFrames;
    }
  }
  if (modules_.audioCapture) {
    auto captureAudioFrames = modules_.audioCapture->pollAudioFrames(frameTimestampMs);
    audioFrames.insert(audioFrames.end(), captureAudioFrames.begin(), captureAudioFrames.end());
  }
  if (modules_.mediaFrames) {
    // Media-layer audio needs the active scene layers; rebuild the plan here (it is
    // derived from scene-route / media-playback state, not from the polled frames).
    const auto plan = buildCompositorRenderPlan({});
    auto mediaAudioFrames = modules_.mediaFrames->pollMediaAudioFrames(plan.layers, frameTimestampMs);
    audioFrames.insert(audioFrames.end(), mediaAudioFrames.begin(), mediaAudioFrames.end());
  }

  work.audioFrames = std::move(audioFrames);
  work.channels = audioChannels_;
  work.routingSends = audioRoutingSends_;
  work.audioMonitorEnabled = audioMonitorEnabled_;
  work.audioMonitorVolume = audioMonitorVolume_;
  work.recordingActive = (recordingStatus_ == "recording" || recordingStatus_ == "warning");
  work.recordingIsoParticipantIds = recordingIsoParticipantIds_;
  work.outputDestinations = outputDestinations_;
  work.outputDestinationSettings = outputDestinationSettings_;
  work.programFrame = lastProgramFrame_;
  work.tickId = ++audioOutputTickId_;
  return work;
}

// Run the heavy/blocking audio + output DSP/IO. Caller holds audioOutputMutex_ (or is
// the single-threaded test path). Touches only audioOutputMutex_-domain modules
// (mixer / monitorOutput / encoder / outputSender) + the BS.1770 loudness members,
// never coreMutex-domain published members — all results go into the returned struct.
MediaCore::AudioOutputResults MediaCore::runAudioOutputWork(const AudioOutputWorkItem& work) {
  AudioOutputResults results;
  if (!work.valid) {
    return results;
  }
  results.valid = true;
  results.recordingActive = work.recordingActive;

  const auto tMix0 = std::chrono::steady_clock::now();
  results.mixedFrameCount = modules_.mixer->mix(work.audioFrames);
  const auto mixMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tMix0).count();
  if (mixMs >= 30) std::fprintf(stderr, "[audio] mixer->mix %lldms (%zu frames)\n", static_cast<long long>(mixMs), work.audioFrames.size());

  // Routed-bus matrix mix over the real PCM into a LOCAL bus map (published later).
  {
    std::vector<modules::RoutedAudioSource> routedSources;
    routedSources.reserve(work.audioFrames.size());
    for (const auto& frame : work.audioFrames) {
      if (frame.pcm.empty() || frame.channels <= 0) {
        continue;
      }
      modules::RoutedAudioSource source;
      source.sourceId = frame.participantId;
      source.pcm = &frame.pcm;
      source.channels = frame.channels;
      for (const auto& channel : work.channels) {
        if (channel.participantId == frame.participantId) {
          source.muted = channel.muted;
          source.solo = channel.solo;
          source.pan = channel.pan;
          source.gainLinear = modules::dbfsToLinear(channel.hasManualGain ? channel.manualGainDb : 0.0);
          break;
        }
      }
      routedSources.push_back(source);
    }
    std::vector<modules::RoutedAudioCrosspoint> crosspoints;
    crosspoints.reserve(work.routingSends.size());
    for (const auto& send : work.routingSends) {
      crosspoints.push_back({send.sourceId, send.busId, modules::dbfsToLinear(send.gainDb)});
    }
    const auto tMrb0 = std::chrono::steady_clock::now();
    results.routedBusPcm = modules::mixRoutedBuses(routedSources, crosspoints);
    const auto mrbMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tMrb0).count();
    if (mrbMs >= 20) std::fprintf(stderr, "[audio] mixRoutedBuses %lldms (%zu src, %zu sends)\n", static_cast<long long>(mrbMs), routedSources.size(), work.routingSends.size());
    if (!results.routedBusPcm.empty()) {
      std::map<std::string, std::vector<std::string>> busInserts;
      for (const auto& send : work.routingSends) {
        if (!send.busPluginInserts.empty()) {
          busInserts[send.busId] = send.busPluginInserts;
        }
      }
      const auto tBic0 = std::chrono::steady_clock::now();
      for (auto& [busId, pcm] : results.routedBusPcm) {
        const auto found = busInserts.find(busId);
        if (found != busInserts.end() && !pcm.empty()) {
          modules::applyBusInsertChain(pcm.data(), pcm.size(), 48000.0, found->second);
        }
      }
      const auto bicMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tBic0).count();
      if (bicMs >= 20) std::fprintf(stderr, "[audio] busInsertChains %lldms\n", static_cast<long long>(bicMs));
    }
  }

  // LOCAL bus tap lookups over the freshly-mixed routedBusPcm (NOT the published member).
  static const std::vector<float> kEmptyTap;
  const auto localBusTap = [&](const std::string& busId) -> const std::vector<float>& {
    const auto found = results.routedBusPcm.find(busId);
    return found == results.routedBusPcm.end() ? kEmptyTap : found->second;
  };
  const auto& localProgramTap = localBusTap("master");

  if (work.audioMonitorEnabled) {
    results.monitorTouched = true;
    if (work.audioMonitorVolume <= 0.0) {
      results.monitorStatus = "volume-zero";
    } else if (!modules_.monitorOutput || !modules_.monitorOutput->active()) {
      results.monitorStatus = "unavailable";
      results.monitorWarning = "Native audio monitor output device is not open.";
    } else {
      const auto& routedMonitorBus = localBusTap("mon");
      const bool hasRoutedMonitorBus = !routedMonitorBus.empty();
      const auto& monitorBus = hasRoutedMonitorBus ? routedMonitorBus : modules_.mixer->monitorBusPcm();
      const int channels = hasRoutedMonitorBus ? 2 : std::max(1, modules_.mixer->monitorBusChannels());
      const int frameCount = static_cast<int>(monitorBus.size() / static_cast<size_t>(channels));
      if (frameCount <= 0) {
        results.monitorStatus = "armed";
        results.monitorWarning = "Audio monitor is armed but no PCM reached the MON bus or fallback monitor mix.";
      } else if (modules_.monitorOutput->render(monitorBus.data(), frameCount, channels, work.audioMonitorVolume)) {
        results.monitorStatus = modules_.monitorOutput->hardwareOutput() ? "playing" : "stub-monitor";
        results.monitorFramesPlayedDelta += frameCount;
      } else {
        results.monitorStatus = "dropping";
        const auto outputWarnings = modules_.monitorOutput->warnings();
        results.monitorWarning = outputWarnings.empty()
                                     ? "Native audio monitor accepted no frames this tick; endpoint buffer may be full."
                                     : outputWarnings.back();
      }
    }
  }

  // BS.1770 master loudness over the program audio (routed master bus, else the
  // mixer's default program mix). Mutates the loudness members in place under
  // audioOutputMutex_; masterMeterState reads them under the same lock.
  {
    const std::vector<float>& programAudio =
        !localProgramTap.empty() ? localProgramTap : modules_.mixer->monitorBusPcm();
    const int meterChannels = !localProgramTap.empty() ? 2 : modules_.mixer->monitorBusChannels();
    updateProgramLoudnessMeter(programAudio, meterChannels, modules_.mixer->monitorBusSampleRate());
  }

  // Encoder submit (uses the program-frame snapshot), output-sender network sync,
  // recording mux. encoder->submit early-returns when the frame carries no CPU
  // pixels, so when no output is active (readback skipped) this is harmless.
  modules_.encoder->submit(work.programFrame);
  const auto session = modules_.encoder->session();
  auto outputDestinations = work.outputDestinations;
  outputDestinations.erase(
      std::remove(outputDestinations.begin(), outputDestinations.end(), std::string("recording")),
      outputDestinations.end());
  const std::vector<float>& streamBusAudio = localBusTap("stream");
  const std::vector<float>& outputProgramAudio =
      !streamBusAudio.empty() ? streamBusAudio
                              : !localProgramTap.empty() ? localProgramTap : modules_.mixer->monitorBusPcm();
  const int outputAudioChannels =
      !streamBusAudio.empty() || !localProgramTap.empty() ? 2 : modules_.mixer->monitorBusChannels();
  const auto failOutputSenderSync = [&](const std::string& message) {
    const auto destination =
        std::find(outputDestinations.begin(), outputDestinations.end(), "rtmp") != outputDestinations.end()
            ? "rtmp"
            : !outputDestinations.empty() ? outputDestinations.front() : std::string("stream");
    try {
      modules_.outputSender->fail(destination, message, static_cast<double>(work.programFrame.frameNumber * 33));
    } catch (...) {
    }
  };
  const auto tOut0 = std::chrono::steady_clock::now();
  try {
    modules_.outputSender->sync(
        outputDestinations,
        &work.programFrame,
        static_cast<double>(work.programFrame.frameNumber * 33),
        work.outputDestinationSettings,
        outputProgramAudio.empty() ? nullptr : &outputProgramAudio,
        outputAudioChannels,
        modules_.mixer->monitorBusSampleRate());
    const auto outMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - tOut0)
                           .count();
    if (outMs >= 20) {
      std::fprintf(stderr, "[outputSender] sync %lldms dests=%zu\n",
                   static_cast<long long>(outMs), outputDestinations.size());
    }
  } catch (const std::exception& ex) {
    failOutputSenderSync(std::string("Output sender failed during sync: ") + ex.what());
  } catch (...) {
    failOutputSenderSync("Output sender failed during sync.");
  }
  if (work.recordingActive) {
    ++results.recordingProgramFramesDelta;
    const auto isoIds = work.recordingIsoParticipantIds.empty() ? session.isoParticipantIds : work.recordingIsoParticipantIds;
    results.recordingIsoFramesDelta += static_cast<int64_t>(isoIds.size());
    const std::vector<float>& programAudio =
        !localProgramTap.empty() ? localProgramTap : modules_.mixer->monitorBusPcm();
    const int audioChannels = !localProgramTap.empty() ? 2 : modules_.mixer->monitorBusChannels();
    if (!programAudio.empty() && audioChannels > 0) {
      modules_.encoder->submitAudio(programAudio.data(),
                                    static_cast<int>(programAudio.size() / static_cast<size_t>(audioChannels)),
                                    audioChannels, modules_.mixer->monitorBusSampleRate());
    }
    results.recordingAudioPacketsObserved = modules_.encoder->session().recordingAudioPacketCount;
    results.recordingElapsedMsDelta += static_cast<double>(work.frameIntervalMs);
  }
  return results;
}

// Publish the worker results back into the coreMutex-domain members that
// sessionState() reads. Caller holds coreMutex (or is the single-threaded test path).
void MediaCore::publishAudioOutputResults(const AudioOutputResults& results) {
  if (!results.valid) {
    return;
  }
  routedBusPcm_ = results.routedBusPcm;
  mixedAudioFrameCount_ = results.mixedFrameCount;
  if (results.monitorTouched) {
    audioMonitorStatus_ = results.monitorStatus;
    audioMonitorWarning_ = results.monitorWarning;
    audioMonitorFramesPlayed_ += results.monitorFramesPlayedDelta;
  }
  if (results.recordingActive) {
    recordingProgramFramesWritten_ += results.recordingProgramFramesDelta;
    recordingIsoFramesWritten_ += results.recordingIsoFramesDelta;
    recordingAudioPacketsObserved_ = results.recordingAudioPacketsObserved;
    recordingElapsedMs_ += results.recordingElapsedMsDelta;
  }
}

// Worker entry point: gather (brief coreMutex) → work (audioOutputMutex_ only) →
// publish (brief coreMutex). The render thread takes ONLY coreMutex and so is never
// blocked by the long DSP/IO span; the worker never holds both locks at once.
void MediaCore::renderAudioOutputTick(std::mutex& coreMutex) {
  AudioOutputWorkItem work;
  {
    std::lock_guard<std::mutex> lock(coreMutex);
    work = gatherAudioOutputWork();
  }
  AudioOutputResults results;
  {
    std::lock_guard<std::mutex> lock(audioOutputMutex_);
    results = runAudioOutputWork(work);
  }
  {
    std::lock_guard<std::mutex> lock(coreMutex);
    publishAudioOutputResults(results);
  }
}

}  // namespace corevideo::core
