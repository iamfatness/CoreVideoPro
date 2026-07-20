#include "core/MediaCore.h"

#include "compositor/CompositorLayout.h"
#include "core/LockHoldGuardrail.h"
#include "core/Protocol.h"
#include "modules/AudioDsp.h"
#include "modules/ProgramFramePreview.h"
#include "modules/RealZoomCaptureSource.h"
#include "modules/WinUiCaptureDeviceAdapter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <thread>

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
    destination.keyframeIntervalSeconds =
        std::max(0.5, std::min(10.0, value.getNumber("keyframeIntervalSeconds", destination.keyframeIntervalSeconds)));
    destination.rateControl = value.getString("rateControl", destination.rateControl);
    destination.h264Profile = value.getString("h264Profile", destination.h264Profile);
    destination.bFrames =
        std::max(0, std::min(4, static_cast<int>(value.getNumber("bFrames", destination.bFrames))));
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

#if COREVIDEO_WITH_UVC
  result.emplace_back("uvc-capture");
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
  // OS-level device identity (UVC symbolic link) so the shell can correlate a
  // core-enumerated device with its own WinRT enumeration.
  if (!device.nativeDeviceId.empty()) {
    result.emplace("nativeDeviceId", device.nativeDeviceId);
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

rpc::Json MediaCore::connectCaptureDevice(const std::string& deviceId,
                                          const std::string& outputSourceId) {
  return captureDeviceArray(
      outputSourceId.empty() ? modules_.captureDevice->connect(deviceId)
                             : modules_.captureDevice->connect(deviceId, outputSourceId));
}

rpc::Json MediaCore::disconnectCaptureDevice(const std::string& deviceId) {
  std::fprintf(stderr, "[lifecycle] disconnect capture %s\n", deviceId.c_str());
  return captureDeviceArray(modules_.captureDevice->disconnect(deviceId));
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

rpc::Json MediaCore::addBrowserSource(const rpc::Json& payload, std::string& error) {
  const std::string url = payload.getString("url");
  const int width = static_cast<int>(payload.getNumber("width", 1920));
  const int height = static_cast<int>(payload.getNumber("height", 1080));
  const int fps = static_cast<int>(payload.getNumber("fps", 30));
  const std::string id = browserSources_->addSource(url, width, height, fps, error);
  if (id.empty()) {
    return rpc::Json(nullptr);
  }
  return browserSourcesState();
}

rpc::Json MediaCore::removeBrowserSource(const std::string& browserId, std::string& error) {
  if (!browserSources_->removeSource(browserId)) {
    error = "Unknown browser source '" + browserId + "'.";
    return rpc::Json(nullptr);
  }
  return browserSourcesState();
}

rpc::Json MediaCore::reloadBrowserSource(const std::string& browserId, std::string& error) {
  if (!browserSources_->reloadSource(browserId, error)) {
    return rpc::Json(nullptr);
  }
  return browserSourcesState();
}

rpc::Json MediaCore::browserSourcesState() const {
  rpc::Json::Array sources;
  for (const auto& item : browserSources_->telemetry()) {
    sources.emplace_back(rpc::Json::Object{
        {"id", item.id},
        {"url", item.url},
        {"width", item.width},
        {"height", item.height},
        {"fps", item.fps},
        {"measuredFps", item.measuredFps},
        {"running", item.running},
        {"gaveUp", item.gaveUp},
        {"restartCount", item.restartCount},
        {"consecutiveFailures", item.consecutiveFailures},
        {"health", item.health},
        {"lastError", item.lastError},
        {"framesReceived", static_cast<double>(item.framesReceived)},
    });
  }
  return rpc::Json(std::move(sources));
}

bool MediaCore::zoomEngineConfigured() const {
  return zoomEngineRuntime_ && zoomEngineRuntime_->configured();
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
      {"virtualCamera", virtualCameraState()},
      {"captureDevices", captureDevicesState()},
      {"browserSources", browserSourcesState()},
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
  // Cold-start snapshot: a newly connected consumer gets the current preview
  // composite shared texture without waiting for the next structural change.
  const auto previewSharedTexture = modules::previewSharedTextureJson(lastProgramFrame_);
  if (!previewSharedTexture.isNull()) {
    state.emplace("previewSharedTexture", previewSharedTexture);
  }
  // Preview-bus telemetry (present only once a preview scene is set): lets the WinUI
  // and tests see the composited preview scene structure without a GPU texture. `composite`
  // is whether the core runs the dedicated third composite (multi-layer) vs. leaving the
  // operator preview on the single-source fallback.
  if (previewSceneActive_) {
    const auto previewPlanLayers = static_cast<int>(buildPreviewCompositorRenderPlan({}).layers.size());
    state.emplace("previewScene", rpc::Json::Object{
                                      {"sceneId", previewSceneId_},
                                      {"routeCount", previewRouteCount_},
                                      {"overlayCount", previewOverlayCount_},
                                      {"layerCount", previewPlanLayers},
                                      {"composite", hasPreviewScene()},
                                  });
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

  // Deliver the PREVIEW scene on the same frequent, reliable channel (the production
  // sync's set-preview-scene fires only on user actions). applyPreviewScene dedups by
  // content signature, so re-applying every spine tick does NOT churn preview state
  // unless the scene actually changed. Done BEFORE the runtime delegate so it works on
  // both the stub and the real-engine paths (the preview bus lives in MediaCore).
  if (const rpc::Json* previewScene = payload.get("previewScene"); previewScene && previewScene->isObject()) {
    if (applyPreviewScene(*previewScene)) {
      std::fprintf(stderr, "[preview] set-preview-scene received: %d routes (spine)\n", previewRouteCount_);
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
  // pump thread drain Zoom frames WITHOUT contending on the core lock â€” the
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

std::vector<rpc::Json> MediaCore::drainPreviewSharedTextureEvents() {
  auto events = std::move(pendingPreviewSharedTextureEvents_);
  pendingPreviewSharedTextureEvents_.clear();
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
  // the GPU shared texture, so this event is unused â€” and it was emitted at the full
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
  // identity + geometry (label, slot, rects) â€” but NOT the active-speaker flag,
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
    mix(tile.role);
    // Tally is a low-frequency user action (a source taken to program/preview),
    // not frame-rate churn, so include it so the overlay re-renders the tally.
    mix(tile.tally);
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

void MediaCore::enqueuePreviewSharedTextureEvent() {
  const auto event = modules::previewSharedTextureEvent(lastProgramFrame_);
  if (event.isNull()) {
    return;
  }
  // Emit only on structural change (handle/dims) or cold start â€” the live pixels
  // flow through the stable keyed-mutex texture, presented continuously by the
  // host, so a per-frame event would only flood the pipe (see the program event).
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
  mix(lastProgramFrame_.previewSharedTexture.sharedHandleHex);
  mixInt(lastProgramFrame_.previewWidth);
  mixInt(lastProgramFrame_.previewHeight);
  if (signature == 0u) {
    signature = 1u;
  }
  if (previewStructureEmitted_ && signature == lastPreviewStructureSignature_) {
    return;
  }
  lastPreviewStructureSignature_ = signature;
  previewStructureEmitted_ = true;
  pendingPreviewSharedTextureEvents_.emplace_back(event);
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
  // longer needs to drive a synthetic tick under coreMutex â€” return the latest
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
  } else if (type == "set-preview-scene") {
    applyPreviewScene(command);
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
  } else if (type == "sync-virtual-camera") {
    syncVirtualCamera(command);
  } else if (type == "sync-audio-monitor") {
    syncAudioMonitor(command);
  } else if (type == "scan-vst-plugins") {
    startPluginHostScan();
  } else if (type == "open-vst-editor") {
    openVstPluginEditor(command);
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
  } else if (type == "configure-multiviewer") {
    configureMultiviewer(command);
  } else if (type == "configure-srt-ingest-sources") {
    configureSrtIngestSources(command);
  } else if (type == "browser-add") {
    std::string error;
    (void)addBrowserSource(command, error);
    if (!error.empty()) {
      std::fprintf(stderr, "[browser] browser-add REJECTED: %s\n", error.c_str());
    }
  } else if (type == "browser-remove") {
    std::string error;
    (void)removeBrowserSource(command.getString("browserId"), error);
    if (!error.empty()) {
      std::fprintf(stderr, "[browser] browser-remove REJECTED: %s\n", error.c_str());
    }
  } else if (type == "browser-reload") {
    std::string error;
    (void)reloadBrowserSource(command.getString("browserId"), error);
    if (!error.empty()) {
      std::fprintf(stderr, "[browser] browser-reload REJECTED: %s\n", error.c_str());
    }
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
      state.opacity = static_cast<float>((std::max)(0.0, (std::min)(1.0, route.getNumber("opacity", 1.0))));
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
  syncStillMediaDesired();
}

void MediaCore::syncStillMediaDesired() {
  if (!stillMediaCache_) {
    return;
  }
  std::vector<modules::StillMediaFrameCache::StillRequest> desired;
  const auto addRoutes = [&desired](const std::vector<SceneRouteState>& routes,
                                    const std::string& sourcePrefix) {
    for (const auto& route : routes) {
      if (route.mediaAssetId.empty() || route.mediaAssetPath.empty()) {
        continue;
      }
      if (!modules::isStillImageMediaAsset(route.mediaAssetKind, route.mediaAssetPath)) {
        continue;  // video media routes keep the existing playout path
      }
      desired.push_back({sourcePrefix + route.mediaAssetId,
                         modules::normalizeMediaAssetPath(route.mediaAssetPath)});
    }
  };
  addRoutes(sceneRoutes_, "media:");
  addRoutes(previewSceneRoutes_, "preview:media:");
  stillMediaCache_->setDesired(std::move(desired));
}

void MediaCore::setStillImageDecoderForTest(std::unique_ptr<modules::IStillImageDecoder> decoder,
                                            size_t cacheBudgetBytes) {
  stillMediaCache_ =
      std::make_unique<modules::StillMediaFrameCache>(std::move(decoder), cacheBudgetBytes);
  syncStillMediaDesired();
}

void MediaCore::setParticipantTransform(const rpc::Json&) {
  ++transformCount_;
}

void MediaCore::setOverlayAsset(const rpc::Json& command) {
  const std::string overlayId = command.getString("overlayId", "overlay:" + std::to_string(overlayIds_.size() + 1));
  if (command.get("enabled") && !command.get("enabled")->asBool()) {
    overlayIds_.erase(overlayId);
    if (auto existing = overlayAssets_.find(overlayId); existing != overlayAssets_.end()) {
      const auto* requestedPhase = command.get("keyPhase");
      const bool explicitlyHidden = requestedPhase && requestedPhase->isString() &&
                                    requestedPhase->asString() == "hidden";
      if (explicitlyHidden) {
        // The shell already presented and timed the building-out phase. Its
        // final hidden command is a retirement acknowledgement, not a request
        // to start another native build-out.
        overlayAssets_.erase(existing);
        overlayCount_ = static_cast<int>(overlayIds_.size());
        return;
      }
      // Animate the overlay out rather than dropping it instantly; the render
      // tick retires it once the building-out animation has settled. Only START the
      // build-out once: the shell re-sends enabled=false on every sync while the key is
      // hidden, and resetting keyProgress each time restarted the slide-out every tick =
      // a "double bounce" when the operator takes the lower third out. If it is already
      // building out, leave it alone so it plays once and retires.
      if (existing->second.keyPhase != "building-out") {
        existing->second.keyPhase = "building-out";
        existing->second.keyProgress = 0.f;
      }
      existing->second.retireAfterBuildOut = true;
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
  asset.retireAfterBuildOut = false;
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
    // encoder->submit/session in runAudioOutputWork. coreMutex(outer)â†’this(inner).
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
  {
    // Encoder module mutation: guard against the audio/output worker's
    // concurrent encoder->submit/submitAudio/session in runAudioOutputWork.
    // coreMutex (outer, held by the command thread) â†’ audioOutputMutex_ (inner),
    // matching startRecordingSession/startProgramOutput. Finalizes the MP4
    // container(s) so the artifact is playable immediately after stop instead
    // of only after process exit or the next recording start.
    std::lock_guard<std::mutex> audioLock(audioOutputMutex_);
    modules_.encoder->stopRecording();
  }
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
  // modules_.encoder->configureRecording). All callers â€” startProgramOutput,
  // setRecordingTargets, startRecordingSession â€” acquire it around the call.
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
  audioLimiterEnabled_ = !command.get("limiterEnabled") || command.get("limiterEnabled")->asBool();
  // Mastering chain (docs/mastering-chain-spec.md M1) - settings ride the same
  // sync command; telemetry (ride dB) goes OUT via the snapshot, never echoed
  // back into settings (law 5).
  if (const rpc::Json* mastering = command.get("mastering")) {
    modules::MasteringParams params;
    params.enabled = mastering->get("enabled") && mastering->get("enabled")->asBool();
    if (const auto* v = mastering->get("targetLufs")) params.targetLufs = v->asNumber();
    if (const auto* v = mastering->get("ceilingDbfs")) params.ceilingDbfs = v->asNumber();
    if (const auto* v = mastering->get("glueAmount")) params.glueAmount = v->asNumber();
    if (const auto* v = mastering->get("maxRideDb")) params.maxRideDb = v->asNumber();
    if (const auto* v = mastering->get("inputGainDb")) params.inputGainDb = v->asNumber();
    if (const auto* v = mastering->get("highPassHz")) params.highPassHz = v->asNumber();
    if (const auto* v = mastering->get("lowPassHz")) params.lowPassHz = v->asNumber();
    if (const auto* v = mastering->get("lowShelfDb")) params.lowShelfDb = v->asNumber();
    if (const auto* v = mastering->get("presenceDb")) params.presenceDb = v->asNumber();
    if (const auto* v = mastering->get("highShelfDb")) params.highShelfDb = v->asNumber();
    if (const auto* v = mastering->get("stereoWidth")) params.stereoWidth = v->asNumber();
    if (params.enabled != masteringParams_.enabled || params.targetLufs != masteringParams_.targetLufs) {
      std::fprintf(stderr, "[mastering] enabled=%d target=%.1f ceiling=%.1f glue=%.2f maxRide=%.1f\n",
                   params.enabled ? 1 : 0, params.targetLufs, params.ceilingDbfs, params.glueAmount,
                   params.maxRideDb);
    }
    masteringParams_ = params;
  }
  const rpc::Json* channels = command.get("channels");
  // HOLD-LAST guard (same measured shell churn as the routing matrix): an
  // empty channel list mid-show would strip gain/pan/inserts off live audio
  // for a tick. Hold the last non-empty list; adopt empty only if it persists.
  const bool incomingEmpty = !channels || !channels->isArray() || channels->asArray().empty();
  if (incomingEmpty && !audioChannels_.empty()) {
    ++emptyMixSyncStreak_;
    if (emptyMixSyncStreak_ < 25) {
      return;
    }
  }
  if (!incomingEmpty) {
    emptyMixSyncStreak_ = 0;
  } else {
    previousAudioChannels_.clear();
    absentMixChannelStreaks_.clear();
  }

  audioChannels_.clear();
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
    // C5b: per-insert parameter overrides (gate threshold, EQ freqs, comp
    // ratioâ€¦). Unknown keys are carried and ignored by the chain.
    if (const rpc::Json* settings = channel.get("insertSettings"); settings != nullptr && settings->isObject()) {
      for (const auto& [insertName, params] : settings->asObject()) {
        if (!params.isObject()) {
          continue;
        }
        auto& target = input.insertSettings[insertName];
        for (const auto& [key, value] : params.asObject()) {
          if (value.isNumber()) {
            target[key] = value.asNumber();
          }
        }
      }
    }
    if (!input.participantId.empty()) {
      audioChannels_.push_back(std::move(input));
    }
  }
  // PER-CHANNEL hold-last (same measured partial-sync churn as the routing
  // matrix): a channel absent from this sync keeps its previous gain/pan/
  // inserts until the absence persists; present channels take the new values.
  {
    std::set<std::string> incomingChannels;
    for (const auto& channel : audioChannels_) {
      incomingChannels.insert(channel.participantId);
      absentMixChannelStreaks_.erase(channel.participantId);
    }
    for (const auto& previous : previousAudioChannels_) {
      if (incomingChannels.find(previous.participantId) != incomingChannels.end()) {
        continue;
      }
      if (absentMixChannelStreaks_[previous.participantId] < 25) {
        audioChannels_.push_back(previous);
      }
    }
    for (auto it = absentMixChannelStreaks_.begin(); it != absentMixChannelStreaks_.end();) {
      if (incomingChannels.find(it->first) == incomingChannels.end()) {
        ++it->second;
      }
      it = it->second > 30 ? absentMixChannelStreaks_.erase(it) : std::next(it);
    }
    previousAudioChannels_ = audioChannels_;
  }
  // Audio-only command: in the live server the audio/output worker re-mixes on its
  // own cadence, so the command thread must not run a blocking render+readback here.
  // Direct/test callers (no worker) render synchronously so the returned snapshot
  // reflects the new mix immediately.
  if (!audioWorkerActive_) {
    renderSyntheticTick();
  }
}

// Virtual Camera (docs/virtual-camera-spec.md V2): enable/disable the system
// webcam output. On enable the publisher creates the SHM slot and (Windows dev
// build) registers MFCreateVirtualCamera; the output tick then publishes the
// program frame as NV12. Idempotent.
void MediaCore::syncVirtualCamera(const rpc::Json& command) {
  const bool on = !command.get("on") || command.get("on")->asBool();
  // The DLL's media type is FIXED at 1920x1080@60 NV12 (MediaSource.h). The
  // publisher MUST write that exact size or the DLL rejects the frame on a dims
  // mismatch and falls back to the slate. So we ignore any width/height/fps the
  // shell sends and pin the vcam to the DLL's native format. (If we ever make the
  // DLL media type dynamic, thread the negotiated size back here instead.)
  const int width = 1920;
  const int height = 1080;
  const int fps = 60;
  const bool mirror = command.get("mirror") && command.get("mirror")->asBool();
  const std::string deviceName = command.get("deviceName") ? command.get("deviceName")->asString() : "";

  virtualCamera_->setMirror(mirror);
  virtualCamera_->setDeviceName(deviceName);
  if (on && !virtualCameraEnabled_) {
    virtualCamera_->start(width, height, fps);
    virtualCameraEnabled_ = true;
    std::fprintf(stderr, "[virtualcam] enable requested %dx%d@%d mirror=%d\n", width, height, fps,
                 mirror ? 1 : 0);
  } else if (!on && virtualCameraEnabled_) {
    virtualCamera_->stop();
    virtualCameraEnabled_ = false;
    std::fprintf(stderr, "[virtualcam] disable requested\n");
  }
}

rpc::Json MediaCore::virtualCameraState() const {
  const auto status = virtualCamera_->status();
  return rpc::Json::Object{
      {"enabled", status.enabled},
      {"status", status.state},
      {"deviceName", status.deviceName},
      {"resolution", rpc::Json::Object{{"width", status.width}, {"height", status.height}}},
      {"fps", status.fps},
      {"framesPublished", static_cast<double>(status.framesPublished)},
      {"warning", status.warning},
  };
}

void MediaCore::syncAudioMonitor(const rpc::Json& command) {
  // monitorOutput->start/stop + mixer reads mutate/read audio/output module state the
  // worker also touches (monitorOutput->render, mixer->monitorBus*); guard the whole
  // body. coreMutex(outer, held by the command loop) â†’ audioOutputMutex_(inner).
  std::lock_guard<std::mutex> audioLock(audioOutputMutex_);
  audioMonitorEnabled_ = command.get("enabled") && command.get("enabled")->asBool();
  audioMonitorDeviceId_ = command.getString("deviceId");
  audioMonitorDeviceName_ = command.getString("deviceName");
  audioMonitorVolume_ = std::max(0.0, std::min(1.0, command.getNumber("volume", audioMonitorVolume_)));
  audioMonitorWarning_.clear();
  // Click-hunt: the operator toggle state was not reaching the adapter -
  // log exactly what each sync carries so UI-vs-core disagreements are visible.
  std::fprintf(stderr, "[monitor] sync: enabled=%d device=%s volume=%.2f\n",
               audioMonitorEnabled_ ? 1 : 0, audioMonitorDeviceName_.c_str(), audioMonitorVolume_);

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
  const rpc::Json* sends = command.get("sends");
  // HOLD-LAST guard (click-hunt 2026-07-05, measured on the rig): the shell
  // transiently syncs an EMPTY matrix during row-rebuild windows (~1/s), and
  // clearing here unrouted live audio for a tick — a 20ms hole in the MON bus
  // per episode, audible as clicking/warble. A live mixer must never hard-drop
  // audio on a blank control-plane sync: keep the last non-empty matrix and
  // only adopt an empty one after it persists (a real clear-all keeps sending
  // empty and wins after ~1s).
  const bool incomingEmpty = !sends || !sends->isArray() || sends->asArray().empty();
  if (incomingEmpty && !audioRoutingSends_.empty()) {
    ++emptyRoutingSyncStreak_;
    if (emptyRoutingSyncStreak_ < 25) {
      return;  // transient blank: hold the live routing
    }
  }
  if (!incomingEmpty) {
    emptyRoutingSyncStreak_ = 0;
  } else {
    // Adopting a persistent clear-all: the per-source hold must not
    // resurrect the routes we are deliberately dropping.
    previousRoutingSends_.clear();
    absentRoutingSourceStreaks_.clear();
  }

  audioRoutingSends_.clear();
  audioRoutingWarnings_.clear();
  audioRoutingSynced_ = true;

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

  // PER-SOURCE hold-last (rig-measured: PARTIAL syncs — mic missing, media
  // present — still punctured live audio 34x/min after the empty-sync guard).
  // A source absent from this sync keeps its previous sends until the absence
  // PERSISTS (~0.5s of consecutive syncs); then the removal is adopted.
  // Sources present in the sync always take the new values and reset.
  const std::set<std::string> incomingSources = routedSources;
  for (const auto& sourceId : incomingSources) {
    absentRoutingSourceStreaks_.erase(sourceId);
  }
  for (const auto& previous : previousRoutingSends_) {
    if (incomingSources.find(previous.sourceId) != incomingSources.end()) {
      continue;  // present: new config wins
    }
    if (absentRoutingSourceStreaks_[previous.sourceId] < 25) {
      audioRoutingSends_.push_back(previous);
    }
  }
  for (auto it = absentRoutingSourceStreaks_.begin(); it != absentRoutingSourceStreaks_.end();) {
    if (incomingSources.find(it->first) == incomingSources.end()) {
      ++it->second;
    }
    it = it->second > 30 ? absentRoutingSourceStreaks_.erase(it) : std::next(it);  // adopted: forget
  }
  previousRoutingSends_ = audioRoutingSends_;

  // Bus OUTPUT routing (mixer topology): aux/custom buses can send their mix
  // into the fixed program buses, like subgroups on a real desk. Only
  // removable buses (aux-/bus-) may be sources — the fixed set as sources
  // would allow cycles; fixed-bus targets keep the graph acyclic by
  // construction.
  audioBusSends_.clear();
  if (const rpc::Json* busSends = command.get("busSends"); busSends != nullptr && busSends->isArray()) {
    static const std::array<std::string_view, 5> kBusSendTargets = {"master", "pgm-l", "pgm-r",
                                                                    "stream", "mon"};
    for (const auto& send : busSends->asArray()) {
      const std::string fromBusId = send.getString("fromBusId");
      const std::string toBusId = send.getString("toBusId");
      const bool fromRemovable = fromBusId.rfind("aux-", 0) == 0 || fromBusId.rfind("bus-", 0) == 0;
      const bool toFixed = std::any_of(kBusSendTargets.begin(), kBusSendTargets.end(),
                                       [&](std::string_view bus) { return bus == toBusId; });
      if (!fromRemovable || !toFixed) {
        addWarning("Bus send " + fromBusId + " -> " + toBusId +
                   " rejected (only aux/custom buses may feed the fixed program buses).");
        continue;
      }
      AudioBusSendInput input;
      input.fromBusId = fromBusId;
      input.toBusId = toBusId;
      const double rawGain = send.get("gainDb") ? send.get("gainDb")->asNumber() : 0.0;
      input.gainDb = std::max(kMinAudioRoutingGainDb, std::min(kMaxAudioRoutingGainDb, rawGain));
      audioBusSends_.push_back(std::move(input));
    }
  }

  // PFL/listen: the monitor auditions this bus instead of "mon". Empty or
  // unknown = normal monitor bus.
  const std::string listenBusId = command.getString("monitorBusId");
  monitorListenBusId_ = isAudioRoutingBus(listenBusId) ? listenBusId : std::string();

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

void MediaCore::configureMultiviewer(const rpc::Json& command) {
  // User-selectable multiviewer configuration. Only recognized fields are read;
  // unknown fields are ignored gracefully. A layout-mode change is structural, so
  // reset the structural-emit flag to force the next render to re-emit the tiles.
  const std::string requestedMode = command.getString("layoutMode", multiviewLayoutMode_);
  std::string mode = requestedMode;
  if (mode != "grid" && mode != "pgmPvwTop" && mode != "pgmPvwLarge" && mode != "pgmPvwSide") {
    mode = "grid";
  }
  if (mode != multiviewLayoutMode_) {
    multiviewLayoutMode_ = mode;
    multiviewStructureEmitted_ = false;
  }
  if (command.get("tileCount")) {
    const int requested = static_cast<int>(command.getNumber("tileCount", multiviewTileCount_));
    const int clamped = std::max(1, std::min(16, requested));
    if (clamped != multiviewTileCount_) {
      multiviewTileCount_ = clamped;
      multiviewStructureEmitted_ = false;
    }
  }
  if (command.get("showLabels")) {
    multiviewShowLabels_ = command.get("showLabels")->asBool();
  }
  if (command.get("showTally")) {
    multiviewShowTally_ = command.get("showTally")->asBool();
  }
  if (command.get("showMeters")) {
    multiviewShowMeters_ = command.get("showMeters")->asBool();
  }
  if (command.get("showClock")) {
    multiviewShowClock_ = command.get("showClock")->asBool();
  }
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
    // Unchanged layout â€” do NOT churn multiviewSources_ or reset the structural-emit flag.
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

bool MediaCore::applyPreviewScene(const rpc::Json& previewScene) {
  // The WinUI sends the PREVIEW scene node â€” the same shape as load-scene-graph
  // ({sceneId, background, routes[], colorGrade, overlays[]}) â€” carried either by
  // the set-preview-scene command or the frequent spine `previewScene` object.
  // Parse into temps + build a cheap content signature so a re-send of an unchanged
  // scene is a no-op (mirrors applyMultiviewLayout â€” never churns state on repeats).
  const std::string sceneId = previewScene.getString("sceneId", "unloaded");
  std::string signature = "s:" + sceneId + ";";

  SceneBackgroundState background;
  if (const rpc::Json* bg = previewScene.get("background"); bg && bg->isObject()) {
    background.mediaAssetId = bg->getString("mediaAssetId");
    background.mediaAssetName = bg->getString("mediaAssetName");
    background.mediaAssetKind = bg->getString("mediaAssetKind");
    background.mediaAssetPath = bg->getString("mediaAssetPath");
    background.playing = !bg->get("playing") || bg->get("playing")->asBool();
    background.enabled = !background.mediaAssetId.empty() && !background.mediaAssetPath.empty();
  }
  signature += "bg:" + background.mediaAssetId + ":" + background.mediaAssetPath + ";";

  modules::CompositorColorGrade colorGrade;
  if (const rpc::Json* grade = previewScene.get("colorGrade"); grade && grade->isObject()) {
    colorGrade = readColorGrade(*grade);
  }
  signature += "cg:" + std::to_string(colorGrade.exposure) + "," + std::to_string(colorGrade.contrast) + "," +
               std::to_string(colorGrade.saturation) + "," + std::to_string(colorGrade.temperature) + ";";

  std::vector<SceneRouteState> routes;
  if (const rpc::Json* routesNode = previewScene.get("routes"); routesNode && routesNode->isArray()) {
    int routeIndex = 0;
    for (const auto& route : routesNode->asArray()) {
      SceneRouteState state;
      state.routeId = route.getString("routeId");
      state.mode = route.getString("mode", "fixed");
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
      if (const rpc::Json* rect = route.get("rect"); rect && rect->isObject()) {
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
      state.borderColor = route.getString("borderColor", "#44C1A1");
      state.borderThickness = static_cast<float>((std::max)(0.0, (std::min)(12.0, route.getNumber("borderThickness", 2.0))));
      state.sourceScale = static_cast<float>((std::max)(0.25, (std::min)(4.0, route.getNumber("sourceScale", 1.0))));
      state.sourceOffsetX = static_cast<float>((std::max)(-1.0, (std::min)(1.0, route.getNumber("sourceOffsetX", 0.0))));
      state.sourceOffsetY = static_cast<float>((std::max)(-1.0, (std::min)(1.0, route.getNumber("sourceOffsetY", 0.0))));
      state.opacity = static_cast<float>((std::max)(0.0, (std::min)(1.0, route.getNumber("opacity", 1.0))));
      if (const rpc::Json* cg = route.get("colorGrade"); cg && cg->isObject()) {
        state.hasColorGrade = true;
        state.colorGrade = readColorGrade(*cg);
      }
      if (state.routeId.empty()) {
        state.routeId = "preview-route-" + std::to_string(routeIndex);
      }
      signature += "r:" + std::to_string(state.zIndex) + ":" + state.mode + ":" + state.participantId + ":" +
                   state.captureDeviceId + ":" + state.mediaAssetId + ":" + state.fitMode + ":" +
                   std::to_string(state.rectX) + "," + std::to_string(state.rectY) + "," +
                   std::to_string(state.rectWidth) + "," + std::to_string(state.rectHeight) + "," +
                   std::to_string(state.opacity) + "|";
      routes.push_back(std::move(state));
      ++routeIndex;
    }
  }

  // Preview overlays (optional): rendered statically at the on-air phase (no
  // independent animation clock â€” the preview bus mirrors structure, not motion).
  std::map<std::string, OverlayAssetState> overlays;
  if (const rpc::Json* overlaysNode = previewScene.get("overlays"); overlaysNode && overlaysNode->isArray()) {
    int overlayOrder = 0;
    for (const auto& ov : overlaysNode->asArray()) {
      if (!ov.isObject() || (ov.get("enabled") && !ov.get("enabled")->asBool())) {
        continue;
      }
      OverlayAssetState asset;
      asset.overlayId = ov.getString("overlayId", "preview-overlay:" + std::to_string(overlayOrder));
      asset.text = ov.getString("text");
      asset.imageUri = ov.getString("imageUri");
      asset.sourceId = ov.getString("sourceId");
      asset.sourceName = ov.getString("sourceName");
      asset.position = ov.getString("position", "lower-third");
      asset.title = ov.getString("title");
      asset.org = ov.getString("org");
      asset.keyPosition = ov.getString("keyPosition", "lower-left");
      asset.keyer = ov.getString("keyer", "downstream");
      asset.keyPhase = "on-air";
      asset.keyProgress = 1.f;
      asset.insertionOrder = overlayOrder++;
      signature += "o:" + asset.overlayId + ":" + asset.position + ":" + asset.title + ":" + asset.text + "|";
      overlays.emplace(asset.overlayId, std::move(asset));
    }
  }

  if (previewSceneActive_ && signature == previewSceneSignature_) {
    return false;  // Unchanged â€” do not churn preview state.
  }

  previewSceneSignature_ = std::move(signature);
  previewSceneId_ = sceneId;
  previewSceneRoutes_ = std::move(routes);
  previewSceneBackground_ = std::move(background);
  previewColorGrade_ = colorGrade;
  previewOverlayAssets_ = std::move(overlays);
  previewRouteCount_ = static_cast<int>(previewSceneRoutes_.size());
  previewOverlayCount_ = static_cast<int>(previewOverlayAssets_.size());
  previewSceneActive_ = true;
  // A preview-scene change is structural; force the next render's event re-emit.
  previewStructureEmitted_ = false;
  syncStillMediaDesired();
  return true;
}

namespace {

// The feed-resolution result for one multiview source: the same source-id
// conventions the program plan uses so resolveLayers/frameForParticipant match
// the same decoded frames.
struct ResolvedMultiviewFeed {
  std::string kind;
  std::string sourceId;
  std::string participantId;
  std::string mediaAssetId;
};

ResolvedMultiviewFeed resolveMultiviewFeed(
    const std::string& srcKind,
    const std::string& sourceId,
    const std::string& participantId,
    const std::string& captureDeviceId,
    const std::string& mediaAssetId) {
  ResolvedMultiviewFeed feed;
  if (srcKind == "media" && !mediaAssetId.empty()) {
    feed.kind = "media-video";
    feed.sourceId = "media:" + mediaAssetId;
    feed.mediaAssetId = mediaAssetId;
  } else if (srcKind == "capture" && !captureDeviceId.empty()) {
    feed.kind = "participant-video";
    feed.participantId = "capture:" + captureDeviceId;
    feed.sourceId = feed.participantId;
  } else if (!participantId.empty()) {
    feed.kind = "participant-video";
    feed.participantId = participantId;
    feed.sourceId = "zoom:" + participantId;
  } else {
    feed.kind = "participant-video";
    feed.sourceId = sourceId;
  }
  return feed;
}

}  // namespace

modules::CompositorRenderPlan MediaCore::buildMultiviewRenderPlan(const std::vector<modules::VideoFrame>& videoFrames) const {
  modules::CompositorRenderPlan renderPlan;
  renderPlan.sceneId = sceneId_;
  renderPlan.width = multiviewCanvasWidth_ > 0 ? multiviewCanvasWidth_ : outputWidth_;
  renderPlan.height = multiviewCanvasHeight_ > 0 ? multiviewCanvasHeight_ : outputHeight_;
  renderPlan.fps = outputFps_;
  renderPlan.colorGrade = colorGrade_;

  const float mvCanvasW = static_cast<float>(renderPlan.width > 0 ? renderPlan.width : 1920);
  const float mvCanvasH = static_cast<float>(renderPlan.height > 0 ? renderPlan.height : 1080);

  const std::string activeSpeakerId = zoomSnapshot().getString("activeSpeakerId");
  const int totalSources = static_cast<int>(multiviewSources_.size());
  const bool pgmPvw = multiviewLayoutMode_ != "grid";
  // Grid shows every source; the pgmPvw modes show up to tileCount source tiles.
  const int sourceCount = pgmPvw ? (std::min)(totalSources, (std::max)(1, multiviewTileCount_)) : totalSources;
  const auto layout = compositor::computeMultiviewLayout(multiviewLayoutMode_, sourceCount);
  renderPlan.renderPlanId = "multiview:" + multiviewLayoutMode_ + ":" + std::to_string(sourceCount);

  int order = 0;

  if (layout.hasProgramPreview) {
    const auto pgmRect = compositor::centeredAspectRect(layout.programCell, mvCanvasW, mvCanvasH);
    const auto pvwRect = compositor::centeredAspectRect(layout.previewCell, mvCanvasW, mvCanvasH);

    // PROGRAM cell: composite the FULL current program scene by remapping every
    // program-plan layer's rect into the PGM sub-rect. This reuses the exact
    // program layer set (routes + overlays + captions) so the PGM cell matches
    // what the operator sees on PROGRAM.
    const auto programPlan = buildCompositorRenderPlan(videoFrames);
    renderPlan.layers.reserve(programPlan.layers.size() + static_cast<size_t>(sourceCount) + 1);
    for (const auto& src : programPlan.layers) {
      modules::CompositorRenderPlanLayer layer = src;
      layer.layerId = "multiview-pgm:" + src.layerId;
      layer.rect = {
          pgmRect.x + src.rect.x * pgmRect.width,
          pgmRect.y + src.rect.y * pgmRect.height,
          src.rect.width * pgmRect.width,
          src.rect.height * pgmRect.height};
      layer.hasClipRect = true;
      layer.clipRect = {pgmRect.x, pgmRect.y, pgmRect.width, pgmRect.height};
      layer.order = order++;
      layer.borderStyle = "none";
      layer.borderThickness = 0.f;
      renderPlan.layers.push_back(std::move(layer));
    }

    // PREVIEW cell: composite the FULL previewed scene by remapping every
    // preview-plan layer's rect into the PVW sub-rect â€” mirroring the PGM cell
    // above â€” so the PVW cell always matches the dedicated PREVIEW monitor and
    // follows a Take (Previewâ†’Program swaps previewSceneRoutes_). When nothing is
    // cued in preview (no preview layers) the cell stays empty â€” an ATEM PVW bus
    // shows what is cued or black, never an arbitrary roster source. (The old
    // fallback drew multiviewSources_.front(), a fixed first-source feed that
    // never reflected the preview and never swapped on Take.)
    if (hasPreviewScene()) {
      const auto previewPlan = buildPreviewCompositorRenderPlan(videoFrames);
      for (const auto& src : previewPlan.layers) {
        modules::CompositorRenderPlanLayer layer = src;
        layer.layerId = "multiview-pvw:" + src.layerId;
        layer.rect = {
            pvwRect.x + src.rect.x * pvwRect.width,
            pvwRect.y + src.rect.y * pvwRect.height,
            src.rect.width * pvwRect.width,
            src.rect.height * pvwRect.height};
        layer.hasClipRect = true;
        layer.clipRect = {pvwRect.x, pvwRect.y, pvwRect.width, pvwRect.height};
        layer.order = order++;
        layer.borderStyle = "none";
        layer.borderThickness = 0.f;
        renderPlan.layers.push_back(std::move(layer));
      }
    }
  }

  // Source tiles: one centered-16:9 layer per source cell.
  const int placed = (std::min)(sourceCount, static_cast<int>(layout.sourceCells.size()));
  for (int index = 0; index < placed; ++index) {
    const auto& source = multiviewSources_[static_cast<size_t>(index)];
    const auto feed = resolveMultiviewFeed(source.kind, source.sourceId, source.participantId,
                                           source.captureDeviceId, source.mediaAssetId);
    modules::CompositorRenderPlanLayer layer;
    layer.layerId = "multiview:" + (source.sourceId.empty() ? std::to_string(index) : source.sourceId);
    layer.kind = feed.kind;
    layer.sourceId = feed.sourceId;
    layer.participantId = feed.participantId;
    layer.mediaAssetId = feed.mediaAssetId;
    const auto tileRect = compositor::centeredAspectRect(layout.sourceCells[static_cast<size_t>(index)], mvCanvasW, mvCanvasH);
    layer.rect = {tileRect.x, tileRect.y, tileRect.width, tileRect.height};
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
    layer.order = order++;
    renderPlan.layers.push_back(std::move(layer));
  }

  return renderPlan;
}

std::vector<modules::MultiviewTileRect> MediaCore::buildMultiviewTiles(const std::string& activeSpeakerId) const {
  std::vector<modules::MultiviewTileRect> tiles;
  const int width = multiviewCanvasWidth_ > 0 ? multiviewCanvasWidth_ : outputWidth_;
  const int height = multiviewCanvasHeight_ > 0 ? multiviewCanvasHeight_ : outputHeight_;
  const float mvCanvasW = static_cast<float>(width > 0 ? width : 1920);
  const float mvCanvasH = static_cast<float>(height > 0 ? height : 1080);

  const int totalSources = static_cast<int>(multiviewSources_.size());
  const bool pgmPvw = multiviewLayoutMode_ != "grid";
  const int sourceCount = pgmPvw ? (std::min)(totalSources, (std::max)(1, multiviewTileCount_)) : totalSources;
  const auto layout = compositor::computeMultiviewLayout(multiviewLayoutMode_, sourceCount);

  // Program-route identities for source tally. A source tile whose feed matches a
  // program route reads tally "pgm". The core has no preview bus yet, so source
  // tiles never read "pvw" (TODO: refine once a core preview program exists).
  std::set<std::string> programKeys;
  for (const auto& route : sceneRoutes_) {
    if (!route.participantId.empty()) programKeys.insert("p:" + route.participantId);
    if (!route.captureDeviceId.empty()) programKeys.insert("c:" + route.captureDeviceId);
    if (!route.mediaAssetId.empty()) programKeys.insert("m:" + route.mediaAssetId);
  }

  auto assignRect = [&](modules::MultiviewTileRect& tile, const compositor::LayerRect& cell) {
    const auto r = compositor::centeredAspectRect(cell, mvCanvasW, mvCanvasH);
    tile.x = r.x;
    tile.y = r.y;
    tile.w = r.width;
    tile.h = r.height;
  };

  tiles.reserve(static_cast<size_t>(sourceCount) + 2);

  if (layout.hasProgramPreview) {
    modules::MultiviewTileRect pgm;
    pgm.role = "pgm";
    pgm.tally = "pgm";
    pgm.label = "Program";
    pgm.slot = -2;
    assignRect(pgm, layout.programCell);
    tiles.push_back(std::move(pgm));

    modules::MultiviewTileRect pvw;
    pvw.role = "pvw";
    pvw.tally = "pvw";
    pvw.label = "Preview";
    pvw.slot = -1;
    // The PVW cell renders the live preview composite (buildMultiviewRenderPlan),
    // not a specific roster source, so it carries no pinned sourceId/participantId
    // â€” clicking it is a no-op rather than cueing an arbitrary source. (It used to
    // pin multiviewSources_.front(), a fixed first-source feed that never swapped.)
    assignRect(pvw, layout.previewCell);
    tiles.push_back(std::move(pvw));
  }

  const int placed = (std::min)(sourceCount, static_cast<int>(layout.sourceCells.size()));
  for (int index = 0; index < placed; ++index) {
    const auto& source = multiviewSources_[static_cast<size_t>(index)];
    const auto feed = resolveMultiviewFeed(source.kind, source.sourceId, source.participantId,
                                           source.captureDeviceId, source.mediaAssetId);
    modules::MultiviewTileRect tile;
    tile.sourceId = feed.sourceId.empty() ? source.sourceId : feed.sourceId;
    tile.participantId = source.participantId;
    tile.slot = source.slot;
    tile.label = source.label;
    tile.role = "source";
    tile.activeSpeaker = !source.participantId.empty() && source.participantId == activeSpeakerId;

    std::string key;
    if (source.kind == "media" && !source.mediaAssetId.empty()) {
      key = "m:" + source.mediaAssetId;
    } else if (source.kind == "capture" && !source.captureDeviceId.empty()) {
      key = "c:" + source.captureDeviceId;
    } else if (!source.participantId.empty()) {
      key = "p:" + source.participantId;
    }
    tile.tally = (!key.empty() && programKeys.count(key) > 0) ? "pgm" : "none";
    assignRect(tile, layout.sourceCells[static_cast<size_t>(index)]);
    tiles.push_back(std::move(tile));
  }

  return tiles;
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
          {"pluginHost", pluginHostState()},
          {"limiterEnabled", audioLimiterEnabled_},
          {"limiterActive", audioLimiterEnabled_ && nativeMix.limiterActive},
          {"masteringEnabled", masteringParams_.enabled},
          {"masteringRideDb", audioMasteringRideDb_},
          {"mixedFrameCount", static_cast<double>(nativeMix.mixedFrameCount)},
          {"monitorEnabled", audioMonitorEnabled_},
          {"monitorStatus", audioMonitorStatus_},
          {"monitorDeviceId", audioMonitorDeviceId_},
          {"monitorDeviceName", audioMonitorDeviceName_},
          {"monitorVolume", audioMonitorVolume_},
          {"monitorFramesPlayed", static_cast<double>(audioMonitorFramesPlayed_)},
          {"monitorUnderruns", static_cast<double>(audioMonitorUnderruns_)},
          {"monitorFeedbackRisk", audioMonitorFeedbackRisk_},
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
        {"monitorUnderruns", static_cast<double>(audioMonitorUnderruns_)},
        {"monitorFeedbackRisk", audioMonitorFeedbackRisk_},
        {"participants", rpc::Json::Array{}},
        {"masterMeter", masterMeterState()},
        {"summary", "Audio mix idle."},
        {"pluginHost", pluginHostState()},
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
  const bool vstHostRunning = pluginHostClient_.ready() && pluginHostClient_.hostAlive();
  const auto vstHostExchanges = pluginHostClient_.exchanges();
  const int vstHostStatusCode = pluginHostClient_.statusCode();
  const std::string vstHostActivePlugin = pluginHostClient_.activePlugin();
  const std::string vstHostError = pluginHostClient_.lastError();

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
    }
    if (!hasPcm && !channel.muted && warningSet.insert("missing-pcm:" + channel.participantId).second) {
      warnings.emplace_back("No native PCM has been mixed for " + channel.participantId + "; meters are held at silence for that channel.");
    }

    rpc::Json::Array pluginInserts;
    for (const auto& insert : channel.pluginInserts) {
      const bool isVst = isHostHandledInsertName(insert);
      const std::string query = vstSelectionQueryFromInsertName(insert);
      const bool active = isVst && vstHostRunning && vstHostExchanges > 0 &&
                          vstHostStatusCode == corevideo::pluginhost::kHostStatusPluginActive &&
                          (query.empty() || vstHostActivePlugin == query ||
                           query.find(vstHostActivePlugin) != std::string::npos);
      const bool failed = isVst && vstHostStatusCode == corevideo::pluginhost::kHostStatusPluginFailed &&
                          !vstHostError.empty();
      if (failed && warningSet.insert("vst-host-failed:" + insert).second) {
        warnings.emplace_back("VST3 insert bypassed safely: " + vstHostError);
      }
      pluginInserts.emplace_back(rpc::Json::Object{
          {"name", insert},
          {"format", isVst ? "vst3" : "builtin"},
          {"status", !isVst ? "available" : active ? "processing" : failed ? "bypassed" : "starting"},
          {"processingEnabled", !isVst || active},
      });
    }

    rpc::Json::Object participant{
        {"participantId", channel.participantId},
        {"inputLevel", measuredInputLevel},
        {"outputLevel", outputLevel},
        {"gainDb", gainDb},
        {"rmsDbfs", hasPcm ? round1Dbfs(modules::linearToDbfs(measured->rmsLevel)) : deriveRmsDbfs(0)},
        {"peakDbfs", hasPcm ? round1Dbfs(modules::linearToDbfs(measured->peakLevel)) : derivePeakDbfs(0)},
        // C7b: live compressor gain reduction (dB, 0 when idle/not engaged) -
        // feeds the workspace GR meter.
        {"gainReductionDb",
         audioCompGainReductionDbBySource_.count(channel.participantId) != 0
             ? audioCompGainReductionDbBySource_.at(channel.participantId)
             : 0.0},
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
      // REAL BS.1770 program loudness (owner-reported: this read a hardcoded
      // -16 whenever mixer channels were synced â€” i.e. always). The worker's
      // updateProgramLoudnessMeter maintains these members under the same
      // audioOutputMutex_ this function holds. Short-term (3s) is the live
      // console readout; fall back to momentary until its window fills.
      {"loudnessLufs", programLufsShortTerm_ > -119.0 ? programLufsShortTerm_ : programLufsMomentary_},
      {"pluginHost", pluginHostState()},
      {"limiterEnabled", audioLimiterEnabled_},
      {"limiterActive", limiterActive},
      {"mixedFrameCount", static_cast<double>(mixedAudioFrameCount_)},
      {"monitorEnabled", audioMonitorEnabled_},
      {"monitorStatus", audioMonitorStatus_},
      {"monitorDeviceId", audioMonitorDeviceId_},
      {"monitorDeviceName", audioMonitorDeviceName_},
      {"monitorVolume", audioMonitorVolume_},
      {"monitorFramesPlayed", static_cast<double>(audioMonitorFramesPlayed_)},
      {"monitorUnderruns", static_cast<double>(audioMonitorUnderruns_)},
      {"monitorFeedbackRisk", audioMonitorFeedbackRisk_},
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
  // full 3 s window every tick costs ~240ms and was starving the 60fps render â€”
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
      {"monitorUnderruns", static_cast<double>(audioMonitorUnderruns_)},
      {"monitorFeedbackRisk", audioMonitorFeedbackRisk_},
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
  auto devices = modules_.captureDevice->enumerate();
  // Browser sources present as capture devices (kind "browser") so the shell's
  // Sources pickers list them exactly like screens.
  auto browserDevices = browserSources_->enumerate();
  devices.insert(devices.end(), browserDevices.begin(), browserDevices.end());
  return captureDeviceArray(devices);
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
  std::vector<std::string> retired;
  for (auto& [overlayId, asset] : overlayAssets_) {
    if (asset.keyPhase == "building-in" || asset.keyPhase == "building-out") {
      // The shell and compositor must share one timing contract. Using the old
      // fixed 420 ms clock here while the shell waited for the operator's
      // buildInMs/buildOutMs caused the core to settle/retire a key early and
      // then recreate it on the next phase command -- the visible bounce.
      const double phaseDurationMs = static_cast<double>(
          asset.keyPhase == "building-in" ? asset.buildInMs : asset.buildOutMs);
      const float step = phaseDurationMs > 0.0
                             ? static_cast<float>(frameIntervalMs / phaseDurationMs)
                             : 1.f;
      asset.keyProgress = std::min(1.f, asset.keyProgress + step);
      if (asset.keyProgress >= 1.f) {
        if (asset.keyPhase == "building-in") {
          asset.keyPhase = "on-air";
          asset.keyProgress = 1.f;
        } else if (asset.retireAfterBuildOut) {
          // Native-owned build-out settled -> retire the asset entirely.
          retired.push_back(overlayId);
        } else {
          // Shell-owned lower-third build-out has settled. Keep one invisible
          // layer until the explicit hidden command arrives. Repeated scene
          // syncs can now update this same stable layer instead of recreating a
          // fresh key and flashing it back on screen.
          asset.keyProgress = 1.f;
        }
      }
    } else {
      asset.keyProgress = 1.f;
    }
  }
  for (const auto& overlayId : retired) {
    overlayAssets_.erase(overlayId);
    overlayIds_.erase(overlayId);
  }
  overlayCount_ = static_cast<int>(overlayIds_.size());
}

modules::CompositorRenderPlan MediaCore::buildCompositorRenderPlan(const std::vector<modules::VideoFrame>& videoFrames) const {
  auto plan = buildRenderPlanForScene(sceneId_, routeCount_, overlayCount_, sceneBackground_, sceneRoutes_,
                                      colorGrade_, overlayAssets_, captionEnabled_, captionText_, captionSpeaker_,
                                      videoFrames);
  plan.warnings = sceneValidationWarnings_;
  return plan;
}

modules::CompositorRenderPlan MediaCore::buildPreviewCompositorRenderPlan(const std::vector<modules::VideoFrame>& videoFrames) const {
  // The preview scene composites its own routes/background/overlays/grade. Captions
  // are a program broadcast element, so the preview bus renders no caption band.
  auto plan = buildRenderPlanForScene(previewSceneId_, previewRouteCount_, previewOverlayCount_,
                                      previewSceneBackground_, previewSceneRoutes_, previewColorGrade_,
                                      previewOverlayAssets_, /*captionEnabled=*/false, std::string{}, std::string{},
                                      videoFrames);
  // Program and Preview may hold the same asset at different playback positions.
  // Give Preview its own frame-source namespace so its held cue frame cannot be
  // replaced by Program's moving decoder (or vice versa).
  for (auto& layer : plan.layers) {
    if (!layer.mediaAssetId.empty()) {
      const auto sourceId = layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId;
      layer.sourceId = "preview:" + sourceId;
    }
  }
  return plan;
}

bool MediaCore::hasPreviewScene() const {
  // Composite the preview bus whenever a preview scene has been synced with at least one
  // layer. Compositing single-source previews too (not just multi-layer) keeps the WinUI
  // logic simple and CORRECT: the composite is stable/always-on once a scene is set, so a
  // multi-layerâ†’single-source preview switch never strands the consumer on a stale texture
  // (the preview-shared-texture event is structural-change-gated and never re-emits an
  // "off" state). Live video still requires a per-frame recomposite regardless, so gating
  // out the single-source case would not save the per-frame GPU pass anyway.
  if (!previewSceneActive_) {
    return false;
  }
  const int layerCount = previewRouteCount_ + (previewSceneBackground_.enabled ? 1 : 0) +
                         static_cast<int>(previewOverlayAssets_.size());
  return layerCount >= 1;
}

modules::CompositorRenderPlan MediaCore::buildRenderPlanForScene(
    const std::string& sceneId,
    int routeCount,
    int overlayCount,
    const SceneBackgroundState& sceneBackground,
    const std::vector<SceneRouteState>& sceneRoutes,
    const modules::CompositorColorGrade& colorGrade,
    const std::map<std::string, OverlayAssetState>& overlayAssets,
    bool captionEnabled,
    const std::string& captionText,
    const std::string& captionSpeaker,
    const std::vector<modules::VideoFrame>& videoFrames) const {
  modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = sceneId + ":" + std::to_string(routeCount) + ":" + std::to_string(overlayCount);
  renderPlan.sceneId = sceneId;
  renderPlan.width = outputWidth_;
  renderPlan.height = outputHeight_;
  renderPlan.fps = outputFps_;
  renderPlan.colorGrade = colorGrade;

  int videoLayerIndex = 0;
  const int videoLayerCount = routeCount > 0 ? routeCount : static_cast<int>(videoFrames.size());
  if (sceneBackground.enabled) {
    modules::CompositorRenderPlanLayer layer;
    layer.layerId = "background:" + sceneBackground.mediaAssetId;
    layer.kind = "media-background";
    layer.sourceId = layer.layerId;
    layer.mediaAssetId = sceneBackground.mediaAssetId;
    layer.mediaAssetName = sceneBackground.mediaAssetName;
    layer.mediaAssetKind = sceneBackground.mediaAssetKind;
    layer.mediaAssetPath = sceneBackground.mediaAssetPath;
    layer.mediaAssetPlaying = sceneBackground.playing;
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
  if (!sceneRoutes.empty()) {
    renderPlan.layers.reserve(static_cast<size_t>(sceneRoutes.size() + overlayCount));
    for (const auto& route : sceneRoutes) {
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
      layer.opacity = route.opacity;
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
  orderedOverlays.reserve(overlayAssets.size());
  for (const auto& [overlayId, asset] : overlayAssets) {
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
  for (int legacyIndex = overlayLayerIndex; legacyIndex < overlayCount; ++legacyIndex) {
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
  if (captionEnabled && !captionText.empty()) {
    modules::CompositorRenderPlanLayer layer;
    layer.layerId = "overlay:caption";
    layer.kind = "overlay";
    layer.order = static_cast<int>(renderPlan.layers.size());
    layer.rect = {0.08f, 0.86f, 0.84f, 0.10f};
    layer.opacity = 0.95f;
    layer.hasOverlayContent = true;
    layer.overlay.isCaption = true;
    layer.overlay.text = captionText;
    layer.overlay.speaker = captionSpeaker;
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
  // Stage timing for the ~60fps display tick (videoOnly only): attributes the
  // render-thread coreMutex hold to ingest / plan / program / multiview /
  // preview so a long tick in the rig log says WHERE the time went. Averaged
  // over 120 ticks (~2s) and logged alongside the compositor's source-texture
  // upload counters (which prove the per-source cache is deduping uploads).
  static int64_t s_stageIngestUs = 0;
  static int64_t s_stagePlanUs = 0;
  static int64_t s_stageProgramUs = 0;
  static int64_t s_stageMultiviewUs = 0;
  static int64_t s_stagePreviewUs = 0;
  static int s_stageTicks = 0;
  auto stageMark = std::chrono::steady_clock::now();
  const auto markStage = [&stageMark, videoOnly](int64_t& acc) {
    if (!videoOnly) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    acc += std::chrono::duration_cast<std::chrono::microseconds>(now - stageMark).count();
    stageMark = now;
  };
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
  // Browser-source frames ride the capture stream (keyed "capture:browser:<n>"),
  // so scenes/multiview/routing treat them exactly like any capture device. Same
  // per-frame copy cost as one WinUI capture-shm bridge device.
  if (!browserSources_->empty()) {
    auto browserFrames = browserSources_->pollVideoFrames(frameTimestampMs);
    captureFrames.insert(captureFrames.end(),
                         std::make_move_iterator(browserFrames.begin()),
                         std::make_move_iterator(browserFrames.end()));
  }
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
        // Carry over any decoded frame for this participant â€” I420 (GPU path) OR
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
  // Still-image media routes (logos/bugs): inject the persistent decoded frames
  // (keyed "media:<assetId>") so program, preview bus and multiview all match
  // them like any other source frame. Cheap by construction — shared_ptr copies
  // of cached buffers with stable frameIds (zero per-tick pixel work; the WIC
  // decode ran on the cache's background thread). Until a decode completes the
  // layer keeps the existing placeholder.
  if (stillMediaCache_) {
    auto stillFrames = stillMediaCache_->collectFrames(frameTimestampMs);
    videoFrames.insert(videoFrames.end(), std::make_move_iterator(stillFrames.begin()),
                       std::make_move_iterator(stillFrames.end()));
  }
  markStage(s_stageIngestUs);

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
  // readbacks (base64 preview + pixel signature) â€” only the GPU shared texture is
  // needed on screen, and the per-frame CPU Map otherwise caps the render rate.
  // EXCEPTION: when an output/encoder/recording session is active, the encoder
  // submit (on the audio/output worker) needs the composed CPU pixels, so do the
  // readback even on the light tick while streaming/recording. With no output
  // active (the common case, incl. the perf soak) the light tick stays readback-free.
  const bool outputActive = (encoderLifecycleStatus_ == "encoding") ||
                            recordingStatus_ == "recording" || recordingStatus_ == "warning";
  renderPlan.skipCpuReadback = videoOnly ? !outputActive : false;
  // The virtual camera is an output too, but it wants NATIVE resolution (it serves
  // a real webcam), not the 320x180 UI thumbnail. Gate the dedicated full-res
  // program readback on the vcam being enabled; the publisher (on the audio/output
  // worker) converts that full BGRA to NV12 for the SHM slot. Independent of
  // skipCpuReadback so the light 60fps display tick still skips the small preview.
  // Streaming needs the same GPU tap as the virtual camera. RTMP previously
  // consumed `ProgramFrame::preview` (a 320x180 UI thumbnail), so YouTube saw
  // the connection but could not render the declared 4K program correctly.
  // The tap scales on-GPU to 1080p and converts to NV12 on its own thread.
  renderPlan.fullProgramReadback = virtualCameraEnabled_ || outputActive;

  if (modules_.mediaFrames) {
    auto mediaLayers = renderPlan.layers;
    if (hasPreviewScene()) {
      const auto previewMediaPlan = buildPreviewCompositorRenderPlan(videoFrames);
      mediaLayers.insert(mediaLayers.end(), previewMediaPlan.layers.begin(), previewMediaPlan.layers.end());
    }
    auto mediaFrames = modules_.mediaFrames->pollMediaFrames(mediaLayers, frameTimestampMs);
    videoFrames.insert(videoFrames.end(), mediaFrames.begin(), mediaFrames.end());
    for (const auto& warning : modules_.mediaFrames->warnings()) {
      if (std::find(renderPlan.warnings.begin(), renderPlan.warnings.end(), warning) == renderPlan.warnings.end()) {
        renderPlan.warnings.push_back(warning);
      }
    }
  }
  // Failure honesty: surface still-media decode failures (missing file, bad
  // image) in the render-plan warnings so they reach snapshot diagnostics —
  // the stderr side is rate-limited inside the cache.
  if (stillMediaCache_) {
    for (const auto& warning : stillMediaCache_->warnings()) {
      if (std::find(renderPlan.warnings.begin(), renderPlan.warnings.end(), warning) == renderPlan.warnings.end()) {
        renderPlan.warnings.push_back(warning);
      }
    }
  }
  markStage(s_stagePlanUs);
  lastProgramFrame_ = modules_.compositor->render(renderPlan, videoFrames);
  if (!videoOnly && lastProgramFrame_.preview.bgra.empty()) {
    fillSyntheticProgramFramePreview(lastProgramFrame_.preview, renderPlan, videoFrames, lastProgramFrame_);
  }
  markStage(s_stageProgramUs);
  // Second GPU composite: the whole multiview grid into ONE keyed-mutex shared
  // texture (mirrors the program shared texture). Opt-in â€” only when a layout is
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
    // Per-tile rects (geometry + role + tally + label) for the active layout mode.
    // buildMultiviewTiles derives the same cell geometry buildMultiviewRenderPlan
    // placed its layers into, plus the PGM/PVW cells for the pgmPvw modes.
    const std::string activeSpeakerId = zoomSnapshot().getString("activeSpeakerId");
    lastProgramFrame_.multiviewTiles = buildMultiviewTiles(activeSpeakerId);
  }
  markStage(s_stageMultiviewUs);
  // Third GPU composite: the PREVIEW scene into its OWN keyed-mutex shared texture
  // (mirrors the program shared texture). Opt-in â€” only for a genuinely multi-layer
  // preview scene (a single passthrough source stays on the cheap WinUI single-source
  // path). Reuses the same videoFrames, stays on the light videoOnly tick (no CPU
  // readback), and never touches the audio/output lock.
  if (hasPreviewScene()) {
    auto previewPlan = buildPreviewCompositorRenderPlan(videoFrames);
    previewPlan.skipCpuReadback = true;
    lastProgramFrame_.previewSharedTexture = modules_.compositor->renderPreview(previewPlan, videoFrames);
    lastProgramFrame_.previewWidth = previewPlan.width;
    lastProgramFrame_.previewHeight = previewPlan.height;
    static bool loggedPreview = false;
    if (!loggedPreview) {
      loggedPreview = true;
      std::fprintf(stderr, "[preview] composite: routes=%d layers=%zu handle='%s' %dx%d\n",
                   previewRouteCount_, previewPlan.layers.size(),
                   lastProgramFrame_.previewSharedTexture.sharedHandleHex.c_str(),
                   previewPlan.width, previewPlan.height);
    }
  } else if (lastProgramFrame_.previewSharedTexture.width != 0) {
    // Preview scene retired / became single-source: clear the handle so the WinUI
    // falls back to the single-source preview path and the event re-emits on return.
    lastProgramFrame_.previewSharedTexture = {};
    lastProgramFrame_.previewWidth = 0;
    lastProgramFrame_.previewHeight = 0;
    previewStructureEmitted_ = false;
  }
  markStage(s_stagePreviewUs);
  if (videoOnly && ++s_stageTicks >= 120) {
    // Delta the cumulative compositor upload counters so the line reads as
    // "uploads in the last ~2s window".
    static modules::CompositorSourceTexStats s_lastTexStats;
    const auto texStats = modules_.compositor->sourceTexStats();
    std::fprintf(stderr,
                 "[render] stages avg-ms ingest=%.2f plan=%.2f program=%.2f multiview=%.2f preview=%.2f"
                 "  source-tex uploads=%lld hits=%lld creates=%lld scratch=%lld\n",
                 s_stageIngestUs / (s_stageTicks * 1000.0), s_stagePlanUs / (s_stageTicks * 1000.0),
                 s_stageProgramUs / (s_stageTicks * 1000.0), s_stageMultiviewUs / (s_stageTicks * 1000.0),
                 s_stagePreviewUs / (s_stageTicks * 1000.0),
                 static_cast<long long>(texStats.cachedUploads - s_lastTexStats.cachedUploads),
                 static_cast<long long>(texStats.cacheHits - s_lastTexStats.cacheHits),
                 static_cast<long long>(texStats.textureCreates - s_lastTexStats.textureCreates),
                 static_cast<long long>(texStats.scratchUploads - s_lastTexStats.scratchUploads));
    s_lastTexStats = texStats;
    s_stageIngestUs = s_stagePlanUs = s_stageProgramUs = s_stageMultiviewUs = s_stagePreviewUs = 0;
    s_stageTicks = 0;
  }
  // Throttle the base64 preview/shared-texture events to ~10fps. They are only a
  // UI thumbnail, but at full render rate (~60fps) the base64 BGRA payloads
  // saturate stdout and starve RPC command responses (host then times out and
  // the channel looks dead). Recording/streaming still use the full-rate frame.
  {
    const auto nowTp = std::chrono::steady_clock::now();
    // The shared-texture handle is tiny (a handle + dimensions) â€” emit it on every
    // render so the GPU program present runs at the full render rate (~60fps).
    enqueueProgramSharedTextureEvent();
    // Per-participant GPU textures for the legacy multiview tiles (tiny handles).
    enqueueParticipantSharedTextureEvents();
    // The multiview shared-texture event is emitted only on structural change
    // (and once at cold start), so this is safe to call every render.
    enqueueMultiviewSharedTextureEvent();
    // The preview shared-texture event is likewise emitted only on structural
    // change (handle/dims) â€” the live pixels flow through the keyed-mutex texture.
    enqueuePreviewSharedTextureEvent();
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
    auto work = gatherAudioOutputWork();
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

  // One contiguous PCM frame per source per tick: multiple 10ms packets drained
  // in one tick must CONCATENATE, not overlap-sum in the bus mixers (spec R3).
  work.audioFrames = modules::coalescePcmAudioFramesBySource(std::move(audioFrames));
  work.channels = audioChannels_;
  work.routingSends = audioRoutingSends_;
  work.busSends = audioBusSends_;
  work.monitorListenBusId = monitorListenBusId_;
  work.limiterEnabled = audioLimiterEnabled_;
  work.masteringParams = masteringParams_;
  work.audioMonitorEnabled = audioMonitorEnabled_;
  work.audioMonitorVolume = audioMonitorVolume_;
  // Feedback-guard inputs (spec R6): the resolved endpoints of every ACTIVE
  // loopback capture source, compared in the monitor block against the
  // endpoint the monitor actually opened.
  if (modules_.audioCapture) {
    for (const auto& metric : modules_.audioCapture->metrics()) {
      if (metric.streaming && !metric.endpointId.empty() &&
          metric.audioSourceKind.find("loopback") != std::string::npos) {
        work.loopbackCaptureEndpointIds.push_back(metric.endpointId);
      }
    }
  }
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
// never coreMutex-domain published members â€” all results go into the returned struct.
MediaCore::AudioOutputResults MediaCore::runAudioOutputWork(AudioOutputWorkItem& work) {
  AudioOutputResults results;
  if (!work.valid) {
    return results;
  }
  results.valid = true;
  results.recordingActive = work.recordingActive;

  // Spec 4.2: absorb capture-vs-tick phase jitter so every downstream block
  // (buses, monitor, encoder) is exactly one tick of samples per source -
  // variable 480/960 blocks were audible as raw-path distortion.
  modules::steadyAudioFrameFeed(work.audioFrames, audioFeedStates_);

  // Canonical bus rate for every source BEFORE any mixing (Zoom-source buzz:
  // 32k PCM summed raw into the 48k bus = wrong speed + per-tick shortfall).
  {
    const int busRate = modules_.mixer ? modules_.mixer->monitorBusSampleRate() : 48000;
    for (auto& frame : work.audioFrames) {
      if (!frame.pcm.empty() && frame.channels > 0 && frame.sampleRate > 0 && frame.sampleRate != busRate) {
        modules::resampleLinearTo(frame.pcm, frame.channels, frame.sampleRate, busRate,
                                  audioResampleStates_[frame.participantId]);
        frame.sampleRate = busRate;
        frame.sampleCount = static_cast<int>(frame.pcm.size() / static_cast<size_t>(frame.channels));
      }
    }
  }

  // DEBUG TAP (env-gated): dump the mic PCM entering the mix and the MON bus
  // leaving it as raw float32 - the decisive click-hunt instrument. Set
  // COREVIDEO_AUDIO_DEBUG_DIR to enable; files append per process run.
  // Tap files stay OPEN across ticks: per-tick fopen/fclose x many files cost
  // ~13ms/tick on the audio worker (soak run 16: work=17ms, 42/50 ticks) -
  // the Heisenberg lesson, structural edition. Diagnostic-only; OS closes at
  // process exit.
  static const auto debugTapFile = [](const std::string& path) -> FILE* {
    static std::map<std::string, FILE*> files;
    auto it = files.find(path);
    if (it == files.end()) {
      it = files.emplace(path, std::fopen(path.c_str(), "ab")).first;
    }
    return it->second;
  };
  static const char* debugDir = std::getenv("COREVIDEO_AUDIO_DEBUG_DIR");
  if (debugDir != nullptr) {
    for (const auto& frame : work.audioFrames) {
      if (!frame.pcm.empty()) {
        const std::string path = std::string(debugDir) + "/tap-in-" + frame.participantId + ".f32";
        if (FILE* file = debugTapFile(path)) {
          std::fwrite(frame.pcm.data(), sizeof(float), frame.pcm.size(), file);
        }
      }
    }
  }

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
          // Spec 4.4: the strip's insert chain + noise suppression now PROCESS
          // (previously stored/exported only). Pointer into work.channels,
          // which outlives the mix call.
          source.inserts = &channel.pluginInserts;
          source.insertSettings = &channel.insertSettings;  // C5b params
          source.dspState = &channelDspStates_[frame.participantId];  // C7c continuity
          if (debugDir != nullptr) {
            std::fprintf(stderr, "[dsp] %s state=%p env=%.5f gain=%.5f hold=%zu\n",
                         frame.participantId.c_str(), static_cast<void*>(source.dspState),
                         source.dspState->gateEnvelope, source.dspState->gateGain,
                         source.dspState->gateHoldRemaining);
          }
          source.noiseSuppression = channel.noiseSuppression;
          source.sampleRate = modules_.mixer->monitorBusSampleRate();
          break;
        }
      }
      routedSources.push_back(source);
    }
    if (debugDir != nullptr) {
      const std::string path = std::string(debugDir) + "/tap-structure.txt";
      if (FILE* file = debugTapFile(path)) {
        for (const auto& src : routedSources) {
          std::string inserts;
          if (src.inserts != nullptr) {
            for (const auto& name : *src.inserts) {
              inserts += name + "+";
            }
          }
          std::fprintf(file, "%s f=%zu ch=%d ns=%d g=%.4f mu=%d ins=%s | ", src.sourceId.c_str(),
                       src.pcm != nullptr ? src.pcm->size() / static_cast<size_t>(src.channels > 0 ? src.channels : 1) : 0,
                       src.channels, src.noiseSuppression ? 1 : 0, src.gainLinear, src.muted ? 1 : 0,
                       inserts.c_str());
        }
        std::fprintf(file, "sends=%zu chans=%zu\n", work.routingSends.size(), work.channels.size());
      }
    }
    std::vector<modules::RoutedAudioCrosspoint> crosspoints;
    crosspoints.reserve(work.routingSends.size());
    for (const auto& send : work.routingSends) {
      crosspoints.push_back({send.sourceId, send.busId, modules::dbfsToLinear(send.gainDb)});
    }
    // One fail-open bridge hook for both channel and bus chains. A recognized
    // host insert always returns true even when unresolved/loading/failed so
    // no built-in name matcher can accidentally reinterpret it. PCM is copied
    // back only after a completed isolated-host exchange.
    const auto processExternalInsert = [this](const std::string& insertName,
                                              std::vector<float>& pcm,
                                              double sampleRate) -> bool {
      if (!isHostHandledInsertName(insertName)) {
        return false;
      }

      const std::string vstQuery = vstSelectionQueryFromInsertName(insertName);
      VstInsertSelection selection;
      selection.resolved = true;  // legacy host-test names select test gain
      if (!vstQuery.empty()) {
        selection = resolveVstInsertForWorker(vstQuery);
      }
      if (!selection.resolved || pcm.empty()) {
        return true;  // recognized, safely bypassed
      }
      if (pluginHostClient_.ready()) {
        pluginHostClient_.exchange(
            pcm.data(), pcm.size(), 2, static_cast<int>(sampleRate), 4,
            selection.bundleId, selection.className);
      } else {
        ensurePluginHostServeStarted();
      }
      return true;
    };
    const auto tMrb0 = std::chrono::steady_clock::now();
    results.routedBusPcm = modules::mixRoutedBuses(routedSources, crosspoints, work.limiterEnabled,
                                                   &results.compGainReductionDbBySource, &busLimiterGains_,
                                                   processExternalInsert);
    std::map<std::string, std::vector<std::string>> busInserts;
    for (const auto& send : work.routingSends) {
      if (!send.busPluginInserts.empty()) {
        busInserts[send.busId] = send.busPluginInserts;
      }
    }
    const auto applyOrderedBusInserts = [&](const std::string& busId) {
      auto pcmIt = results.routedBusPcm.find(busId);
      const auto insertsIt = busInserts.find(busId);
      if (pcmIt == results.routedBusPcm.end() || pcmIt->second.empty() || insertsIt == busInserts.end()) {
        return;
      }
      for (const auto& insert : insertsIt->second) {
        if (processExternalInsert(insert, pcmIt->second, 48000.0)) {
          continue;
        }
        modules::applyBusInsertChain(pcmIt->second.data(), pcmIt->second.size(), 48000.0, {insert});
      }
    };
    // BUS OUTPUT ROUTING (mixer topology): aux/custom bus mixes sum into their
    // destinations in two passes around the mastering block. Pass 1 (targets
    // = "master") runs BEFORE mastering so an aux feeding the program is
    // mastered with it and inherits everywhere. Pass 2 (all other targets)
    // runs AFTER the mastering-inherit so a monitor-only cue send survives
    // the inherit's overwrite of mon/stream. Touched targets are re-limited —
    // summing can exceed the per-bus ceiling from mixRoutedBuses.
    std::vector<modules::AudioBusSend> preMasterSends;
    std::vector<modules::AudioBusSend> postMasterSends;
    for (const auto& send : work.busSends) {
      modules::AudioBusSend converted{send.fromBusId, send.toBusId, modules::dbfsToLinear(send.gainDb)};
      (send.toBusId == "master" ? preMasterSends : postMasterSends).push_back(std::move(converted));
    }
    const auto relimitTouchedBuses = [&](const std::set<std::string>& touched) {
      for (const auto& busId : touched) {
        auto& pcm = results.routedBusPcm[busId];
        if (pcm.empty()) {
          continue;
        }
        if (work.limiterEnabled) {
          modules::applySmoothedPeakLimiter(pcm.data(), pcm.size(), 2, -1.0, 60.0, 48000.0,
                                            &busSendLimiterGains_[busId]);
        } else {
          for (auto& sample : pcm) {
            sample = std::max(-1.0f, std::min(1.0f, sample));
          }
        }
      }
    };
    if (!preMasterSends.empty()) {
      relimitTouchedBuses(modules::applyBusSends(results.routedBusPcm, preMasterSends));
    }
    // The master insert chain must run BEFORE Program/Stream/Monitor inherit
    // the master signal. The previous post-copy placement processed a dead-end
    // master buffer while the outgoing program retained the unprocessed PCM.
    applyOrderedBusInserts("master");
    // Mastering chain on the MASTER bus (M1). Topology CONFIRMED by owner
    // 2026-07-06: master and program L/R carry the SAME signal - the chain
    // applies once and pgm-l/pgm-r inherit the processed master.
    if (work.masteringParams.enabled) {
      auto master = results.routedBusPcm.find("master");
      if (master != results.routedBusPcm.end() && master->second.size() >= 2) {
        results.masteringRideDb = modules::processMasteringChain(
            masteringState_, work.masteringParams, master->second.data(),
            master->second.size() / 2, 48000.0);
        // ONE program signal (owner topology): every program-facing bus
        // inherits the mastered master - program L/R, the STREAM bus (what
        // the encoder broadcasts), and MON (what the operator hears; without
        // this the chain was inaudible at the monitor - owner-reported).
        static int s_masteringLogTick = 0;
        if (++s_masteringLogTick % 250 == 1) {
          std::fprintf(stderr, "[mastering] ride=%.2fdB avg=%.1fLUFS target=%.1f\n",
                       results.masteringRideDb, masteringState_.loudnessAvgLufs,
                       work.masteringParams.targetLufs);
        }
      }
    }
    // MASTER is the default program signal even when the built-in mastering
    // chain is bypassed. An explicitly routed STREAM or MON bus is a deliberate
    // matrix override and must retain its own mix; only missing/empty program
    // buses inherit the processed master.
    if (const auto master = results.routedBusPcm.find("master");
        master != results.routedBusPcm.end() && !master->second.empty()) {
      const auto inheritWhenUnrouted = [&](const std::string& busId) {
        auto target = results.routedBusPcm.find(busId);
        if (target == results.routedBusPcm.end() || target->second.empty()) {
          results.routedBusPcm[busId] = master->second;
        }
      };
      inheritWhenUnrouted("pgm-l");
      inheritWhenUnrouted("pgm-r");
      inheritWhenUnrouted("stream");
      inheritWhenUnrouted("mon");
    }
    // Bus sends pass 2: non-master targets, after the mastering-inherit so a
    // cue/aux send into MON or STREAM is not wiped by the inherit copy.
    if (!postMasterSends.empty()) {
      relimitTouchedBuses(modules::applyBusSends(results.routedBusPcm, postMasterSends));
    }
    const auto tBic0 = std::chrono::steady_clock::now();
    for (const auto& [busId, inserts] : busInserts) {
      if (busId != "master" && !inserts.empty()) {
        applyOrderedBusInserts(busId);
      }
    }
    const auto bicMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - tBic0)
                           .count();
    if (bicMs >= 20) {
      std::fprintf(stderr, "[audio] busInsertChains %lldms\n", static_cast<long long>(bicMs));
    }
    if (debugDir != nullptr) {
      const auto monTap = results.routedBusPcm.find("mon");
      if (monTap != results.routedBusPcm.end() && !monTap->second.empty()) {
        const std::string path = std::string(debugDir) + "/tap-mon.f32";
        if (FILE* file = debugTapFile(path)) {
          std::fwrite(monTap->second.data(), sizeof(float), monTap->second.size(), file);
        }
      }
      if (monTap != results.routedBusPcm.end()) {
        const std::string sizesPath = std::string(debugDir) + "/tap-mon-sizes.txt";
        if (FILE* file = debugTapFile(sizesPath)) {
          std::fprintf(file, "%zu\n", monTap->second.size() / 2);
        }
      }
    }
    const auto mrbMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tMrb0).count();
    if (mrbMs >= 20) std::fprintf(stderr, "[audio] mixRoutedBuses %lldms (%zu src, %zu sends)\n", static_cast<long long>(mrbMs), routedSources.size(), work.routingSends.size());
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
      // PFL/listen: when the operator auditions a specific bus, the monitor
      // renders THAT bus; empty listen id = the normal MON bus. Falls back to
      // MON (then the legacy mixer monitor mix) when the listened bus is
      // silent this tick, so soloing an empty aux never mutes the headphones
      // into confusion — the status label reports what is actually playing.
      const auto& listenBus =
          work.monitorListenBusId.empty() ? kEmptyTap : localBusTap(work.monitorListenBusId);
      const auto& routedMonitorBus = !listenBus.empty() ? listenBus : localBusTap("mon");
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
      // Cumulative device-dry gap count (spec R5): previously the endpoint
      // playing silence between fills was audible but invisible to telemetry.
      results.monitorUnderruns = modules_.monitorOutput->underrunCount();

      // Feedback guard (spec R6): the out-of-box config loopback-captures the
      // default render endpoint â€” if the monitor plays into that SAME endpoint,
      // its output re-enters the mix. Endpoint ids are GUID paths; compare
      // case-insensitively.
      const auto monitorEndpoint = modules_.monitorOutput->resolvedEndpointId();
      if (!monitorEndpoint.empty()) {
        const auto equalsIgnoreCase = [](const std::string& a, const std::string& b) {
          return a.size() == b.size() &&
                 std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) ==
                          std::tolower(static_cast<unsigned char>(y));
                 });
        };
        for (const auto& loopbackEndpoint : work.loopbackCaptureEndpointIds) {
          if (equalsIgnoreCase(loopbackEndpoint, monitorEndpoint)) {
            results.monitorFeedbackRisk = true;
            const std::string feedbackWarning =
                "Monitor output plays into the same endpoint the local loopback source captures â€” "
                "feedback loop. Pick a different monitor device or disable the local audio source.";
            results.monitorWarning = results.monitorWarning.empty()
                                         ? feedbackWarning
                                         : results.monitorWarning + " " + feedbackWarning;
            break;
          }
        }
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
  // Fetch the compositor's full-program 1080p NV12 tap once and share it with
  // both RTMP and the virtual camera. The tap owns the slow GPU work; this
  // worker only copies the latest ~3MB frame and never maps the render device.
  static std::vector<std::uint8_t> programNv12;  // output worker is single-threaded
  static int programNv12Width = 0;
  static int programNv12Height = 0;
  bool hasNewProgramNv12 = false;
  if (!outputDestinations.empty() || virtualCameraEnabled_) {
    int width = 0;
    int height = 0;
    hasNewProgramNv12 = modules_.compositor->takeVcamNv12(programNv12, width, height);
    if (hasNewProgramNv12) {
      programNv12Width = width;
      programNv12Height = height;
    }
  }
  auto outputProgramFrame = work.programFrame;
  if (!outputDestinations.empty() && !programNv12.empty() && programNv12Width > 0 && programNv12Height > 0) {
    outputProgramFrame.programNv12Width = programNv12Width;
    outputProgramFrame.programNv12Height = programNv12Height;
    outputProgramFrame.programNv12 = programNv12;
  }
  const auto tOut0 = std::chrono::steady_clock::now();
  try {
    // RTMP pacing must follow wall time, not ProgramFrame::frameNumber. The
    // audio/output worker runs at ~50 Hz, so the old frameNumber * 33 clock
    // advanced ~1.65 seconds per real second and pushed a declared 30 fps
    // stream at almost 50 fps. YouTube accepts the handshake, then closes that
    // over-speed ingest after a few seconds. Capture real monotonic time before
    // enqueueing into AsyncOutputSender so dropped work cannot accelerate it.
    static const auto outputClockEpoch = std::chrono::steady_clock::now();
    const double outputElapsedMs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - outputClockEpoch)
            .count());
    modules_.outputSender->sync(
        outputDestinations,
        &outputProgramFrame,
        outputElapsedMs,
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
  // Virtual Camera: publish the program frame to the SHM slot the OS reads.
  // Same per-tick output cadence as the network senders (no shared-lock pixel
  // work). No-op when the operator has not enabled the virtual camera.
  if (virtualCameraEnabled_ && hasNewProgramNv12) {
    try {
      virtualCamera_->publishNv12(programNv12.data(), programNv12Width, programNv12Height);
    } catch (...) {
    }
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
    const auto encoderSession = modules_.encoder->session();
    results.recordingAudioPacketsObserved = encoderSession.recordingAudioPacketCount;
    // Surface the encoder's recording warning (published into recordingWarning_
    // → snapshot recording.warning). Without this an audio WriteSample failure
    // lived only in encoderSession.warnings and the recording section looked
    // healthy while muxing a video-only MP4.
    results.recordingWarning = encoderSession.recordingWarning;
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
  audioMasteringRideDb_ = results.masteringRideDb;
  mixedAudioFrameCount_ = results.mixedFrameCount;
  audioCompGainReductionDbBySource_ = std::move(results.compGainReductionDbBySource);
  if (results.monitorTouched) {
    audioMonitorStatus_ = results.monitorStatus;
    audioMonitorWarning_ = results.monitorWarning;
    audioMonitorFramesPlayed_ += results.monitorFramesPlayedDelta;
    audioMonitorUnderruns_ = results.monitorUnderruns;
    audioMonitorFeedbackRisk_ = results.monitorFeedbackRisk;
  }
  if (results.recordingActive) {
    recordingProgramFramesWritten_ += results.recordingProgramFramesDelta;
    recordingIsoFramesWritten_ += results.recordingIsoFramesDelta;
    recordingAudioPacketsObserved_ = results.recordingAudioPacketsObserved;
    if (!results.recordingWarning.empty() && recordingWarning_ != results.recordingWarning) {
      // Encoder-side failure (e.g. dropped program audio) becomes the visible
      // recording warning. Log once per distinct warning — this is the loud
      // half of the guarantee that a video-only recording cannot look healthy.
      recordingWarning_ = results.recordingWarning;
      std::fprintf(stderr, "[recording] warning: %s\n", recordingWarning_.c_str());
    }
    recordingElapsedMs_ += results.recordingElapsedMsDelta;
  }
}

// Worker entry point: gather (brief coreMutex) â†’ work (audioOutputMutex_ only) â†’
// publish (brief coreMutex). The render thread takes ONLY coreMutex and so is never
// blocked by the long DSP/IO span; the worker never holds both locks at once.
void MediaCore::renderAudioOutputTick(std::mutex& coreMutex) {
  AudioOutputWorkItem work;
  {
    std::lock_guard<std::mutex> lock(coreMutex);
    // Increment 6 guardrail: gather/publish are the worker's only coreMutex
    // holds and are budgeted sub-ms â€” the long DSP/IO span below runs under
    // audioOutputMutex_ only. An over-budget hold here means blocking work
    // crept back under the big lock.
    ScopedLockHoldTimer holdTimer("audio.gather", LockHoldGuardrail::kDefaultBudgetUs);
    work = gatherAudioOutputWork();
  }
  AudioOutputResults results;
  {
    std::lock_guard<std::mutex> lock(audioOutputMutex_);
    results = runAudioOutputWork(work);
  }
  {
    std::lock_guard<std::mutex> lock(coreMutex);
    ScopedLockHoldTimer holdTimer("audio.publish", LockHoldGuardrail::kDefaultBudgetUs);
    publishAudioOutputResults(results);
  }
}

// ---- VST host P1 (docs/vst-host-spec.md) ------------------------------------

namespace {
std::string resolvePluginHostExecutablePath() {
  if (const char* fromEnv = std::getenv("COREVIDEO_PLUGIN_HOST_PATH"); fromEnv != nullptr && fromEnv[0] != '\0') {
    return fromEnv;
  }
#ifdef _WIN32
  char modulePath[MAX_PATH] = {};
  if (::GetModuleFileNameA(nullptr, modulePath, MAX_PATH) > 0) {
    std::string path(modulePath);
    const auto slash = path.find_last_of("\\/");
    if (slash != std::string::npos) {
      const std::string sibling = path.substr(0, slash + 1) + "corevideo-plugin-host.exe";
      if (::GetFileAttributesA(sibling.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return sibling;
      }
    }
  }
#endif
  return {};
}
}  // namespace

void MediaCore::startPluginHostScan() {
  {
    std::lock_guard<std::mutex> lock(pluginHostMutex_);
    if (pluginHostScanInFlight_) {
      return;
    }
    pluginHostScanInFlight_ = true;
    pluginHostStatus_ = "scanning";
  }

  // Detached: process spawn + directory walk can take hundreds of ms â€” never
  // inside cmd.handle's coreMutex budget. MediaCore lives for the process
  // lifetime, so capturing `this` is safe.
  std::thread([this] {
    const auto exePath = resolvePluginHostExecutablePath();
    std::string status;
    std::vector<PluginHostPluginInfo> plugins;
    if (exePath.empty()) {
      status = "absent";
    } else {
      const auto output = runPluginHostScan(exePath);
      if (output.empty()) {
        status = "error";
      } else {
        plugins = parsePluginScanOutput(output);
        status = "probing";
      }
    }

    {
      std::lock_guard<std::mutex> lock(pluginHostMutex_);
      pluginHostPlugins_ = plugins;
      pluginHostStatus_ = status;
      if (status != "probing") {
        pluginHostScanInFlight_ = false;
        return;
      }
    }

    // P2a: probe each plugin in ITS OWN host process (one crashing plugin
    // cannot poison the rest â€” nonzero exit IS the fail verdict), publishing
    // verdicts incrementally so the browser fills in as probes complete.
    // Still on this detached thread â€” never inside a lock domain.
    for (size_t index = 0; index < plugins.size(); ++index) {
      const auto probeOutput = runPluginHostProcess(exePath, {"--probe", plugins[index].id});
      const auto verdict = parsePluginProbeResult(probeOutput);

      std::lock_guard<std::mutex> lock(pluginHostMutex_);
      if (index < pluginHostPlugins_.size() && pluginHostPlugins_[index].id == plugins[index].id) {
        pluginHostPlugins_[index].probe = verdict.pass ? "pass" : "fail";
        if (!verdict.vendor.empty() && pluginHostPlugins_[index].vendor.empty()) {
          pluginHostPlugins_[index].vendor = verdict.vendor;
        }
        // P2c: the audio classes are what "vst:<name>" insert names select.
        pluginHostPlugins_[index].classNames = verdict.classNames;
      }
    }

    std::lock_guard<std::mutex> lock(pluginHostMutex_);
    pluginHostStatus_ = "ready";
    pluginHostScanInFlight_ = false;
  }).detach();
}

namespace {
int64_t pluginHostSteadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

void MediaCore::ensurePluginHostServeStarted() {
  bool expected = false;
  if (!pluginHostServeStarting_.compare_exchange_strong(expected, true)) {
    return;  // starter already ran (or is running)
  }
  // A1: respawns ride the house backoff ladder (5→10→20→40→60s, give up after
  // 5 consecutive failed retries) so a crash-on-load plugin cannot hot-loop
  // CreateProcess during a show. The policy lives under pluginHostMutex_ — a
  // tiny leaf the worker already takes per tick in resolveVstInsertForWorker.
  bool allowed = false;
  {
    std::lock_guard<std::mutex> lock(pluginHostMutex_);
    allowed = pluginHostRespawnPolicy_.requestStart(pluginHostSteadyNowMs());
    if (!allowed && pluginHostRespawnPolicy_.gaveUp() && !pluginHostGaveUpAnnounced_) {
      pluginHostGaveUpAnnounced_ = true;
      std::fprintf(stderr,
                   "[plugin-host] serve respawn GAVE UP after %d consecutive failures; VST inserts "
                   "stay BYPASSED (audio unprocessed) until the plug-in is re-selected\n",
                   pluginHostRespawnPolicy_.consecutiveFailures());
    }
  }
  if (!allowed) {
    pluginHostServeStarting_.store(false, std::memory_order_release);
    return;
  }
  // Detached: CreateProcess + kernel-object setup never runs in the audio
  // worker; the worker bypasses until ready() flips.
  std::thread([this] {
    const auto exePath = resolvePluginHostExecutablePath();
    bool launched = false;
    if (!exePath.empty()) {
      launched = pluginHostClient_.start(
          exePath, "serve-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    }
    {
      std::lock_guard<std::mutex> lock(pluginHostMutex_);
      pluginHostRespawnPolicy_.onLaunchResult(pluginHostSteadyNowMs(), launched);
    }
    if (!launched) {
      std::fprintf(stderr, "[plugin-host] serve launch FAILED (%s)\n",
                   exePath.empty() ? "corevideo-plugin-host.exe not found" : exePath.c_str());
    }
    // Allow a new starter after launch failure or a later isolated-host exit.
    // The audio worker observes ready()==false and requests a replacement;
    // program audio stays fail-open while that happens.
    pluginHostServeStarting_.store(false, std::memory_order_release);
  }).detach();
}

void MediaCore::openVstPluginEditor(const rpc::Json& command) {
  const std::string selectionName = command.getString("selection");
  const std::string query = vstSelectionQueryFromInsertName(selectionName);
  if (query.empty()) {
    std::lock_guard<std::mutex> lock(pluginHostMutex_);
    pluginHostInsertError_ = "cannot open controls: no VST3 plug-in selected";
    return;
  }
  {
    // Operator action: clicking "Open controls" resets the respawn ladder so a
    // gave-up host is always recoverable without an app restart (spec A1).
    std::lock_guard<std::mutex> lock(pluginHostMutex_);
    pluginHostRespawnPolicy_.reset();
    pluginHostGaveUpAnnounced_ = false;
  }
  const VstInsertSelection selection = resolveVstInsertForWorker(query);
  if (!selection.resolved) return;
  if (pluginHostClient_.ready() && pluginHostClient_.hostAlive()) {
    pluginHostClient_.requestEditor(selection.bundleId, selection.className);
    return;
  }
  ensurePluginHostServeStarted();
  // Opening controls is control-plane work, never the audio worker. Give the
  // isolated host a short launch window and then signal its dedicated editor
  // event; audio continues fail-open throughout.
  std::thread([this, selection] {
    for (int attempt = 0; attempt < 40; ++attempt) {
      if (pluginHostClient_.ready() && pluginHostClient_.hostAlive()) {
        pluginHostClient_.requestEditor(selection.bundleId, selection.className);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::lock_guard<std::mutex> lock(pluginHostMutex_);
    pluginHostInsertError_ = "isolated VST3 host did not start for the editor";
  }).detach();
}

VstInsertSelection MediaCore::resolveVstInsertForWorker(const std::string& query) {
  VstInsertSelection selection;
  bool kickScan = false;
  {
    std::lock_guard<std::mutex> lock(pluginHostMutex_);
    selection = resolveVstInsertSelection(query, pluginHostPlugins_);
    pluginHostInsertError_ = selection.resolved ? std::string{} : selection.error;
    if (selection.resolved) {
      // A NEW selection is an operator action: reset the respawn ladder so a
      // crash-looped previous plug-in never blocks trying a different one.
      const std::string selectionKey = selection.bundleId + "\x1f" + selection.className;
      if (selectionKey != pluginHostLastSelectionKey_) {
        pluginHostLastSelectionKey_ = selectionKey;
        pluginHostRespawnPolicy_.reset();
        pluginHostGaveUpAnnounced_ = false;
      }
    }
    if (!selection.resolved && pluginHostStatus_ == "absent" && !pluginHostScanAutoKicked_) {
      // No scan has ever run but the operator named a plugin: kick ONE scan
      // (async, detached) so the insert self-heals once results land.
      pluginHostScanAutoKicked_ = true;
      kickScan = true;
    }
  }
  if (kickScan) {
    startPluginHostScan();
  }
  if (!selection.resolved) {
    // Rate-capped (~5s at the 50Hz worker): loud, not spammy.
    static std::atomic<int> s_resolveLogTick{0};
    if (s_resolveLogTick.fetch_add(1, std::memory_order_relaxed) % 250 == 0) {
      std::fprintf(stderr, "[plugin-host] vst insert BYPASSED: %s\n", selection.error.c_str());
    }
  }
  return selection;
}

rpc::Json MediaCore::pluginHostState() const {
  std::lock_guard<std::mutex> lock(pluginHostMutex_);
  rpc::Json::Array plugins;
  for (const auto& plugin : pluginHostPlugins_) {
    rpc::Json::Array classNames;
    for (const auto& className : plugin.classNames) {
      classNames.emplace_back(className);
    }
    plugins.emplace_back(rpc::Json::Object{
        {"id", plugin.id},
        {"name", plugin.name},
        {"vendor", plugin.vendor},
        {"probe", plugin.probe},
        {"classNames", classNames},
    });
  }
  // P2c failure honesty: a core-side selection error (typo'd insert name, no
  // scan) outranks the host-side status; otherwise report what the host
  // actually did (active plugin, or its load/process error). A respawn
  // give-up outranks both — the insert is auto-bypassed and only an operator
  // action recovers it, so the message must never be masked (it is DERIVED
  // from policy state here, not stored, because the worker rewrites
  // pluginHostInsertError_ every tick).
  std::string lastError =
      !pluginHostInsertError_.empty() ? pluginHostInsertError_ : pluginHostClient_.lastError();
  if (pluginHostRespawnPolicy_.gaveUp()) {
    lastError = "isolated VST3 host crashed repeatedly; plug-in bypassed — re-select it or "
                "reopen its controls to retry";
  }
  return rpc::Json::Object{
      {"status", pluginHostStatus_},
      {"plugins", plugins},
      // P2b-2: the live serve transport, when running.
      {"serve", rpc::Json::Object{
                    {"running", pluginHostClient_.ready() && pluginHostClient_.hostAlive()},
                    {"exchanges", static_cast<double>(pluginHostClient_.exchanges())},
                    {"deadlineMisses", static_cast<double>(pluginHostClient_.deadlineMisses())},
                    {"activePlugin", pluginHostClient_.activePlugin()},
                    {"statusCode", static_cast<double>(pluginHostClient_.statusCode())},
                    {"lastError", lastError},
                    {"editorStatusCode", static_cast<double>(pluginHostClient_.editorStatusCode())},
                    {"editorActivePlugin", pluginHostClient_.editorActivePlugin()},
                    {"editorLastError", pluginHostClient_.editorLastError()},
                    // A1: respawn backoff telemetry (attempts = consecutive
                    // failed respawns; gaveUp = auto-bypassed until reset).
                    {"respawn", rpc::Json::Object{
                                    {"attempts", static_cast<double>(
                                                     pluginHostRespawnPolicy_.consecutiveFailures())},
                                    {"gaveUp", pluginHostRespawnPolicy_.gaveUp()},
                                }},
                }},
  };
}


}  // namespace corevideo::core
