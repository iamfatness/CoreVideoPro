#include "core/MediaCore.h"

#include "compositor/CompositorLayout.h"
#include "core/Protocol.h"
#include "modules/ProgramFramePreview.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <utility>

namespace corevideo::core {
namespace {

rpc::Json::Array stringArray(const std::vector<std::string>& values) {
  rpc::Json::Array result;
  for (const auto& value : values) {
    result.emplace_back(value);
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
      {"maxProgramResolution", "1920x1080"},
      {"maxProgramFps", 30},
      {"maxParticipantFeeds", 6},
      {"maxIsoRecordings", 2},
      {"capabilities", capabilityArray(renderer, encoderSession)},
  };
}

rpc::Json MediaCore::health() const {
  const auto session = modules_.encoder->session();
  return rpc::Json::Object{
      {"status", session.active ? "live" : "idle"},
      {"renderer", modules_.compositor->rendererName()},
      {"encoder", session.encoderName},
      {"codec", session.codec},
      {"targetBitrateMbps", session.targetBitrateMbps},
      {"hardwareEncoder", session.hardwareAccelerated},
      {"recordingArtifactPath", session.recordingArtifactPath},
      {"recordingBytesWritten", static_cast<double>(session.recordingBytesWritten)},
      {"frameCount", static_cast<double>(lastProgramFrame_.frameNumber)},
      {"encodedFrameCount", static_cast<double>(session.encodedFrameCount)},
      {"mixedAudioFrames", static_cast<double>(mixedAudioFrameCount_)},
      {"captureDeviceCount", static_cast<double>(modules_.captureDevice->enumerate().size())},
      {"messages", rpc::Json::Array{"COREVIDEO_STUB synthetic media path active"}},
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
        {"participants", rpc::Json::Array{}},
        {"tick", zoomSnapshotTick_},
    };
  }

  return rpc::Json::Object{
      {"meetingState", "in_meeting"},
      {"activeSpeakerId", "operator-1"},
      {"caption", ""},
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
      {"encoder", session.encoderName},
      {"codec", session.codec},
      {"hardwareEncoder", session.hardwareAccelerated},
      {"active", session.active},
      {"programFrameCount", static_cast<double>(lastProgramFrame_.frameNumber)},
      {"renderPlanId", lastProgramFrame_.renderPlanId},
      {"compositorRenderer", lastProgramFrame_.renderer},
      {"encoderSession", encoderSessionState(session)},
      {"outputSenderSession", outputSenderSessionState()},
      {"captureDevices", captureDevicesState()},
      {"health", health()},
      {"profile", profile()},
      {"audioMixSession", audioMixSessionState()},
      {"captionTrack", captionTrackState()},
      {"brandKit", brandKitState()},
      {"meetingState", resolveMeetingStateForSession()},
      {"breakoutRoomId", breakoutRoomId_},
      {"breakoutRoomName", breakoutRoomName_},
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

rpc::Json MediaCore::applyCommands(const rpc::Json::Array& commands) {
  for (const auto& command : commands) {
    (void)applyCommand(command);
  }
  renderSyntheticTick();
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
  } else if (type == "push-caption-cue") {
    pushCaptionCue(command);
  } else if (type == "set-caption-enabled") {
    setCaptionEnabled(command);
  } else if (type == "set-brand-kit") {
    setBrandKit(command);
  } else if (type == "simulate-breakout-room-change") {
    simulateBreakoutRoomChange(command);
  }
  return sessionState();
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

void MediaCore::loadSceneGraph(const rpc::Json& command) {
  sceneId_ = command.getString("sceneId", "unloaded");
  sceneRoutes_.clear();
  const rpc::Json* routes = command.get("routes");
  if (routes && routes->isArray()) {
    for (const auto& route : routes->asArray()) {
      SceneRouteState state;
      state.routeId = route.getString("routeId");
      state.mode = route.getString("mode");
      state.participantId = route.getString("participantId");
      state.audioRole = route.getString("audioRole");
      sceneRoutes_.push_back(std::move(state));
    }
  }
  routeCount_ = static_cast<int>(sceneRoutes_.size());
}

void MediaCore::setParticipantTransform(const rpc::Json&) {
  ++transformCount_;
}

void MediaCore::setOverlayAsset(const rpc::Json&) {
  ++overlayCount_;
}

void MediaCore::startProgramOutput(const rpc::Json& command) {
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
}

void MediaCore::startRecordingSession(const rpc::Json& command) {
  setRecordingTargets(command);
  recordingSessionId_ = command.getString("sessionId", recordingSessionId_.empty() ? "native-recording-session" : recordingSessionId_);
  recordingStartedAtMs_ = command.get("startedAtMs") ? command.get("startedAtMs")->asNumber() : recordingStartedAtMs_;
  recordingElapsedMs_ = 0;
  recordingStatus_ = "recording";
  recordingWriterStatus_ = "writing";
  recordingError_.clear();
  recordingWarning_.clear();
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
}

void MediaCore::recoverRecordingSession(const rpc::Json& command) {
  recordingStatus_ = recordingWarning_.empty() ? "recording" : "warning";
  recordingWriterStatus_ = recordingWarning_.empty() ? "writing" : "warning";
  recordingError_.clear();
  recordingWarning_ = command.getString("reason", recordingWarning_);
}

void MediaCore::syncParticipantAudioMix(const rpc::Json& command) {
  audioChannels_.clear();
  const rpc::Json* channels = command.get("channels");
  if (!channels || !channels->isArray()) {
    return;
  }
  for (const auto& channel : channels->asArray()) {
    ParticipantAudioChannelInput input;
    input.participantId = channel.getString("participantId");
    input.inputLevel = static_cast<int>(channel.get("inputLevel") ? channel.get("inputLevel")->asNumber() : 0);
    input.muted = channel.get("muted") && channel.get("muted")->asBool();
    input.noiseSuppression = channel.get("noiseSuppression") && channel.get("noiseSuppression")->asBool();
    if (const rpc::Json* manualGain = channel.get("manualGainDb")) {
      input.manualGainDb = manualGain->asNumber();
      input.hasManualGain = true;
    }
    if (!input.participantId.empty()) {
      audioChannels_.push_back(std::move(input));
    }
  }
  renderSyntheticTick();
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
  if (brandLogoText_.empty()) {
    brandLogoText_ = "CoreVideo Pro";
    brandWarnings_.push_back("Brand logo text was empty; using default bug label.");
  }
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

}  // namespace

rpc::Json MediaCore::audioMixSessionState() const {
  if (audioChannels_.empty()) {
    return rpc::Json::Object{
        {"status", "idle"},
        {"masterLevel", 0},
        {"loudnessLufs", -60},
        {"limiterActive", false},
        {"mixedFrameCount", static_cast<double>(mixedAudioFrameCount_)},
        {"participants", rpc::Json::Array{}},
        {"summary", "Audio mix idle."},
        {"warnings", rpc::Json::Array{}},
    };
  }

  rpc::Json::Array participants;
  rpc::Json::Array warnings;
  int masterTotal = 0;
  int audibleCount = 0;
  bool limiterActive = false;
  int boostingCount = 0;
  int duckingCount = 0;
  int mutedCount = 0;
  int manualCount = 0;

  for (const auto& channel : audioChannels_) {
    const int smartGainDb = calculateSmartGainDb(channel.inputLevel);
    const int gainDb = channel.muted ? -60 : static_cast<int>(clampDouble(smartGainDb + (channel.hasManualGain ? channel.manualGainDb : 0), -12, 12));
    const bool noiseSuppression = channel.noiseSuppression || channel.inputLevel < 35;
    const int outputLevel = channel.muted ? 0 : clampInt(channel.inputLevel + gainDb * 4, 0, 100);
    limiterActive = limiterActive || outputLevel >= 88;
    if (!channel.muted) {
      masterTotal += outputLevel;
      ++audibleCount;
    }
    if (gainDb > 0 && !channel.muted) ++boostingCount;
    if (gainDb < 0 && !channel.muted) ++duckingCount;
    if (channel.muted) ++mutedCount;
    if (channel.hasManualGain && channel.manualGainDb != 0) ++manualCount;
    if (noiseSuppression && channel.inputLevel < 35) {
      warnings.emplace_back("Noise suppression active on low-level sources.");
    }

    rpc::Json::Object participant{
        {"participantId", channel.participantId},
        {"inputLevel", channel.inputLevel},
        {"outputLevel", outputLevel},
        {"gainDb", gainDb},
        {"noiseSuppression", noiseSuppression},
        {"limiterActive", outputLevel >= 88},
        {"muted", channel.muted},
        {"status", audioStatusFor(channel.muted, gainDb)},
    };
    if (channel.hasManualGain) {
      participant.emplace("manualGainDb", channel.manualGainDb);
    }
    participants.emplace_back(std::move(participant));
  }

  const int masterLevel = audibleCount > 0 ? clampInt((masterTotal / audibleCount) + 8, 0, 100) : 0;
  limiterActive = limiterActive || masterLevel >= 88;
  std::ostringstream summary;
  if (boostingCount > 0) summary << boostingCount << " boosted";
  if (duckingCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << duckingCount << " ducked";
  if (mutedCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << mutedCount << " muted";
  if (manualCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << manualCount << " manual";
  const std::string summaryText = summary.tellp() > 0 ? summary.str() + " in program mix" : "Program mix balanced";

  return rpc::Json::Object{
      {"status", warnings.empty() ? "live" : "warning"},
      {"masterLevel", masterLevel},
      {"loudnessLufs", limiterActive ? -14 : -16},
      {"limiterActive", limiterActive},
      {"mixedFrameCount", static_cast<double>(mixedAudioFrameCount_)},
      {"participants", participants},
      {"summary", summaryText},
      {"warnings", warnings},
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
      {"appliedOverlayCount", appliedOverlayCount},
      {"summary", summary.str()},
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
    if (!sender.sendArtifactPath.empty()) {
      senderJson.emplace("sendArtifactPath", sender.sendArtifactPath);
      senderJson.emplace("sendBytesWritten", static_cast<double>(sender.sendBytesWritten));
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
  rpc::Json::Array streams{
      rpc::Json::Object{
          {"kind", "program"},
          {"path", recordingTargetFolder_ + "/" + recordingFilenamePrefix_ + "-program-0." + recordingFormat_},
          {"status", recordingWriterStatus_},
          {"expectedFrames", static_cast<double>(recordingProgramFramesWritten_ + recordingDroppedFrames_)},
          {"framesWritten", static_cast<double>(recordingProgramFramesWritten_)},
          {"missingFrames", 0},
          {"droppedFrames", static_cast<double>(recordingDroppedFrames_)},
          {"bytesWritten", static_cast<double>(recordingProgramFramesWritten_ * 260000)},
      },
  };
  for (const auto& participantId : isoIds) {
    streams.emplace_back(rpc::Json::Object{
        {"kind", "iso"},
        {"participantId", participantId},
        {"path", recordingTargetFolder_ + "/" + recordingFilenamePrefix_ + "-iso-" + participantId + "-0." + recordingFormat_},
        {"status", recordingWriterStatus_},
        {"readiness", "ready"},
        {"framesWritten", static_cast<double>(recordingProgramFramesWritten_)},
        {"bytesWritten", static_cast<double>(recordingProgramFramesWritten_ * 140000)},
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
      {"totalFramesWritten", static_cast<double>(recordingProgramFramesWritten_ + recordingIsoFramesWritten_)},
      {"totalDroppedFrames", static_cast<double>(recordingDroppedFrames_)},
      {"totalBytesWritten", static_cast<double>(std::max<int64_t>(session.recordingBytesWritten, recordingProgramFramesWritten_ * 260000 + recordingIsoFramesWritten_ * 140000))},
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
  return recording;
}

modules::CompositorRenderPlan MediaCore::buildCompositorRenderPlan(const std::vector<modules::VideoFrame>& videoFrames) const {
  modules::CompositorRenderPlan renderPlan;
  renderPlan.renderPlanId = sceneId_ + ":" + std::to_string(routeCount_) + ":" + std::to_string(overlayCount_);
  renderPlan.sceneId = sceneId_;
  renderPlan.width = 1920;
  renderPlan.height = 1080;
  renderPlan.fps = 30;

  int videoLayerIndex = 0;
  const int videoLayerCount = routeCount_ > 0 ? routeCount_ : static_cast<int>(videoFrames.size());
  if (!sceneRoutes_.empty()) {
    renderPlan.layers.reserve(static_cast<size_t>(sceneRoutes_.size() + overlayCount_));
    for (const auto& route : sceneRoutes_) {
      modules::CompositorRenderPlanLayer layer;
      layer.layerId = "route:" + route.routeId;
      layer.kind = route.mode == "screen-share" ? "screen-share" : "participant-video";
      layer.order = videoLayerIndex;
      if (!route.participantId.empty()) {
        layer.participantId = route.participantId;
        layer.sourceId = "zoom:" + route.participantId;
      } else if (videoLayerIndex < static_cast<int>(videoFrames.size())) {
        layer.participantId = videoFrames[static_cast<size_t>(videoLayerIndex)].participantId;
        layer.sourceId = "zoom:" + layer.participantId;
      }
      const auto layout = compositor::gridCell((std::max)(1, videoLayerCount), videoLayerIndex);
      layer.rect = {layout.x, layout.y, layout.width, layout.height};
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
  auto videoFrames = modules_.zoom->pollVideoFrames();
  if (zoomEngineRuntime_ && zoomEngineRuntime_->configured()) {
    const auto engineFrames = zoomEngineRuntime_->pollCompositorVideoFrames(static_cast<int64_t>(lastProgramFrame_.frameNumber + 1) * 33);
    if (!engineFrames.empty()) {
      videoFrames = engineFrames;
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
    recordingElapsedMs_ += 33;
  }
}

}  // namespace corevideo::core
