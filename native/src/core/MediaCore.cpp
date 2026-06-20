#include "core/MediaCore.h"

#include "compositor/CompositorLayout.h"
#include "core/Protocol.h"
#include "modules/ProgramFramePreview.h"
#include "modules/RealZoomCaptureSource.h"

#include <algorithm>
#include <array>
#include <cmath>
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

rpc::Json::Array stringArray(const std::vector<std::string>& values) {
  rpc::Json::Array result;
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return result;
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

bool playMonitorPulse(double volume, int masterLevel) {
#ifdef _WIN32
  if (volume <= 0.0 || masterLevel <= 0) {
    return false;
  }
  const auto frequency = static_cast<DWORD>(440 + std::min(420, masterLevel * 4));
  const auto durationMs = static_cast<DWORD>(12 + std::min(20, masterLevel / 4));
  return Beep(frequency, durationMs) != 0;
#else
  (void)volume;
  (void)masterLevel;
  return false;
#endif
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
  const auto encoderSession = modules_.encoder->session();
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
  const auto session = modules_.encoder->session();
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
  const auto session = modules_.encoder->session();
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
      {"mediaPlayback", mediaPlaybackState()},
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
  const auto recording = recordingState(session);
  if (!recording.isNull()) {
    state.emplace("recording", recording);
  }
  return state;
}

rpc::Json MediaCore::syncZoomMediaSpine(const rpc::Json& payload, double elapsedMs) {
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
  if (zoomEngineRuntime_ && zoomEngineRuntime_->configured()) {
    return zoomEngineRuntime_->drainFrameEvents();
  }
  return {};
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

void MediaCore::enqueueProgramFramePreviewEvent() {
  const auto event = modules::programFramePreviewEvent(lastProgramFrame_);
  if (!event.isNull()) {
    pendingProgramFramePreviewEvents_.emplace_back(event);
  }
}

void MediaCore::enqueueProgramSharedTextureEvent() {
  const auto event = modules::programSharedTextureEvent(lastProgramFrame_);
  if (!event.isNull()) {
    pendingProgramSharedTextureEvents_.emplace_back(event);
  }
}

rpc::Json MediaCore::applyCommands(const rpc::Json::Array& commands, double elapsedMs) {
  const auto frameNumberBefore = lastProgramFrame_.frameNumber;
  for (const auto& command : commands) {
    (void)applyCommand(command);
  }

  const int targetTicks = elapsedMs > 0.0 ? std::max(1, static_cast<int>(std::floor(elapsedMs / 33.0))) : 1;
  const auto ticksAlreadyRendered = static_cast<int>(lastProgramFrame_.frameNumber - frameNumberBefore);
  const int additionalTicks = elapsedMs > 0.0 ? std::max(0, targetTicks - ticksAlreadyRendered) : 1;
  for (int tick = 0; tick < additionalTicks; ++tick) {
    renderSyntheticTick();
  }
  return sessionState();
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
  } else if (type == "configure-srt-ingest-sources") {
    configureSrtIngestSources(command);
  } else if (type == "simulate-breakout-room-change") {
    simulateBreakoutRoomChange(command);
  }
  return sessionState();
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

namespace {
std::string normalizeVideoCodec(const std::string& codec, const std::string& fallback) {
  if (codec == "h264" || codec == "h265" || codec == "av1") {
    return codec;
  }
  if (codec == "hevc") {
    return "h265";
  }
  return fallback.empty() ? "h264" : fallback;
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
    overlayCount_ = static_cast<int>(overlayIds_.size());
    return;
  }

  overlayIds_.insert(overlayId);
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
    recordingOutputWidth_ = clampIntValue(static_cast<int>(recordingProfile->getNumber("width", recordingOutputWidth_)), 320, 3840);
    recordingOutputHeight_ = clampIntValue(static_cast<int>(recordingProfile->getNumber("height", recordingOutputHeight_)), 180, 2160);
    recordingOutputFps_ = clampIntValue(static_cast<int>(recordingProfile->getNumber("fps", recordingOutputFps_)), 1, 120);
    recordingTargetBitrateMbps_ = std::max(0.5, std::min(80.0, recordingProfile->getNumber("targetBitrateMbps", recordingTargetBitrateMbps_)));
    recordingVideoCodec_ = normalizeVideoCodec(recordingProfile->getString("codec", recordingVideoCodec_), recordingVideoCodec_);
  }
  configureEncoderRecordingRequest();
  modules_.encoder->start(command.getStringArray("destinations"), command.getStringArray("isoParticipantIds"));
  if (encoderLifecycleStatus_ == "idle" || encoderLifecycleStatus_ == "prepared" || encoderLifecycleStatus_ == "stopped") {
    encoderLifecycleStatus_ = "encoding";
    encoderLastTransition_ = "Program output encoder session started.";
  }
  renderSyntheticTick();
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
}

void MediaCore::failOutputSender(const rpc::Json& command) {
  modules_.outputSender->fail(command.getString("destination"), command.getString("message", "Output sender failed."), command.get("failedAtMs") ? command.get("failedAtMs")->asNumber() : 0);
}

void MediaCore::recoverOutputSender(const rpc::Json& command) {
  modules_.outputSender->recover(command.getString("destination"), command.get("recoveredAtMs") ? command.get("recoveredAtMs")->asNumber() : 0, command.getString("reason", ""));
}

void MediaCore::setRecordingTargets(const rpc::Json& command) {
  recordingTargetFolder_ = command.getString("targetFolder", recordingTargetFolder_);
  recordingFilenamePrefix_ = command.getString("filenamePrefix", recordingFilenamePrefix_);
  recordingFormat_ = command.getString("format", recordingFormat_);
  recordingQuality_ = command.getString("quality", recordingQuality_);
  if (command.get("isoParticipantIds")) {
    recordingIsoParticipantIds_ = command.getStringArray("isoParticipantIds");
  }
  configureEncoderRecordingRequest();
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
  configureEncoderRecordingRequest();
  auto encoderSession = modules_.encoder->session();
  auto destinations = encoderSession.destinations;
  if (std::find(destinations.begin(), destinations.end(), "recording") == destinations.end()) {
    destinations.push_back("recording");
  }
  modules_.encoder->start(destinations, recordingIsoParticipantIds_);
  if (encoderLifecycleStatus_ != "encoding") {
    encoderLifecycleStatus_ = "encoding";
    encoderLastTransition_ = "Recording session started encoder.";
  }
  renderSyntheticTick();
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
  renderSyntheticTick();
}

void MediaCore::syncAudioMonitor(const rpc::Json& command) {
  audioMonitorEnabled_ = command.get("enabled") && command.get("enabled")->asBool();
  audioMonitorDeviceId_ = command.getString("deviceId");
  audioMonitorDeviceName_ = command.getString("deviceName");
  audioMonitorVolume_ = std::max(0.0, std::min(1.0, command.getNumber("volume", audioMonitorVolume_)));
  audioMonitorWarning_.clear();

  if (!audioMonitorEnabled_) {
    audioMonitorStatus_ = "muted";
    return;
  }

  if (audioMonitorDeviceId_.empty()) {
    audioMonitorStatus_ = "missing-device";
    audioMonitorWarning_ = "Audio monitor is enabled but no render device is selected.";
    return;
  }

  audioMonitorStatus_ = mixedAudioFrameCount_ > 0 ? "playing" : "armed";
}

namespace {

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

  const rpc::Json* sources = command.get("sources");
  if (!sources || !sources->isArray()) {
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
    input.audioSyncOffsetMs = static_cast<int>(source.get("audioSyncOffsetMs")
                                                   ? source.get("audioSyncOffsetMs")->asNumber()
                                                   : 0);
    input.audioSyncOffsetMs = std::max(-500, std::min(500, input.audioSyncOffsetMs));
    captureAudioSources_.push_back(std::move(input));
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
    mediaPlaybackPlaying_ = false;
    mediaPlaybackWarnings_.push_back("Media playback command had no media asset id.");
    return;
  }

  const std::string mediaAssetName = command.getString("mediaAssetName");
  if (mediaAssetName.empty()) {
    mediaPlaybackWarnings_.push_back(mediaAssetId + " media asset has no name and may not be present in the media bin.");
  }

  mediaPlaybackAssetId_ = mediaAssetId;
  mediaPlaybackAssetName_ = mediaAssetName.empty() ? mediaAssetId : mediaAssetName;
  mediaPlaybackPlaying_ = command.get("playing") && command.get("playing")->asBool();
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

std::string protocolAudioStatusForDsp(const modules::AudioParticipantMixMetrics& participant) {
  if (participant.muted) return "muted";
  if (participant.gainDb > 0) return "boosting";
  if (participant.gainDb < 0) return "ducking";
  return "balanced";
}

}  // namespace

rpc::Json MediaCore::audioMixSessionState() const {
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
          {"summary", nativeMix.summary},
          {"warnings", warnings},
      };
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
        {"summary", "Audio mix idle."},
        {"warnings", audioMonitorWarning_.empty() ? rpc::Json::Array{} : rpc::Json::Array{audioMonitorWarning_}},
    };
  }

  rpc::Json::Array participants;
  rpc::Json::Array warnings;
  std::unordered_set<std::string> warningSet;
  int masterTotal = 0;
  int audibleCount = 0;
  bool limiterWouldReduce = false;
  int boostingCount = 0;
  int duckingCount = 0;
  int mutedCount = 0;
  int manualCount = 0;
  int soloCount = 0;
  int insertCount = 0;

  for (const auto& channel : audioChannels_) {
    const int smartGainDb = calculateSmartGainDb(channel.inputLevel);
    const int gainDb = channel.muted ? -60 : static_cast<int>(clampDouble(smartGainDb + (channel.hasManualGain ? channel.manualGainDb : 0), -12, 12));
    const bool noiseSuppression = channel.noiseSuppression || channel.inputLevel < 35;
    const int outputLevel = channel.muted ? 0 : clampInt(channel.inputLevel + gainDb * 4, 0, 100);
    const bool channelLimiterWouldReduce = outputLevel >= 88;
    limiterWouldReduce = limiterWouldReduce || channelLimiterWouldReduce;
    if (!channel.muted) {
      masterTotal += outputLevel;
      ++audibleCount;
    }
    if (gainDb > 0 && !channel.muted) ++boostingCount;
    if (gainDb < 0 && !channel.muted) ++duckingCount;
    if (channel.muted) ++mutedCount;
    if (channel.solo) ++soloCount;
    if (channel.hasManualGain && channel.manualGainDb != 0) ++manualCount;
    if (!channel.pluginInserts.empty()) {
      insertCount += static_cast<int>(channel.pluginInserts.size());
      if (warningSet.insert("vst-bridge-scan-only").second) {
        warnings.emplace_back("VST inserts are configured but live third-party plugin processing requires the dev VST bridge.");
      }
    }
    if (noiseSuppression && channel.inputLevel < 35 && warningSet.insert("low-level-noise-suppression").second) {
      warnings.emplace_back("Noise suppression active on low-level sources.");
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
        {"inputLevel", channel.inputLevel},
        {"outputLevel", outputLevel},
        {"gainDb", gainDb},
        {"pan", channel.pan},
        {"solo", channel.solo},
        {"noiseSuppression", noiseSuppression},
        {"limiterActive", audioLimiterEnabled_ && channelLimiterWouldReduce},
        {"muted", channel.muted},
        {"pluginInserts", pluginInserts},
        {"status", audioStatusFor(channel.muted, gainDb)},
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
  if (mutedCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << mutedCount << " muted";
  if (manualCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << manualCount << " manual";
  if (soloCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << soloCount << " solo";
  if (insertCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << insertCount << " inserts";
  const std::string summaryText = summary.tellp() > 0 ? summary.str() + " in program mix" : "Program mix balanced";
  if (!audioMonitorWarning_.empty()) {
    warnings.emplace_back(audioMonitorWarning_);
  }

  return rpc::Json::Object{
      {"status", warnings.empty() ? "live" : "warning"},
      {"masterLevel", masterLevel},
      {"loudnessLufs", limiterActive ? -14 : -16},
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
      {"summary", summaryText},
      {"warnings", warnings},
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
    for (const auto& bus : buses) {
      busSourceCounts.emplace_back(rpc::Json::Object{{"busId", bus}, {"sourceCount", 0}});
    }
    return rpc::Json::Object{
        {"status", audioRoutingWarnings_.empty() ? "idle" : "warning"},
        {"routedSendCount", 0},
        {"routedSourceCount", 0},
        {"busSourceCounts", busSourceCounts},
        {"sends", rpc::Json::Array{}},
        {"summary", audioRoutingSynced_ ? "No audio crosspoints routed." : "Audio routing matrix idle."},
        {"warnings", warnings},
    };
  }

  std::set<std::string> routedSources;
  std::map<std::string, std::set<std::string>> busSources;
  rpc::Json::Array sends;
  for (const auto& send : audioRoutingSends_) {
    routedSources.insert(send.sourceId);
    busSources[send.busId].insert(send.sourceId);
    sends.emplace_back(rpc::Json::Object{
        {"sourceId", send.sourceId},
        {"busId", send.busId},
        {"gainDb", send.gainDb},
    });
  }

  rpc::Json::Array busSourceCounts;
  int routedBusCount = 0;
  for (const auto& bus : buses) {
    const auto found = busSources.find(bus);
    const int sourceCount = found == busSources.end() ? 0 : static_cast<int>(found->second.size());
    if (sourceCount > 0) {
      ++routedBusCount;
    }
    busSourceCounts.emplace_back(rpc::Json::Object{{"busId", bus}, {"sourceCount", sourceCount}});
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
      {"sends", sends},
      {"summary", summary.str()},
      {"warnings", warnings},
  };
}

rpc::Json MediaCore::captureAudioSourcesState() const {
  rpc::Json::Array sources;
  int pairedCount = 0;
  for (const auto& source : captureAudioSources_) {
    if (!source.audioDeviceId.empty()) {
      ++pairedCount;
    }

    sources.emplace_back(rpc::Json::Object{
        {"captureDeviceId", source.captureDeviceId},
        {"audioDeviceId", source.audioDeviceId},
        {"audioDeviceName", source.audioDeviceName},
        {"audioSyncOffsetMs", source.audioSyncOffsetMs},
        {"paired", !source.audioDeviceId.empty()},
    });
  }

  std::ostringstream summary;
  summary << pairedCount << " of " << captureAudioSources_.size() << " capture source"
          << (captureAudioSources_.size() == 1 ? "" : "s") << " paired with microphone input.";

  return rpc::Json::Object{
      {"status", captureAudioSourcesSynced_ ? "ready" : "idle"},
      {"sourceCount", static_cast<int>(captureAudioSources_.size())},
      {"pairedCount", pairedCount},
      {"sources", sources},
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
      {"playing", mediaPlaybackPlaying_},
      {"summary", mediaPlaybackPlaying_ ? "Playing " + name + "." : name + " paused."},
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
  const auto senderSession = modules_.outputSender->session();
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
           {"metadataValid", metadataValid},
           {"containerFormat", containerFormat},
           {"videoCodec", videoCodec},
           {"audioCodec", audioCodec},
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
  if (!sceneRoutes_.empty()) {
    renderPlan.layers.reserve(static_cast<size_t>(sceneRoutes_.size() + overlayCount_));
    for (const auto& route : sceneRoutes_) {
      modules::CompositorRenderPlanLayer layer;
      layer.layerId = "route:" + route.routeId;
      layer.kind = route.mode == "screen-share" ? "screen-share" : "participant-video";
      layer.order = videoLayerIndex;
      if (route.mode == "capture-input" && !route.captureDeviceId.empty()) {
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

  for (int overlayIndex = 0; overlayIndex < overlayCount_; ++overlayIndex) {
    modules::CompositorRenderPlanLayer layer;
    layer.layerId = overlayIndex == 0 ? "overlay:lower-third" : "overlay:brand-bug";
    layer.kind = "overlay";
    layer.order = static_cast<int>(renderPlan.layers.size());
    const auto layout = overlayIndex == 0 ? compositor::lowerThirdOverlay() : compositor::topRightOverlay();
    layer.rect = {layout.x, layout.y, layout.width, layout.height};
    layer.opacity = 0.92f;
    renderPlan.layers.push_back(std::move(layer));
  }

  return renderPlan;
}

void MediaCore::renderSyntheticTick() {
  const auto frameIntervalMs = static_cast<int64_t>(std::max(1.0, std::round(1000.0 / std::max(1, outputFps_))));
  const auto frameTimestampMs = static_cast<int64_t>(lastProgramFrame_.frameNumber + 1) * frameIntervalMs;
  // Tap the latest decoded Zoom frames (real BGRA pixels) and ingest them into
  // the RealZoomCaptureSource so pollVideoFrames() returns pixels. Reading them
  // here does NOT drain the stdout/event queue that feeds the multiview tiles.
  auto* realZoom = dynamic_cast<modules::RealZoomCaptureSource*>(modules_.zoom.get());
  if (realZoom && zoomEngineRuntime_ && zoomEngineRuntime_->configured()) {
    const auto decoded = zoomEngineRuntime_->latestDecodedVideoFrames(frameTimestampMs);
    for (const auto& frame : decoded) {
      if (frame.hasPixels()) {
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
        const auto withPixels = std::find_if(
            videoFrames.begin(), videoFrames.end(), [&](const modules::VideoFrame& candidate) {
              return candidate.participantId == engineFrame.participantId && candidate.hasPixels();
            });
        if (withPixels != videoFrames.end()) {
          merged.push_back(*withPixels);
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
  auto audioFrames = modules_.zoom->pollAudioFrames();
  if (zoomEngineRuntime_ && zoomEngineRuntime_->configured()) {
    const auto engineAudioFrames = zoomEngineRuntime_->pollCompositorAudioFrames(static_cast<int64_t>(lastProgramFrame_.frameNumber + 1) * 20);
    if (!engineAudioFrames.empty()) {
      audioFrames = engineAudioFrames;
    }
  }
  mixedAudioFrameCount_ = modules_.mixer->mix(audioFrames);
  if (audioMonitorEnabled_ && !audioMonitorDeviceId_.empty() && audioMonitorVolume_ > 0.0 && mixedAudioFrameCount_ > 0) {
    const int monitorLevel = modules_.mixer->session().masterLevel > 0 ? modules_.mixer->session().masterLevel : 60;
    if (playMonitorPulse(audioMonitorVolume_, monitorLevel)) {
      audioMonitorStatus_ = "playing";
      ++audioMonitorFramesPlayed_;
    } else {
      audioMonitorStatus_ = "unavailable";
      audioMonitorWarning_ = "Native audio monitor could not open the default Windows playback path.";
    }
  } else if (audioMonitorEnabled_ && !audioMonitorDeviceId_.empty()) {
    audioMonitorStatus_ = "armed";
  }
  const auto renderPlan = buildCompositorRenderPlan(videoFrames);
  lastProgramFrame_ = modules_.compositor->render(renderPlan, videoFrames);
  if (lastProgramFrame_.preview.bgra.empty()) {
    fillSyntheticProgramFramePreview(lastProgramFrame_.preview, renderPlan, videoFrames, lastProgramFrame_);
  }
  enqueueProgramFramePreviewEvent();
  enqueueProgramSharedTextureEvent();
  modules_.encoder->submit(lastProgramFrame_);
  const auto session = modules_.encoder->session();
  modules_.outputSender->sync(session.destinations, &lastProgramFrame_, static_cast<double>(lastProgramFrame_.frameNumber * 33));
  if (recordingStatus_ == "recording" || recordingStatus_ == "warning") {
    ++recordingProgramFramesWritten_;
    const auto isoIds = recordingIsoParticipantIds_.empty() ? session.isoParticipantIds : recordingIsoParticipantIds_;
    recordingIsoFramesWritten_ += static_cast<int64_t>(isoIds.size());
    recordingAudioPacketsObserved_ += static_cast<int64_t>(audioFrames.size());
    recordingElapsedMs_ += static_cast<double>(frameIntervalMs);
  }
}

}  // namespace corevideo::core
