#include "core/BoundedAsyncLog.h"
#include "modules/RecordingQueuePolicy.h"
#include "core/MediaCore.h"
#include "core/AudioObservationFootprint.h"
#include "core/DeliveryTrace.h"
#include "modules/MonitorInputEvidence.h"
#include "core/SystemMemoryPolicy.h"

#include "compositor/CompositorLayout.h"
#include "compositor/AdvancedGrade.h"
#include "compositor/TilesLayout.h"
#include "compositor/TilesPinnedLayout.h"
#include "compositor/TilesMembership.h"
#include "core/AudioControlSourcePolicy.h"
#include "core/ControlCommandPolicy.h"
#include "contracts/Lifecycle.h"
#include "core/SourceAudioIngress.h"
#include "core/SourceVideoIngress.h"
#include "core/AvSyncClapProbe.h"
#include "core/ZoomBusRoster.h"
#include "core/LockHoldGuardrail.h"
#include "core/Protocol.h"
#include "core/RouteSourcePolicy.h"
#include "core/SpeakerFloor.h"
#include "modules/AudioDsp.h"
#include "modules/AsyncEncoderSink.h"
#include "modules/AsyncAudioMonitorOutput.h"
#include "core/SyntheticZoomSlateSource.h"
#include "modules/ProgramFramePreview.h"


#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <map>
#include <random>
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

#include "core/CoreStateCapture.h"
namespace corevideo::core {
namespace {

rpc::Json recordingWriteQueueJson(const modules::RecordingWriteQueueEvidence& q) {
  if (!q.observed) return nullptr;
  return rpc::Json::Object{
      {"depth", static_cast<double>(q.depth)}, {"highWater", static_cast<double>(q.highWater)},
      {"queued", static_cast<double>(q.queued)}, {"accepted", static_cast<double>(q.accepted)},
      {"completedCalls", static_cast<double>(q.completed)}, {"dropped", static_cast<double>(q.dropped)},
      {"startupDropped", static_cast<double>(q.startupDropped)},
      {"droppedAudio", static_cast<double>(q.droppedAudio)}, {"startupDroppedAudio", static_cast<double>(q.startupDroppedAudio)},
      {"queuedAudioSamples", static_cast<double>(q.queuedAudioSamples)},
      {"highWaterBytes", static_cast<double>(q.highWaterBytes)}, {"oldestQueuedAgeMs", static_cast<double>(q.oldestQueuedAgeMs)},
      {"lastOverflowReason", q.lastOverflowReason}};
}

constexpr int64_t kStaleCaptureAudioAgeMs = 1000;
// The stub Zoom session's (constant) directed speaker.
constexpr const char* kStubActiveSpeakerId = "operator-1";

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

int64_t monotonic100ns() {
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool avSyncTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("COREVIDEO_AV_SYNC_TRACE");
    return value != nullptr && std::string_view(value) == "1";
  }();
  return enabled;
}

// WASAPI's capture-client QPC timestamps are expressed in 100 ns units. Use
// that same clock domain for the Program texture publish marker.
int64_t avSyncQpc100ns() {
#ifdef _WIN32
  LARGE_INTEGER counter{}, frequency{};
  if (::QueryPerformanceCounter(&counter) && ::QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
    return static_cast<int64_t>((static_cast<long double>(counter.QuadPart) * 10'000'000.0L) /
                                static_cast<long double>(frequency.QuadPart));
  }
#endif
  return monotonic100ns();
}

// Key parameters are normalized: a value outside 0..1 is a caller bug, and
// clamping keeps the shader's smoothstep well-formed.
float clampUnit(double value) {
  return static_cast<float>(value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value));
}

float clampColorGradeAxis(double value) {
  return static_cast<float>(std::max(-100.0, std::min(100.0, value)));
}

modules::CompositorColorGrade readColorGrade(const rpc::Json& value) {
  auto result = modules::CompositorColorGrade{
      clampColorGradeAxis(value.getNumber("exposure", 0.0)),
      clampColorGradeAxis(value.getNumber("contrast", 0.0)),
      clampColorGradeAxis(value.getNumber("saturation", 0.0)),
      clampColorGradeAxis(value.getNumber("temperature", 0.0)),
      value.getString("lut", "none"),
  };
  if (const auto* advanced = value.get("advanced")) modules::readAdvancedGrade(*advanced, result.advanced);
  return result;
}

std::string normalizeVideoCodec(const std::string& codec, const std::string& fallback);

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
    destination.fpsExplicit = value.get("fps") != nullptr;
    destination.fps = static_cast<int>(value.getNumber("fps", destination.fps));
    destination.targetBitrateExplicit = value.get("targetBitrateMbps") != nullptr;
    destination.targetBitrateMbps = value.getNumber("targetBitrateMbps", destination.targetBitrateMbps);
    destination.audioBitrateKbps =
        std::max(32, std::min(512, static_cast<int>(value.getNumber("audioBitrateKbps", destination.audioBitrateKbps))));
    destination.videoCodecExplicit = value.get("videoCodec") != nullptr;
    destination.videoCodec = normalizeVideoCodec(value.getString("videoCodec", destination.videoCodec), destination.videoCodec);
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

struct CapabilityReport {
  rpc::Json::Array available;
  rpc::Json::Object states;
};

CapabilityReport capabilityReport(const modules::ModuleSet& modules, bool zoomConfigured,
                                  const modules::OutputSession& encoderSession) {
  CapabilityReport report;
  const auto add = [&](const std::string& name, modules::ModuleSet::CapabilityConstruction status) {
    rpc::Json::Object value{{"state", status.state}, {"detail", status.detail}};
    if (!status.failureScope.empty()) value.emplace("failureScope", status.failureScope);
    report.states.emplace(name, std::move(value));
    if (status.state == "available") report.available.emplace_back(name);
  };
  const auto intrinsic = [&](const char* name, bool constructed) {
    add(name, {constructed ? "available" : "omitted", "", ""});
  };
  const auto factory = [&](const char* name) {
    const auto found = modules.capabilityConstruction.find(name);
    add(name, found == modules.capabilityConstruction.end()
        ? modules::ModuleSet::CapabilityConstruction{"omitted", "not-reported-by-factory", ""}
        : found->second);
  };

  intrinsic("audio-mixer", modules.mixer != nullptr);
  intrinsic("scene-graph-rendering", modules.compositor != nullptr);
  intrinsic("dynamic-overlays", modules.compositor != nullptr);
  intrinsic("graphics-instances", modules.compositor && modules.compositor->supportsGraphicsInstances());
  factory("gpu-compositor");
  const bool gpu = modules.compositor && modules.compositor->rendererName() != "software";
  intrinsic("chroma-key", gpu);
  intrinsic("smart-framing", gpu);
  intrinsic("source-grade-preview", modules.compositor && modules.compositor->supportsGradePreview());
  factory("local-audio-capture");
  factory("audio-monitor-output");
  const char* zoomState = COREVIDEO_WITH_ZOOM && zoomConfigured ? "available" : "omitted";
  const char* zoomDetail = COREVIDEO_STUB ? "stub-build" : !COREVIDEO_WITH_ZOOM
      ? "zoom-sdk-not-built" : !zoomConfigured ? "engine-path-not-configured" : "";
  add("zoom-raw-video", {zoomState, zoomDetail, ""});
  add("zoom-raw-audio", {zoomState, zoomDetail, ""});
  intrinsic("program-recording", encoderSession.hardwareAccelerated);
  intrinsic("iso-recording", encoderSession.hardwareAccelerated);
  for (const char* name : {"rtmp-output", "ndi-output", "ndi-ingest", "srt-output", "hls-output",
                           "srt-ingest", "rtmp-ingest",
                           "decklink-capture", "aja-capture", "uvc-capture"}) factory(name);
  return report;
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
  if (device.decodedFrames) result.emplace("decodedFrames", static_cast<double>(*device.decodedFrames));
  if (device.isoCpuArrivals) {
    const auto& a = *device.isoCpuArrivals;
    result.emplace("isoCpuArrivals", rpc::Json::Object{
        {"sourceId", a.sourceId}, {"observed", static_cast<double>(a.observed)},
        {"demanded", a.demanded}, {"accepted", static_cast<double>(a.accepted)},
        {"drained", static_cast<double>(a.drained)}, {"overflowed", static_cast<double>(a.overflowed)},
        {"released", static_cast<double>(a.released)}, {"pending", static_cast<double>(a.pending)},
        {"bytes", static_cast<double>(a.bytes)}});
  }
  if (device.decodedAudioSamples) result.emplace("decodedAudioSamples", static_cast<double>(*device.decodedAudioSamples));
  if (device.lastFrameAgeMs) result.emplace("lastFrameAgeMs", static_cast<double>(*device.lastFrameAgeMs));
  if (device.decoderFailures) result.emplace("decoderFailures", static_cast<double>(*device.decoderFailures));
  if (device.codecDecodeErrors) result.emplace("codecDecodeErrors", static_cast<double>(*device.codecDecodeErrors));
  if (device.packetDecodeErrors) result.emplace("packetDecodeErrors", static_cast<double>(*device.packetDecodeErrors));
  if (device.vendor == "srt") {
    result.emplace("rttMs", device.rttMs ? rpc::Json(*device.rttMs) : rpc::Json(nullptr));
    result.emplace("rttStatus", device.rttMs ? "measured" : "unavailable: FFmpeg owns SRT socket");
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

// Meter floor for a channel with no PCM (digital silence). Every published
// rms/peak dBFS is MEASURED by the DSP off real PCM or sits at this floor;
// nothing is derived from a 0-100 level any more (#765).
constexpr double kAudioDbfsFloor = -120.0;

double round1Dbfs(double value) {
  return std::round(value * 10.0) / 10.0;
}

std::string protocolAudioStatusForDsp(const modules::AudioParticipantMixMetrics& participant) {
  if (participant.muted) return "muted";
  if (participant.noiseSuppressionActive) return "cleaning";
  if (participant.gainDb > 0) return "boosting";
  if (participant.gainDb < 0) return "ducking";
  return "balanced";
}


}
CoreStateCapture::CoreStateCapture(const MediaCore& core)
    : origin_(&core), modules_(core.modules_), pluginHostClient_(core.pluginHostClient_),
      mediaTransports_(core.mediaTransports_.get()), browserSources_(core.browserSources_.get()),
      virtualCamera_(core.virtualCamera_.get()), zoomEngineRuntime_(core.zoomEngineRuntime_.get()),
      renderWorkDistribution_(&core.renderWorkDistribution_), senderLifecycles_(&core.senderLifecycles_),
      projectionMutex_(&core.snapshotProjectionMutex_) {
  ScopedLockHoldTimer captureTimer("rpc.metadata-capture", LockHoldGuardrail::kDefaultBudgetUs);
  appliedControlRevision_ = core.appliedControlRevision_;
  graphicsState_ = core.graphicsInstances_.snapshot();
  resourceEventStaging_ = core.pendingResourceEvents_.facts();
  audioChannels_ = core.audioChannels_;
  audioCompGainReductionDbBySource_ = core.audioCompGainReductionDbBySource_;
  audioLimiterEnabled_ = core.audioLimiterEnabled_;
  audioMasteringRideDb_ = core.audioMasteringRideDb_;
  audioMonitorControlEpoch_ = core.audioMonitorControlEpoch_;
  audioMonitorControlResult_ = core.audioMonitorControlResult_;
  audioMonitorControlRevision_ = core.audioMonitorControlRevision_;
  audioMonitorDeviceId_ = core.audioMonitorDeviceId_;
  audioMonitorDeviceName_ = core.audioMonitorDeviceName_;
  audioMonitorEnabled_ = core.audioMonitorEnabled_;
  audioMonitorFeedbackRisk_ = core.audioMonitorFeedbackRisk_;
  audioMonitorFramesPlayed_ = core.audioMonitorFramesPlayed_;
  audioMonitorIntentRevision_ = core.audioMonitorIntentRevision_;
  audioMonitorOperationOrder_ = core.audioMonitorOperationOrder_;
  audioMonitorOperationResults_ = core.audioMonitorOperationResults_;
  audioMonitorQueueDroppedFrames_ = core.audioMonitorQueueDroppedFrames_;
  audioMonitorStatus_ = core.audioMonitorStatus_;
  audioMonitorUnderruns_ = core.audioMonitorUnderruns_;
  audioMonitorVolume_ = core.audioMonitorVolume_;
  audioMonitorWarning_ = core.audioMonitorWarning_;
  audioRouteControl_ = core.audioRouteControl_;
  audioRoutingSends_ = core.audioRoutingSends_;
  audioRoutingSynced_ = core.audioRoutingSynced_;
  audioRoutingWarnings_ = core.audioRoutingWarnings_;
  brandAccentColor_ = core.brandAccentColor_;
  brandBackgroundColor_ = core.brandBackgroundColor_;
  brandCaptionStyle_ = core.brandCaptionStyle_;
  brandColor_ = core.brandColor_;
  brandDefaultOverlayBehavior_ = core.brandDefaultOverlayBehavior_;
  brandFontFamily_ = core.brandFontFamily_;
  brandLogoText_ = core.brandLogoText_;
  brandLowerThirdStyle_ = core.brandLowerThirdStyle_;
  brandName_ = core.brandName_;
  brandWarnings_ = core.brandWarnings_;
  breakoutRoomId_ = core.breakoutRoomId_;
  breakoutRoomName_ = core.breakoutRoomName_;
  captionAtMs_ = core.captionAtMs_;
  captionEnabled_ = core.captionEnabled_;
  captionSpeaker_ = core.captionSpeaker_;
  captionText_ = core.captionText_;
  captionWarnings_ = core.captionWarnings_;
  captureAudioSourcesSynced_ = core.captureAudioSourcesSynced_;
  captureAudioSources_ = core.captureAudioSources_;
  commandProtocolFailures_ = core.commandProtocolFailures_;
  encoderLastTransition_ = core.encoderLastTransition_;
  encoderLifecycleStatus_ = core.encoderLifecycleStatus_;
  encoderPreparedAtMs_ = core.encoderPreparedAtMs_;
  encoderStartedAtMs_ = core.encoderStartedAtMs_;
  encoderStoppedAtMs_ = core.encoderStoppedAtMs_;
  masteringParams_ = core.masteringParams_;
  mediaPlaybackAssetId_ = core.mediaPlaybackAssetId_;
  mediaPlaybackAssetKind_ = core.mediaPlaybackAssetKind_;
  mediaPlaybackAssetName_ = core.mediaPlaybackAssetName_;
  mediaPlaybackAssetPath_ = core.mediaPlaybackAssetPath_;
  mediaPlaybackWarnings_ = core.mediaPlaybackWarnings_;
  mediaPreSumMetered_ = core.mediaPreSumMetered_;
  mediaPreSumPeakLevel_ = core.mediaPreSumPeakLevel_;
  mediaPreSumRmsLevel_ = core.mediaPreSumRmsLevel_;
  mixedAudioFrameCount_ = core.mixedAudioFrameCount_;
  monitorShed_ = core.monitorShed_;
  multiviewLayoutMode_ = core.multiviewLayoutMode_;
  multiviewShowClock_ = core.multiviewShowClock_;
  multiviewShowLabels_ = core.multiviewShowLabels_;
  multiviewShowMeters_ = core.multiviewShowMeters_;
  multiviewShowTally_ = core.multiviewShowTally_;
  multiviewSources_ = core.multiviewSources_;
  multiviewTileCount_ = core.multiviewTileCount_;
  outputDestinations_ = core.outputDestinations_;
  outputFps_ = core.outputFps_;
  outputHeight_ = core.outputHeight_;
  outputProfileId_ = core.outputProfileId_;
  outputResolution_ = core.outputResolution_;
  outputTargetBitrateMbps_ = core.outputTargetBitrateMbps_;
  outputWidth_ = core.outputWidth_;
  overlayAssets_ = core.overlayAssets_;
  overlayCount_ = core.overlayCount_;
  pendingTakeRecord_ = core.pendingTakeRecord_;
  previewOverlayCount_ = core.previewOverlayCount_;
  previewRouteCount_ = core.previewRouteCount_;
  previewSceneActive_ = core.previewSceneActive_;
  previewSceneId_ = core.previewSceneId_;
  recordingAudioBitrateKbps_ = core.recordingAudioBitrateKbps_;
  recordingError_ = core.recordingError_;
  recordingFailureCount_ = core.recordingFailureCount_;
  recordingFilenamePrefix_ = core.recordingFilenamePrefix_;
  recordingFormat_ = core.recordingFormat_;
  recordingIsoParticipantIds_ = core.recordingIsoParticipantIds_;
  recordingLastFailure_ = core.recordingLastFailure_;
  recordingLastRecovery_ = core.recordingLastRecovery_;
  recordingQuality_ = core.recordingQuality_;
  recordingQueueSettingWarning_ = core.recordingQueueSettingWarning_;
  recordingRecoveryCount_ = core.recordingRecoveryCount_;
  recordingSessionId_ = core.recordingSessionId_;
  recordingStartedAtMs_ = core.recordingStartedAtMs_;
  recordingStatus_ = core.recordingStatus_;
  recordingTargetFolder_ = core.recordingTargetFolder_;
  recordingWarning_ = core.recordingWarning_;
  recordingWriteQueueDepth_ = core.recordingWriteQueueDepth_;
  recordingWriterStatus_ = core.recordingWriterStatus_;
  renderedProgramSources_ = core.renderedProgramSources_;
  renderedSceneAttributionState_ = core.renderedSceneAttributionState_;
  renderedSceneAttributionTicks_ = core.renderedSceneAttributionTicks_;
  routeCount_ = core.routeCount_;
  sceneId_ = core.sceneId_;
  shellActiveSpeakerId_ = core.shellActiveSpeakerId_;
  shellScreenShareParticipantId_ = core.shellScreenShareParticipantId_;
  shellZoomRoster_ = core.shellZoomRoster_;
  sourcePolicies_ = core.sourcePolicies_;
  speakerFloor_ = core.speakerFloor_;
  takeRecords_ = core.takeRecords_;
  takeTransition_.operationId = core.takeTransition_.operationId;
  takeTransition_.revision = core.takeTransition_.revision;
  takeTransition_.mode = core.takeTransition_.mode;
  takeTransition_.durationMs = core.takeTransition_.durationMs;
  takeTransition_.elapsedMs = core.takeTransition_.elapsedMs;
  takeTransition_.waitingForColdMedia = core.takeTransition_.waitingForColdMedia;
  takeTransition_.active = core.takeTransition_.active;
  tilesLayer_.present = core.tilesLayer_.present;
  tilesLayer_.layerId = core.tilesLayer_.layerId;
  transformCount_ = core.transformCount_;
  zoomDisplayName_ = core.zoomDisplayName_;
  zoomGuestAvOffsets_ = core.zoomGuestAvOffsets_;
  zoomGuestAvRevision_ = core.zoomGuestAvRevision_;
  zoomJoined_ = core.zoomJoined_;
  zoomSnapshotTick_ = core.zoomSnapshotTick_;
  audioWorkerCompletedTicks_.value = core.audioWorkerCompletedTicks_.load(std::memory_order_relaxed);
  audioWorkerDiscardedTimelineNs_.value = core.audioWorkerDiscardedTimelineNs_.load(std::memory_order_relaxed);
  audioWorkerGeneration_.value = core.audioWorkerGeneration_.load(std::memory_order_relaxed);
  audioWorkerLastProgressNs_.value = core.audioWorkerLastProgressNs_.load(std::memory_order_relaxed);
  audioWorkerLostSamples_.value = core.audioWorkerLostSamples_.load(std::memory_order_relaxed);
  audioWorkerReanchors_.value = core.audioWorkerReanchors_.load(std::memory_order_relaxed);
  audioWorkerWorkMaximumNs_.value = core.audioWorkerWorkMaximumNs_.load(std::memory_order_relaxed);
  audioWorkerWorkTotalNs_.value = core.audioWorkerWorkTotalNs_.load(std::memory_order_relaxed);
  bufferedOutputSequenceGaps_.value = core.bufferedOutputSequenceGaps_.load(std::memory_order_relaxed);
  renderDeadlineMisses_.value = core.renderDeadlineMisses_.load(std::memory_order_relaxed);
  renderWorkerCompletedSlots_.value = core.renderWorkerCompletedSlots_.load(std::memory_order_relaxed);
  renderWorkerDeadlineMisses_.value = core.renderWorkerDeadlineMisses_.load(std::memory_order_relaxed);
  renderWorkerDrainMaximumNs_.value = core.renderWorkerDrainMaximumNs_.load(std::memory_order_relaxed);
  renderWorkerDrainTotalNs_.value = core.renderWorkerDrainTotalNs_.load(std::memory_order_relaxed);
  renderWorkerGeneration_.value = core.renderWorkerGeneration_.load(std::memory_order_relaxed);
  renderWorkerLastProgressNs_.value = core.renderWorkerLastProgressNs_.load(std::memory_order_relaxed);
  renderWorkerLockWaitMaximumNs_.value = core.renderWorkerLockWaitMaximumNs_.load(std::memory_order_relaxed);
  renderWorkerLockWaitTotalNs_.value = core.renderWorkerLockWaitTotalNs_.load(std::memory_order_relaxed);
  renderWorkerMaximumLatenessNs_.value = core.renderWorkerMaximumLatenessNs_.load(std::memory_order_relaxed);
  renderWorkerSkippedSlots_.value = core.renderWorkerSkippedSlots_.load(std::memory_order_relaxed);
  renderWorkerWorkMaximumNs_.value = core.renderWorkerWorkMaximumNs_.load(std::memory_order_relaxed);
  renderWorkerWorkTotalNs_.value = core.renderWorkerWorkTotalNs_.load(std::memory_order_relaxed);
  videoOutputWorkerCompletedTicks_.value = core.videoOutputWorkerCompletedTicks_.load(std::memory_order_relaxed);
  videoOutputWorkerGeneration_.value = core.videoOutputWorkerGeneration_.load(std::memory_order_relaxed);
  videoOutputWorkerLastProgressNs_.value = core.videoOutputWorkerLastProgressNs_.load(std::memory_order_relaxed);
  videoOutputWorkerWorkMaximumNs_.value = core.videoOutputWorkerWorkMaximumNs_.load(std::memory_order_relaxed);
  videoOutputWorkerWorkTotalNs_.value = core.videoOutputWorkerWorkTotalNs_.load(std::memory_order_relaxed);
  lastProgramFrame_.width = core.lastProgramFrame_.width;
  lastProgramFrame_.height = core.lastProgramFrame_.height;
  lastProgramFrame_.layerCount = core.lastProgramFrame_.layerCount;
  lastProgramFrame_.frameNumber = core.lastProgramFrame_.frameNumber;
  lastProgramFrame_.renderPlanId = core.lastProgramFrame_.renderPlanId;
  lastProgramFrame_.renderer = core.lastProgramFrame_.renderer;
  lastProgramFrame_.gpuComposed = core.lastProgramFrame_.gpuComposed;
  lastProgramFrame_.health = core.lastProgramFrame_.health;
  lastProgramFrame_.programPixelSignature = core.lastProgramFrame_.programPixelSignature;
  lastProgramFrame_.renderPlanSignature = core.lastProgramFrame_.renderPlanSignature;
  lastProgramFrame_.warnings = core.lastProgramFrame_.warnings;
  lastProgramFrame_.deliverySequence = core.lastProgramFrame_.deliverySequence;
  lastProgramFrame_.cpuSourceReadyOnly = core.lastProgramFrame_.cpuSourceReadyOnly;
  lastProgramFrame_.sourceAdmissions = core.lastProgramFrame_.sourceAdmissions;
  lastProgramFrame_.sharedTexture = core.lastProgramFrame_.sharedTexture;
  lastProgramFrame_.multiviewSharedTexture = core.lastProgramFrame_.multiviewSharedTexture;
  lastProgramFrame_.multiviewWidth = core.lastProgramFrame_.multiviewWidth;
  lastProgramFrame_.multiviewHeight = core.lastProgramFrame_.multiviewHeight;
  lastProgramFrame_.multiviewTiles = core.lastProgramFrame_.multiviewTiles;
  lastProgramFrame_.previewSharedTexture = core.lastProgramFrame_.previewSharedTexture;
  lastProgramFrame_.previewWidth = core.lastProgramFrame_.previewWidth;
  lastProgramFrame_.previewHeight = core.lastProgramFrame_.previewHeight;
  lastProgramFrame_.participantSharedTextures = core.lastProgramFrame_.participantSharedTextures;
  lastProgramFrame_.sharedTexture.publishedFrameNumber.reset();
  lastProgramFrame_.multiviewSharedTexture.publishedFrameNumber.reset();
  lastProgramFrame_.previewSharedTexture.publishedFrameNumber.reset();
  view_ = core.audioObservation();
  if (core.audioWorkerActive_) {
    std::lock_guard lock(core.encodedPreviewMutex_); encodedPreview_ = core.encodedPreview_;
  } else encodedPreview_ = modules::programFramePreviewJson(core.lastProgramFrame_);
  if (view_.payload) for (const auto& tap : view_.payload->value.busTaps) routedBusPcm_.emplace(tap.busId, tap);
  const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  sourceStatuses_ = core.sourceBus_ ? core.sourceBus_->snapshot(nowNs) : std::vector<SourceBus::SourceStatus>{};
  isoDiagnostics_ = core.isoVideoIngress_.counters();
  gradeEditorCount_ = core.gradePreviews_.size();
  gradeEventBytes_ = core.gradePreviews_.pendingEventBytes();
  gradeEventsRefused_ = core.gradePreviews_.eventsRefused();
  previewComposite_ = core.hasPreviewScene();
  previewPlanLayerCount_ = core.audioWorkerActive_ ? core.lastPreviewPlanLayerCount_ :
      previewComposite_ ? static_cast<int>(core.buildPreviewCompositorRenderPlan({}).layers.size()) : 0;
  for (const auto& layer : core.lastRenderPlan_.layers) {
    if (layer.kind != "participant-video" || layer.layerId.rfind("tile:", 0) != 0) continue;
    modules::CompositorRenderPlanLayer item;
    item.kind = layer.kind; item.layerId = layer.layerId; item.sourceId = layer.sourceId;
    item.participantId = layer.participantId; item.rect = layer.rect;
    lastRenderPlan_.layers.push_back(std::move(item));
  }

}
rpc::Json CoreStateCapture::assemble(SnapshotProjection kind) const {
  std::lock_guard<std::mutex> lock(*projectionMutex_);
  if (const auto* encoder = dynamic_cast<const modules::AsyncEncoderSink*>(modules_.encoder.get())) encoderEvidence_ = encoder->evidence();
  const AudioOutputObservation empty;
  const auto& observation = view_.payload ? view_.payload->value : empty;
  if (kind == SnapshotProjection::Health) return health(observation);
  if (kind == SnapshotProjection::Profile) return profile(observation);
  return sessionState();
}
const AudioOutputObservation::BusTap& CoreStateCapture::audioBusTapPcm(const std::string& id) const {
  static const AudioOutputObservation::BusTap empty;
  const auto found = routedBusPcm_.find(id); return found == routedBusPcm_.end() ? empty : found->second;
}
std::string CoreStateCapture::normalizeIsoSourceId(const std::string& raw) { return MediaCore::normalizeIsoSourceId(raw); }
rpc::Json CoreStateCapture::sessionState() const {
  // One immutable owner lease for every audio/output projection in this response.
  // Live readers never wait for DSP, device work or backend queries.
  const auto view = audioObservation();
  const AudioOutputObservation unobserved;
  const auto& observation = view.payload ? view.payload->value : unobserved;
  const auto& session = observation.encoder;
  const auto buffer = modules_.compositor->programBufferDiagnostics();
  rpc::Json::Array zoomGuestAvSync;
  for (const auto& [participantId, offsetMs] : zoomGuestAvOffsets_) {
    zoomGuestAvSync.emplace_back(rpc::Json::Object{
        {"participantId", participantId}, {"offsetMs", offsetMs}});
  }
  rpc::Json::Object state{
      {"sceneId", sceneId_},
      {"commandProtocolFailures", stringArray(commandProtocolFailures_)},
      {"routeCount", routeCount_},
      {"transformCount", transformCount_},
      {"overlayCount", overlayCount_},
      {"outputs", stringArray(session.destinations)},
      {"isoParticipantIds", stringArray(session.isoParticipantIds)},
      // ISO-3 parity: the canonical scheme-qualified selection (`zoom:<pid>` /
      // `capture:<id>`) alongside the legacy bare list, so the shell reads back
      // exactly the ids it can pass to isoSourceIds.
      {"isoSourceIds", stringArray(canonicalIsoSourceIds())},
      {"zoomGuestAvSync", std::move(zoomGuestAvSync)},
      {"zoomGuestAvSyncRevision", static_cast<double>(zoomGuestAvRevision_)},
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
           {"sceneId", renderedProgramSources_.sceneId()},
           // Always present, whatever the attribution is doing. "live" = this
           // scene id is attributed to a program frame delivered this tick;
           // "holding" = brief delivery jitter, the id is the last delivered
           // one; "unknown" = nothing has confirmed it and sceneId is empty.
           // See RenderedSceneAttributionPolicy.
           {"sceneIdAttribution", std::string(renderedSceneAttributionState_)},
           {"sceneIdAttributionTicks", renderedSceneAttributionTicks_},
           {"deliverySequence", static_cast<double>(lastProgramFrame_.deliverySequence)},
           {"videoSources", renderedProgramSources_.videoSources()},
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
      {"compositor",
       rpc::Json::Object{
           {"status", lastProgramFrame_.frameNumber > 0 ? "live" : "idle"},
           {"renderPlanId", lastProgramFrame_.renderPlanId},
           {"programFrameCount", static_cast<double>(lastProgramFrame_.frameNumber)},
           {"droppedFrameCount", static_cast<double>(buffer.activeFrames > 0 ?
               buffer.underruns : renderDeadlineMisses_.load(std::memory_order_relaxed))},
           {"renderDeadlineMisses", static_cast<double>(renderDeadlineMisses_.load(std::memory_order_relaxed))},
           {"degradedFrameCount", lastProgramFrame_.health == "degraded" ? 1 : 0},
       }},
      // Both unconditional. A take ledger that appears only after a Take, or a
      // subscription node that appears only once churn happens, is absent in
      // exactly the case worth detecting (the multiviewer-node lesson).
      {"takeRecords", takeRecordsState()},
      {"zoomSubscriptionChurn", zoomSubscriptionChurnState()},
      {"encoderSession", encoderSessionState(session)},
      {"outputSenderSession", outputSenderSessionState(observation)},
      {"virtualCamera", virtualCameraState()},
      {"captureDevices", captureDevicesState()},
      {"browserSources", browserSourcesState()},
      {"health", health(observation)},
      {"profile", profile(observation)},
      {"audioMixSession", audioMixSessionState(observation)},
      {"audioRoutingMatrix", audioRoutingMatrixState()},
      {"captureAudioSources", captureAudioSourcesState(observation)},
      {"captionTrack", captionTrackState()},
      {"brandKit", brandKitState()},
      {"overlayState", overlayState()},
      {"graphics", graphicsState_},
      {"mediaPlayback", mediaPlaybackState()},
      {"autoProduction", autoProductionState()},
      {"takeTransition", rpc::Json::Object{
          {"operationId", takeTransition_.operationId},
          {"revision", static_cast<double>(takeTransition_.revision)},
          {"mode", takeTransition_.mode},
          {"durationMs", takeTransition_.durationMs},
          {"progress", takeTransition_.durationMs > 0.0
              ? (std::min)(1.0, takeTransition_.elapsedMs / takeTransition_.durationMs)
              : 1.0},
          {"status", takeTransition_.operationId.empty() ? "idle" :
              takeTransition_.waitingForColdMedia ? "warming" :
              takeTransition_.active ? "active" : "completed"},
      }},
      {"meetingState", resolveMeetingStateForSession()},
      {"breakoutRoomId", breakoutRoomId_},
      {"breakoutRoomName", breakoutRoomName_},
      {"zoom", rpc::Json::Object{{"readiness", zoomReadinessState()}, {"evidence", zoomEvidenceState()}}},
  };
  state.emplace("audioObservation", audioObservationMetadata(view, audioMonitorControlEpoch_));
  state.emplace("appliedControlRevision", static_cast<double>(appliedControlRevision_));
  state.emplace("programBuffer", rpc::Json::Object{
      {"requestedFrames", buffer.requestedFrames}, {"activeFrames", buffer.activeFrames},
      {"generation", static_cast<double>(buffer.generation)},
      {"status", buffer.status}, {"capacity", buffer.capacity}, {"occupancy", buffer.occupancy},
      {"produced", static_cast<double>(buffer.produced)},
      {"delivered", static_cast<double>(buffer.delivered)},
      {"underruns", static_cast<double>(buffer.underruns)},
      {"overflows", static_cast<double>(buffer.overflows)},
      {"gpuNotReady", static_cast<double>(buffer.gpuNotReady)},
      {"deadlineMisses", static_cast<double>(buffer.deadlineMisses)},
      {"prepared", static_cast<double>(buffer.prepared)},
      {"lastQueueWaitMs", buffer.lastQueueWaitMs},
      {"maxQueueWaitMs", buffer.maxQueueWaitMs},
      {"lastPreparationMs", buffer.lastPreparationMs},
      {"maxPreparationMs", buffer.maxPreparationMs},
      {"displayUnconsumed", static_cast<double>(buffer.displayUnconsumed)},
      {"displayBusy", static_cast<double>(buffer.displayBusy)},
      {"outputSequenceGaps", static_cast<double>(bufferedOutputSequenceGaps_.load())},
      {"displayPresentationVerified", false}, {"destinationCompletionVerified", false}});
  const auto logStats = nativeLogStats();
  state.emplace("nativeDiagnostics", rpc::Json::Object{
      {"logDropped", static_cast<double>(logStats.dropped)},
      {"logTruncated", static_cast<double>(logStats.truncated)},
      {"logSinkFailures", static_cast<double>(logStats.sinkFailures)},
      {"logQueued", static_cast<double>(logStats.queued)}});
  if (encoderEvidence_) {
    const auto& encoderEvidence = *encoderEvidence_;
    static constexpr std::array<const char*, 7> evidenceKinds{
        "configure", "start", "programVideo", "isoVideo", "programAudio", "isoAudio", "stop"};
    rpc::Json::Object enqueued, completed, queued;
    for (size_t i = 0; i < evidenceKinds.size(); ++i) {
      enqueued.emplace(evidenceKinds[i], static_cast<double>(encoderEvidence.enqueued[i]));
      completed.emplace(evidenceKinds[i], static_cast<double>(encoderEvidence.completedCalls[i]));
      queued.emplace(evidenceKinds[i], static_cast<double>(encoderEvidence.queuedByKind[i]));
    }
    // ISO-3 (fidelity): per-source distinct-frame accounting for ISO video, so
    // an ISO stem's repeat-freeness is MEASURABLE. `framesWritten` on a stream
    // is an append count and cannot tell a distinct picture from a repeat, which
    // left any change to the ISO cadence unverifiable. The arrival-side numbers
    // (queued / heldFrameSuppressed / queueOverflowed) come from the render
    // gather's ISO queue; the sink-side numbers (submitted / duplicateRejected /
    // dropped / written) come from the async encoder. Read together they are the
    // whole chain from decoded frame to muxed frame.
    std::map<std::string, rpc::Json::Object> isoVideoNodes;
    {
      for (const auto& entry : isoDiagnostics_) {
        auto& node = isoVideoNodes[entry.first];
        node.emplace("queued", static_cast<double>(entry.second.distinctSubmitted));
        node.emplace("heldFrameSuppressed", static_cast<double>(entry.second.duplicateRejected));
        node.emplace("queueOverflowed", static_cast<double>(entry.second.queueOverflowed));
      }
    }
    for (const auto& entry : encoderEvidence.isoVideoBySource) {
      auto& node = isoVideoNodes[entry.first];
      node.emplace("submitted", static_cast<double>(entry.second.submitted));
      node.emplace("duplicateRejected", static_cast<double>(entry.second.duplicateRejected));
      node.emplace("dropped", static_cast<double>(entry.second.dropped));
      node.emplace("written", static_cast<double>(entry.second.written));
    }
    rpc::Json::Object isoVideoBySource;
    for (auto& entry : isoVideoNodes) {
      isoVideoBySource.emplace(entry.first, std::move(entry.second));
    }
    state.emplace("encoderEvidence", rpc::Json::Object{
        {"metricVersion", "async-encoder-evidence-v1"},
        {"generation", static_cast<double>(encoderEvidence.generation)},
        {"lifecycleState", encoderEvidence.lifecycleState},
        {"operation", encoderEvidence.operation},
        {"operationGeneration", static_cast<double>(encoderEvidence.operationGeneration)},
        {"operationSequence", static_cast<double>(encoderEvidence.operationSequence)},
        {"operationAgeMs", static_cast<double>(encoderEvidence.operationAgeMs)},
        {"queueDepth", static_cast<double>(encoderEvidence.queueDepth)},
        {"oldestQueuedAgeMs", static_cast<double>(encoderEvidence.oldestQueuedAgeMs)},
        {"enqueued", std::move(enqueued)}, {"completedCalls", std::move(completed)},
        {"queuedByKind", std::move(queued)},
        {"droppedVideo", static_cast<double>(encoderEvidence.droppedVideo)},
        {"droppedAudio", static_cast<double>(encoderEvidence.droppedAudio)},
        // Kept OUT of droppedVideo on purpose: video shed behind the writer's
        // SYNCHRONOUS open is a clipped head, not steady-state loss. Reported
        // here so it stays visible and attributable to the startup window.
        {"startupDroppedVideo", static_cast<double>(encoderEvidence.startupDroppedVideo)},
        {"isoVideoBySource", std::move(isoVideoBySource)},
        {"lastWriterProgressMs", static_cast<double>(encoderEvidence.lastWriterProgressMs)},
        {"programVideoWritten", static_cast<double>(encoderEvidence.programVideoWritten)},
        {"programAudioPacketsWritten", static_cast<double>(encoderEvidence.programAudioPacketsWritten)},
        {"writtenGeneration", static_cast<double>(encoderEvidence.writtenGeneration)},
        {"stopGeneration", static_cast<double>(encoderEvidence.stopGeneration)},
        {"stopRequestedMs", static_cast<double>(encoderEvidence.stopRequestedMs)},
        {"finalizeStartedMs", static_cast<double>(encoderEvidence.finalizeStartedMs)},
        {"finalizeFinishedMs", static_cast<double>(encoderEvidence.finalizeFinishedMs)},
        {"finalizeResult", encoderEvidence.finalizeResult},
        {"finalizeTimeouts", static_cast<double>(encoderEvidence.finalizeTimeouts)},
        {"firstFailure", encoderEvidence.firstFailure},
        {"firstFailureGeneration", static_cast<double>(encoderEvidence.firstFailureGeneration)},
        {"firstFailureMs", static_cast<double>(encoderEvidence.firstFailureMs)}});
  }
  const auto evidenceNowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  const auto progressAgeMs = [evidenceNowNs](int64_t lastProgressNs) -> double {
    return lastProgressNs > 0
        ? static_cast<double>((std::max)(int64_t{0}, evidenceNowNs - lastProgressNs)) / 1'000'000.0
        : -1.0;
  };
  const auto renderLastProgressNs = renderWorkerLastProgressNs_.load(std::memory_order_relaxed);
  const auto audioLastProgressNs = audioWorkerLastProgressNs_.load(std::memory_order_relaxed);
  const auto videoOutputLastProgressNs = videoOutputWorkerLastProgressNs_.load(std::memory_order_relaxed);
  // Machine sampling belongs to the observation owner, never a live status read.
  state.emplace("systemMemory", rpc::Json::Object{
      {"measured", observation.memory.measured},
      {"level", systemMemoryLevelName(observation.memoryLevel)},
      {"commitAvailableMb", static_cast<double>(observation.memory.commitAvailableBytes / (1024 * 1024))},
      {"commitLimitMb", static_cast<double>(observation.memory.commitLimitBytes / (1024 * 1024))},
      {"sampledAtNs", static_cast<double>(observation.memorySampledAtNs)}});
  const auto capturePreparation = modules_.captureDevice->shmCapturePreparationDiagnostics();
  const auto monitorWorker = modules_.compositor->monitorDiagnostics();
  state.emplace("gradePreview", GradePreviewController::diagnosticsFor(*modules_.compositor, gradeEditorCount_, gradeEventBytes_, gradeEventsRefused_));
  state.emplace("deliveryEvidence", core::deliveryEvidenceSnapshot());
  state.emplace("realtimeEvidence", rpc::Json::Object{
      {"resourceEventStaging", rpc::Json::Object{
          {"version", "resource-event-staging-v1"},
          {"items", double(resourceEventStaging_.items)}, {"ownedByteBound", double(resourceEventStaging_.bytes)},
          {"itemCapacity", double(ResourceEventStaging::Capacity)}, {"byteCapacity", double(ResourceEventStaging::ByteBudget)},
          {"admitted", double(resourceEventStaging_.admitted)}, {"refused", double(resourceEventStaging_.refused)}}},
      {"capturePreparation", rpc::Json::Object{
          {"kind", "winui-shared-memory"}, {"version", "shm-preparation-v1"},
          {"enabled", capturePreparation.enabled}, {"state", capturePreparation.state},
          {"reason", capturePreparation.reason},
          {"lastRefusalReason", capturePreparation.lastRefusalReason},
          {"accepted", static_cast<double>(capturePreparation.accepted)},
          {"refused", static_cast<double>(capturePreparation.refused)},
          {"prepared", static_cast<double>(capturePreparation.prepared)},
          {"torn", static_cast<double>(capturePreparation.torn)},
          {"poolBusy", static_cast<double>(capturePreparation.poolBusy)},
          {"failed", static_cast<double>(capturePreparation.failed)},
          {"residentBytes", static_cast<double>(capturePreparation.residentBytes)},
          {"memoryAccounting", "mapped-payload-plus-four-cpu-frames"},
          {"budgetBytes", static_cast<double>(capturePreparation.budgetBytes)},
          {"active", static_cast<double>(capturePreparation.active)},
          {"retiring", static_cast<double>(capturePreparation.retiring)},
          {"copyTotalNs", static_cast<double>(capturePreparation.copyTotalNs)},
          {"copyMaximumNs", static_cast<double>(capturePreparation.copyMaximumNs)},
          {"senderAcquisitionTimeVerified", false}}},
      {"monitorWorker", rpc::Json::Object{
          {"enabled", monitorWorker.enabled},
          {"observationVersion", "monitor-isolation-v1"},
          {"requestedMode", monitorWorker.requestedMode},
          {"effectiveMode", monitorWorker.effectiveMode},
          {"selectionSource", monitorWorker.selectionSource},
          {"readiness", monitorWorker.readiness},
          {"failureReason", monitorWorker.failureReason},
          {"deliveryEpoch", static_cast<double>(buffer.generation)},
          {"submitted", static_cast<double>(monitorWorker.submitted)},
          {"completed", static_cast<double>(monitorWorker.completed)},
          {"superseded", static_cast<double>(monitorWorker.superseded)},
          {"failed", static_cast<double>(monitorWorker.failed)},
          {"pending", monitorWorker.pending}, {"capacity", 1},
          {"lastSequence", static_cast<double>(monitorWorker.lastSequence)},
          {"lastWorkMs", monitorWorker.lastWorkMs},
          {"inputObservationVersion", "monitor-input-admission-v1"},
          {"inputs", modules::monitorInputEvidence(modules_.compositor->latestMonitors())},
          {"programDisplay", modules::programDisplayEvidence(modules_.compositor->latestMonitors())},
          {"readyInputs", static_cast<double>(monitorWorker.readyInputs)},
          {"heldInputs", static_cast<double>(monitorWorker.heldInputs)},
          {"unavailableInputs", static_cast<double>(monitorWorker.unavailableInputs)},
          {"retainedInputs", static_cast<double>(monitorWorker.retainedInputs)},
          {"retainedInputBytes", static_cast<double>(monitorWorker.retainedInputBytes)},
          {"retentionRefusals", static_cast<double>(monitorWorker.retentionRefusals)},
          {"inputCapacity", static_cast<double>(modules::MonitorRenderDiagnostics::InputCapacity)},
          {"inputByteBudget", static_cast<double>(modules::MonitorRenderDiagnostics::InputByteBudget)},
          {"displayPresentationVerified", false}}},
      {"metricVersion", "realtime-worker-evidence-v1"},
      {"render", rpc::Json::Object{
          {"generation", static_cast<double>(renderWorkerGeneration_.load(std::memory_order_relaxed))},
          {"observed", renderLastProgressNs > 0},
          {"progressAgeMs", progressAgeMs(renderLastProgressNs)},
          {"completedSlots", static_cast<double>(renderWorkerCompletedSlots_.load(std::memory_order_relaxed))},
          {"skippedSlots", static_cast<double>(renderWorkerSkippedSlots_.load(std::memory_order_relaxed))},
          {"deadlineMisses", static_cast<double>(renderWorkerDeadlineMisses_.load(std::memory_order_relaxed))},
          {"maximumLatenessNs", static_cast<double>(renderWorkerMaximumLatenessNs_.load(std::memory_order_relaxed))},
          {"lockWaitTotalNs", static_cast<double>(renderWorkerLockWaitTotalNs_.load(std::memory_order_relaxed))},
          {"lockWaitMaximumNs", static_cast<double>(renderWorkerLockWaitMaximumNs_.load(std::memory_order_relaxed))},
          {"workTotalNs", static_cast<double>(renderWorkerWorkTotalNs_.load(std::memory_order_relaxed))},
          {"workMaximumNs", static_cast<double>(renderWorkerWorkMaximumNs_.load(std::memory_order_relaxed))},
          {"workDistribution", renderWorkDistribution_->snapshot()},
          {"eventDrainTotalNs", static_cast<double>(renderWorkerDrainTotalNs_.load(std::memory_order_relaxed))},
          {"eventDrainMaximumNs", static_cast<double>(renderWorkerDrainMaximumNs_.load(std::memory_order_relaxed))},
          {"gpuCompletionVerified", false}, {"deliveryVerified", false}}},
      // T1.4 monitor load-shedding (core/MonitorShedPolicy.h). Published
      // unconditionally — divisor 1 / level 0 is the healthy state, and a node
      // that only appeared while shedding would be absent in exactly the case
      // worth comparing against.
      {"monitorShed", rpc::Json::Object{
          {"divisor", monitorShed_.divisor()},
          {"level", monitorShed_.level()},
          {"enteredCount", static_cast<double>(monitorShed_.enteredCount())},
          {"shedTicks", static_cast<double>(monitorShed_.shedTicks())},
          {"lastReason", std::string(monitorShed_.lastReason())},
          // The observation that caused the last divisor change: a shed with a
          // big monitorCycleMs was monitor-bound; one with a big programMs was
          // Program- or GPU-bound (see MonitorShedPolicy.h — both shed, by design).
          {"lastTransitionProgramMs",
           static_cast<double>(monitorShed_.lastTransitionObservation().programCostNs) / 1e6},
          {"lastTransitionMonitorCycleMs",
           static_cast<double>(monitorShed_.lastTransitionObservation().monitorCycleCostNs) / 1e6},
          {"lastTransitionBudgetMs",
           static_cast<double>(monitorShed_.lastTransitionObservation().budgetNs) / 1e6}}},
      // Stream congestion no longer changes the compositor's export cadence.
      // These are the actual compositor counters, published once for the
      // encoder texture; per-destination queue pressure lives on each sender.
      {"encoderExport", rpc::Json::Object{
          {"divisor", modules_.compositor ? modules_.compositor->encoderExportDivisor() : 1},
          {"shedFrames", static_cast<double>(
              modules_.compositor ? modules_.compositor->encoderExportShedFrames() : 0)},
          {"exporting", modules_.compositor ? modules_.compositor->encoderExporting() : false}}},
      {"audio", rpc::Json::Object{
          {"generation", static_cast<double>(audioWorkerGeneration_.load(std::memory_order_relaxed))},
          {"observed", audioLastProgressNs > 0},
          {"progressAgeMs", progressAgeMs(audioLastProgressNs)},
          {"completedTicks", static_cast<double>(audioWorkerCompletedTicks_.load(std::memory_order_relaxed))},
          {"workTotalNs", static_cast<double>(audioWorkerWorkTotalNs_.load(std::memory_order_relaxed))},
          {"workMaximumNs", static_cast<double>(audioWorkerWorkMaximumNs_.load(std::memory_order_relaxed))},
          {"pacerReanchors", static_cast<double>(audioWorkerReanchors_.load(std::memory_order_relaxed))},
          {"discardedTimelineNs", static_cast<double>(audioWorkerDiscardedTimelineNs_.load(std::memory_order_relaxed))},
          // Cumulative interleaved PCM samples permanently shed at the feed
          // FIFO cap, summed over every source. Monotonic for the life of the
          // process; the qualification harness fails on any reset.
          {"audioLostSamples", static_cast<double>(audioWorkerLostSamples_.load(std::memory_order_relaxed))}}},
      {"videoOutput", rpc::Json::Object{
          {"generation", static_cast<double>(videoOutputWorkerGeneration_.load(std::memory_order_relaxed))},
          {"observed", videoOutputLastProgressNs > 0},
          {"progressAgeMs", progressAgeMs(videoOutputLastProgressNs)},
          {"completedTicks", static_cast<double>(videoOutputWorkerCompletedTicks_.load(std::memory_order_relaxed))},
          {"workTotalNs", static_cast<double>(videoOutputWorkerWorkTotalNs_.load(std::memory_order_relaxed))},
          {"workMaximumNs", static_cast<double>(videoOutputWorkerWorkMaximumNs_.load(std::memory_order_relaxed))}}}});
  const auto zoomCapture = zoomSnapshot();
  if (zoomCapture.get("participants")) {
    state.emplace("participants", *zoomCapture.get("participants"));
  }
  if (zoomCapture.get("rosterEpoch")) {
    state.emplace("rosterEpoch", *zoomCapture.get("rosterEpoch"));
  }
  if (zoomCapture.get("rosterRevision")) {
    state.emplace("rosterRevision", *zoomCapture.get("rosterRevision"));
  }
  if (const auto activeSpeakerId = zoomCapture.getString("activeSpeakerId"); !activeSpeakerId.empty()) {
    state.emplace("activeSpeakerId", activeSpeakerId);
  }
  state.emplace("programSourceAdmission", modules::programSourceAdmissionJson(lastProgramFrame_));
  const auto preview = encodedPreview_;
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
  if (previewSceneActive_ || previewComposite_) {
    const auto previewPlanLayers = previewPlanLayerCount_;
    state.emplace("previewScene", rpc::Json::Object{
                                      {"sceneId", previewSceneId_},
                                      {"routeCount", previewRouteCount_},
                                      {"overlayCount", previewOverlayCount_},
                                      {"layerCount", previewPlanLayers},
                                      {"composite", previewComposite_},
                                  });
  }
  // T1 (Task 4): publish the solved wall so downstream consumers see what the
  // core actually drew, not a re-derivation with the solver they are meant to
  // be judging (this repo has shipped that "proxy that survives the bug"
  // failure twice — see the loudness-meter and recording-fps history above).
  // Reads lastRenderPlan_ directly — the SAME layers the compositor just
  // rendered — so a later pixel oracle knows exactly where to sample and the
  // canvas editor can position drag handles without recomputing anything.
  // `.present` alone (re-review finding A), matching the plan-cache gate and
  // buildRenderPlanForScene's wallActive: a configured wall with NOBODY live
  // is exactly the state worth publishing (`members: []`), not the state to
  // hide. A node that disappears in the case worth detecting is the
  // "multiviewer node that only appears once configured" mistake again.
  if (tilesLayer_.present) {
    rpc::Json::Array memberNodes;
    for (const auto& layer : lastRenderPlan_.layers) {
      if (layer.kind != "participant-video" || layer.layerId.rfind("tile:", 0) != 0) {
        continue;
      }
      memberNodes.push_back(rpc::Json{rpc::Json::Object{
          {"sourceId", rpc::Json{layer.sourceId}},
          {"rect", rpc::Json{rpc::Json::Object{
              {"x", rpc::Json{static_cast<double>(layer.rect.x)}},
              {"y", rpc::Json{static_cast<double>(layer.rect.y)}},
              {"width", rpc::Json{static_cast<double>(layer.rect.width)}},
              {"height", rpc::Json{static_cast<double>(layer.rect.height)}},
          }}},
      }});
    }
    state.emplace("tiles", rpc::Json::Object{
                               {"layerId", tilesLayer_.layerId},
                               {"members", rpc::Json{memberNodes}},
                           });
  }
  // The multiviewer config the core is ACTUALLY running — always published, even
  // at defaults. `configure-multiviewer` is a ONE-SHOT the shell sends at app
  // launch; when the core respawns under a live shell nothing re-sent it, so the
  // fresh core sat on its `grid` default while the shell still believed
  // `pgmPvwTop`. That silently dropped the PGM/PVW bus cells off the top of the
  // wall and degraded it to a bare source grid, and the shell could not detect it
  // because the applied mode was reported NOWHERE. Reporting it is what lets the
  // shell reconcile a core generation it did not configure (2026-08-08).
  state.emplace("multiviewer", rpc::Json::Object{
                                   {"layoutMode", multiviewLayoutMode_},
                                   {"tileCount", multiviewTileCount_},
                                   {"sourceCount", static_cast<int>(multiviewSources_.size())},
                                   {"showLabels", multiviewShowLabels_},
                                   {"showTally", multiviewShowTally_},
                                   {"showMeters", multiviewShowMeters_},
                                   {"showClock", multiviewShowClock_},
                               });
  const int64_t sourceHealthNowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  state.emplace("sources", core::sourceHealthState(sourceStatuses_,
      [this](const std::string& id) {
        const auto it = sourcePolicies_.find(id);
        return it == sourcePolicies_.end()
            ? core::SourcePresentationPolicy{}
            : core::SourcePresentationPolicy{it->second.dropoutPolicy, it->second.displayName};
      }));
  // Per-source media TRANSPORT state (#535 slice 3b). Published
  // unconditionally - the multiviewer-node rule again: an empty array is the
  // honest answer to "no media is routed", and a node that vanishes in the
  // case worth detecting is the mistake this codebase keeps paying for.
  {
    rpc::Json::Array mediaSourcesArr;
    if (mediaTransports_) {
      for (const auto& row : mediaTransports_->snapshot()) {
        mediaSourcesArr.push_back(rpc::Json::Object{
            {"sourceId", row.sourceId},
            {"mediaAssetId", row.assetId},
            {"state", std::string(core::mediaTransportStateName(row.state))},
            {"loop", row.loop},
            {"onProgram", row.onProgram},
            {"onPreview", row.onPreview},
            {"positionMs", static_cast<double>(row.positionMs)},
            {"durationMs", static_cast<double>(row.durationMs)},
            // #701 frame-delivery trace: a frozen clip is either a decoder that
            // stopped producing (lastDecodedAgeMs grows, videoQueued drains) or a
            // presentation that stopped advancing (frames decoded, presented age
            // grows). Read with programBuffer's delivery counters for the same window.
            {"decodedVideoFrames", static_cast<double>(row.decodedVideoFrames)},
            {"lastDecodedAgeMs", static_cast<double>(row.lastDecodedAgeMs)},
            {"presentedVideoFrames", static_cast<double>(row.presentedVideoFrames)},
            {"lastPresentedAgeMs", static_cast<double>(row.lastPresentedAgeMs)},
            {"videoQueued", static_cast<double>(row.videoQueued)},
            {"decoderRestarts", static_cast<double>(row.decoderRestarts)},
        });
      }
    }
    state.emplace("mediaSources", rpc::Json{mediaSourcesArr});
  }
  const auto recording = recordingState(session);
  if (!recording.isNull()) {
    state.emplace("recording", recording);
  }
  return state;
}

rpc::Json CoreStateCapture::health(const AudioOutputObservation& observation) const {
  const auto& session = observation.encoder;
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
      {"renderDeadlineMisses", static_cast<double>(renderDeadlineMisses_.load(std::memory_order_relaxed))},
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

rpc::Json CoreStateCapture::profile(const AudioOutputObservation& observation) const {
  const auto renderer = modules_.compositor->rendererName();
  const auto& encoderSession = observation.encoder;
  const auto capabilities = capabilityReport(modules_, zoomEngineConfigured(), encoderSession);
  return rpc::Json::Object{
      {"name", renderer == "software" ? "CoreVideo Pro Native Media Core Stub" : "CoreVideo Pro Native Media Core"},
      {"renderer", renderer},
      {"encoder", encoderSession.encoderName},
      {"maxProgramResolution", "3840x2160"},
      {"maxProgramFps", 60},
      {"maxParticipantFeeds", 10},
      {"maxIsoRecordings", 8},
      {"capabilities", capabilities.available},
      {"capabilityStates", capabilities.states},
  };
}

rpc::Json CoreStateCapture::audioMixSessionState(const AudioOutputObservation& observation) const {
  if (audioChannels_.empty()) {
    const auto nativeMix = observation.mixer;
    if (!nativeMix.participants.empty()) {
      rpc::Json::Array participants;
      for (const auto& participant : nativeMix.participants) {
        participants.emplace_back(rpc::Json::Object{
            {"participantId", participant.participantId},
            {"inputLevel", participant.inputLevel},
            {"outputLevel", participant.outputLevel},
            {"gainDb", participant.gainDb},
            // Output meters measured by the DSP off the mixed PCM (#765); a muted
            // strip contributes nothing to a bus, so it reports the silence floor.
            {"rmsDbfs", participant.muted ? kAudioDbfsFloor : round1Dbfs(modules::linearToDbfs(participant.rmsLevel))},
            {"peakDbfs", participant.muted ? kAudioDbfsFloor : round1Dbfs(modules::linearToDbfs(participant.peakLevel))},
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
          {"monitorControl", audioMonitorControlState()},
          {"monitorFramesPlayed", static_cast<double>(audioMonitorFramesPlayed_)},
          {"monitorUnderruns", static_cast<double>(audioMonitorUnderruns_)},
          {"monitorQueueDroppedFrames", static_cast<double>(audioMonitorQueueDroppedFrames_)},
          {"monitorIntentRevision", static_cast<double>(audioMonitorIntentRevision_)},
          {"monitorFeedbackRisk", audioMonitorFeedbackRisk_},
          {"participants", participants},
          {"masterMeter", masterMeterState(observation)},
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
        {"monitorControl", audioMonitorControlState()},
        {"monitorFramesPlayed", static_cast<double>(audioMonitorFramesPlayed_)},
        {"monitorUnderruns", static_cast<double>(audioMonitorUnderruns_)},
          {"monitorQueueDroppedFrames", static_cast<double>(audioMonitorQueueDroppedFrames_)},
          {"monitorIntentRevision", static_cast<double>(audioMonitorIntentRevision_)},
        {"monitorFeedbackRisk", audioMonitorFeedbackRisk_},
        {"participants", rpc::Json::Array{}},
        {"masterMeter", masterMeterState(observation)},
        {"summary", "Audio mix idle."},
        {"pluginHost", pluginHostState()},
        {"warnings", warnings},
    };
  }

  rpc::Json::Array participants;
  rpc::Json::Array warnings;
  std::unordered_set<std::string> warningSet;
  const auto nativeMix = observation.mixer;
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

  // T1.6: the "media" strip meters the pre-sum of every clip it governs (the
  // signal its DSP actually processes). The clips keep their own ids in the
  // mixer session.
  modules::AudioParticipantMixMetrics mediaPreSumMetric;
  if (mediaPreSumMetered_) {
    mediaPreSumMetric.participantId = std::string(kMediaAudioControlSourceId);
    mediaPreSumMetric.rmsLevel = std::clamp(mediaPreSumRmsLevel_, 0.0, 1.0);
    mediaPreSumMetric.peakLevel = std::clamp(mediaPreSumPeakLevel_, mediaPreSumMetric.rmsLevel, 1.0);
    mediaPreSumMetric.inputLevel =
        clampInt(static_cast<int>(std::lround(mediaPreSumMetric.rmsLevel * 100.0)), 0, 100);
    mediaPreSumMetric.limiterActive = mediaPreSumMetric.peakLevel >= 0.92;
  }

  for (const auto& channel : audioChannels_) {
    const auto nativeMetric = nativeMetricsByParticipant.find(channel.participantId);
    const modules::AudioParticipantMixMetrics* measured =
        nativeMetric == nativeMetricsByParticipant.end() ? nullptr : &nativeMetric->second;
    if (mediaPreSumMetered_ && channel.participantId == kMediaAudioControlSourceId) {
      measured = &mediaPreSumMetric;
    }
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
        // These are OUTPUT meters. A muted strip may still receive non-zero
        // input PCM, but none of that signal reaches a bus. Publishing the raw
        // pre-mute measurement made muted microphones appear live in the UI
        // and control API even while the routed audio was correctly silent.
        {"rmsDbfs", hasPcm && !channel.muted
                        ? round1Dbfs(modules::linearToDbfs(measured->rmsLevel))
                        : kAudioDbfsFloor},
        {"peakDbfs", hasPcm && !channel.muted
                         ? round1Dbfs(modules::linearToDbfs(measured->peakLevel))
                         : kAudioDbfsFloor},
        // #481: PRE-MUTE input meters, measured off the same PCM before mute/
        // fader is applied. A muted strip still receives signal - this is how
        // the A1 sees a guest talking while their channel is muted. Never gate
        // this on channel.muted; that would recreate the bug these fields fix.
        {"inputRmsDbfs", hasPcm
                        ? round1Dbfs(modules::linearToDbfs(measured->rmsLevel))
                        : kAudioDbfsFloor},
        {"inputPeakDbfs", hasPcm
                         ? round1Dbfs(modules::linearToDbfs(measured->peakLevel))
                         : kAudioDbfsFloor},
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
      // audio owner publishes these measured values. Short-term (3s) is the live
      // console readout; fall back to momentary until its window fills.
      {"loudnessLufs", observation.shortTermLufs > -119.0 ? observation.shortTermLufs : observation.momentaryLufs},
      {"pluginHost", pluginHostState()},
      {"limiterEnabled", audioLimiterEnabled_},
      {"limiterActive", limiterActive},
      {"mixedFrameCount", static_cast<double>(mixedAudioFrameCount_)},
      {"monitorEnabled", audioMonitorEnabled_},
      {"monitorStatus", audioMonitorStatus_},
      {"monitorDeviceId", audioMonitorDeviceId_},
      {"monitorDeviceName", audioMonitorDeviceName_},
      {"monitorVolume", audioMonitorVolume_},
      {"monitorControl", audioMonitorControlState()},
      {"monitorFramesPlayed", static_cast<double>(audioMonitorFramesPlayed_)},
      {"monitorUnderruns", static_cast<double>(audioMonitorUnderruns_)},
          {"monitorQueueDroppedFrames", static_cast<double>(audioMonitorQueueDroppedFrames_)},
          {"monitorIntentRevision", static_cast<double>(audioMonitorIntentRevision_)},
      {"monitorFeedbackRisk", audioMonitorFeedbackRisk_},
      {"participants", participants},
      {"masterMeter", masterMeterState(observation)},
      {"summary", summaryText},
      {"warnings", warnings},
  };
}

rpc::Json CoreStateCapture::audioRoutingMatrixState() const {
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
        {"control", audioRouteControl_.snapshot()},
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
        {"peakDbfs", pcm.peakDbfs},
        {"rmsDbfs", pcm.rmsDbfs},
    });
  }

  std::ostringstream summary;
  summary << audioRoutingSends_.size() << " send" << (audioRoutingSends_.size() == 1 ? "" : "s") << " from "
          << routedSources.size() << " source" << (routedSources.size() == 1 ? "" : "s") << " across " << routedBusCount
          << " bus(es).";

  return rpc::Json::Object{
      {"status", audioRoutingWarnings_.empty() ? "live" : "warning"},
      {"control", audioRouteControl_.snapshot()},
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

rpc::Json CoreStateCapture::captureAudioSourcesState(const AudioOutputObservation& observation) const {
  rpc::Json::Array sources;
  rpc::Json::Array warnings;
  std::set<std::string> warningSet;
  int pairedCount = 0;
  int streamingCount = 0;
  int64_t totalFramesReceived = 0;
  const int routedMasterFrames = static_cast<int>(programAudioTapPcm().size() / 2);
  const int routedStreamFrames = static_cast<int>(audioBusTapPcm("stream").size() / 2);
  const int routedMonitorFrames = static_cast<int>(audioBusTapPcm("mon").size() / 2);
  const int fallbackMonitorFrames = routedMonitorFrames == 0 ? observation.fallbackMonitorFrames : 0;
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
          {"monitorQueueDroppedFrames", static_cast<double>(audioMonitorQueueDroppedFrames_)},
          {"monitorIntentRevision", static_cast<double>(audioMonitorIntentRevision_)},
      {"monitorFeedbackRisk", audioMonitorFeedbackRisk_},
      {"sources", sources},
      {"warnings", warnings},
      {"summary", captureAudioSourcesSynced_ ? summary.str() : "Capture audio source pairing idle."},
  };
}

rpc::Json CoreStateCapture::encoderSessionState(const modules::OutputSession& session) const {
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
      {"encoderQueueDroppedVideoFrames", static_cast<double>(session.encoderQueueDroppedVideoFrames)},
      {"encoderQueueDroppedAudioPackets", static_cast<double>(session.encoderQueueDroppedAudioPackets)},
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

rpc::Json CoreStateCapture::recordingState(const modules::OutputSession& session) const {
  if (recordingSessionId_.empty() && recordingStatus_ == "stopped" && recordingQueueSettingWarning_.empty()) {
    return nullptr;
  }

  // The published status is DERIVED from the observed lifecycle whenever the
  // sink reports one, so `recording.status` and `recording.lifecycle` can never
  // disagree again. recordingStatus_ remains the internal desired-state gate
  // (it is what the render gather and the idempotent-start dedup read); it is
  // not, and never was, evidence of what the writer is doing.
  const std::string publishedStatus =
      session.lifecycle ? publishedRecordingStatus(session.lifecycle->state, session.lifecycle->health)
                        : recordingStatus_;
  const std::string publishedWriterStatus =
      session.lifecycle ? publishedRecordingWriterStatus(session.lifecycle->state)
                        : recordingWriterStatus_;
  const auto isoIds = recordingIsoParticipantIds_.empty() ? session.isoParticipantIds : recordingIsoParticipantIds_;
  const int64_t programFramesWritten = session.recordingVideoFrameCount;
  int64_t isoFramesWritten = 0;
  int64_t isoBytesWritten = 0;
  for (const auto& iso : session.isoStreams) {
    isoFramesWritten += iso.videoFrameCount;
    isoBytesWritten += iso.bytesWritten;
  }
  const double durationMs = static_cast<double>(session.recordingDurationMs);
  const int64_t audioPacketsObserved = session.recordingAudioPacketCount;
  const bool audioPresent = session.recordingAudioSampleCount > 0;
  const int64_t droppedVideoFrames = session.encoderQueueDroppedVideoFrames;
  const int64_t totalBytesWritten = session.recordingBytesWritten;
  const int64_t programBytesWritten = session.recordingProgramBytesWritten > 0
                                          ? session.recordingProgramBytesWritten
                                          : std::max<int64_t>(0, totalBytesWritten - isoBytesWritten);
  const int recordingWidth = session.recordingWidth > 0 ? session.recordingWidth : lastProgramFrame_.width;
  const int recordingHeight = session.recordingHeight > 0 ? session.recordingHeight : lastProgramFrame_.height;
  const int recordingFps = session.recordingFps > 0 ? session.recordingFps : 30;
  const std::string containerFormat = session.recordingContainerFormat.empty() ? recordingFormat_ : session.recordingContainerFormat;
  const std::string videoCodec = session.recordingVideoCodec.empty() ? session.codec : session.recordingVideoCodec;
  const std::string audioCodec = session.recordingAudioCodec.empty() ? "aac" : session.recordingAudioCodec;
  const bool metadataValid = session.recordingMetadataValid ||
                             (programFramesWritten > 0 && !containerFormat.empty() && !videoCodec.empty() && recordingWidth > 0 && recordingHeight > 0 &&
                              recordingFps > 0 && !audioCodec.empty());
  // Prefer the sink's REAL artifact path (the MF sink writes into a per-session
  // subfolder, spec §5); fall back to the synthesized flat path for the stub.
  const std::string programPath = !session.recordingArtifactPath.empty()
                                      ? session.recordingArtifactPath
                                      : recordingTargetFolder_ + "/" + recordingFilenamePrefix_ +
                                            "-program-0." + recordingFormat_;
  rpc::Json::Array streams{
      rpc::Json::Object{
          {"kind", "program"},
          {"path", programPath},
          {"status", publishedWriterStatus},
          {"expectedFrames", static_cast<double>(programFramesWritten + session.recordingProgramMissingFrames)},
          {"framesWritten", static_cast<double>(programFramesWritten)},
          {"durationMs", durationMs},
          {"frameRate", recordingFps},
          {"hasAudio", audioPresent},
          {"audioSamples", static_cast<double>(session.recordingAudioSampleCount)},
          {"missingFrames", static_cast<double>(session.recordingProgramMissingFrames)},
          {"droppedFrames", static_cast<double>(session.recordingProgramMissingFrames)},
          {"continuityObserved", session.recordingProgramContinuityObserved},
          {"writeQueue", recordingWriteQueueJson(session.programWriteQueue)},
          {"bytesWritten", static_cast<double>(programBytesWritten)},
          {"metadataValid", metadataValid},
      },
  };
  if (!session.isoStreams.empty()) {
    // ISO-1: real per-writer health from the Media Foundation sink — one node per
    // selected ISO source with its own path, frame count, open state and warning.
    // A video-only-broken ISO (writer failed to open / lost its track) is as loud
    // as a video-only program was (#286): its warning shows here AND folds into
    // recording.warning below.
    for (const auto& iso : session.isoStreams) {
      rpc::Json::Object node{
          {"kind", "iso"},
          {"sourceId", iso.sourceId},
          {"participantId", iso.sourceId},
          {"displayName", iso.displayName},
          {"path", iso.path},
          {"status", iso.warning.empty() ? publishedWriterStatus : std::string("warning")},
          {"readiness", iso.trackOpen ? "ready" : "missing"},
          {"framesWritten", static_cast<double>(iso.videoFrameCount)},
          {"muxVideoFrameCount", iso.muxVideoFrameCount ? rpc::Json(static_cast<double>(*iso.muxVideoFrameCount)) : rpc::Json(nullptr)},
          {"durationMs", durationMs},
          {"frameRate", recordingFps},
          // ISO-2: each ISO is self-contained A+V — hasAudio reflects real muxed
          // raw-stem samples (silence-fill keeps a gated stem's timeline aligned,
          // so a talking guest reports audio; a never-opened writer reports none).
          {"hasAudio", iso.audioSampleCount > 0},
          {"audioSamples", static_cast<double>(iso.audioSampleCount)},
          {"bytesWritten", static_cast<double>(iso.bytesWritten)},
          {"metadataValid", iso.trackOpen},
          {"encoderPath", iso.encoderPath},
          {"fallbackReason", iso.fallbackReason},
          {"droppedFrames", static_cast<double>(iso.droppedVideoFrames)},
          {"droppedAudioPackets", static_cast<double>(iso.droppedAudioPackets)},
          {"queuedVideoFrames", static_cast<double>(iso.queuedVideoFrames)},
          {"queuedAudioPackets", static_cast<double>(iso.queuedAudioPackets)},
          {"videoWorkUs", static_cast<double>(iso.videoWorkUs)},
          {"audioWorkUs", static_cast<double>(iso.audioWorkUs)},
          {"maximumWorkUs", static_cast<double>(iso.maximumWorkUs)},
          {"writeQueue", recordingWriteQueueJson(iso.writeQueue)},
      };
      if (!iso.warning.empty()) {
        node.emplace("warning", iso.warning);
      }
      streams.emplace_back(std::move(node));
    }
  } else {
    // A sink without per-writer telemetry must stay explicitly unknown. The old
    // fallback copied Program counts/bytes into every ISO and made a support
    // bundle claim successful stems that may never have existed.
    for (const auto& participantId : isoIds) {
      streams.emplace_back(rpc::Json::Object{
          {"kind", "iso"},
          {"participantId", participantId},
          {"path", recordingTargetFolder_ + "/" + recordingFilenamePrefix_ + "-iso-" + participantId + "-0." + recordingFormat_},
          {"status", "unreported"},
          {"readiness", "unknown"},
          {"framesWritten", 0},
          {"durationMs", durationMs},
          {"frameRate", recordingFps},
          {"hasAudio", false},
          {"bytesWritten", 0},
          {"metadataValid", false},
      });
    }
  }

  rpc::Json::Object recording{
      {"sessionId", recordingSessionId_.empty() ? "native-recording-session" : recordingSessionId_},
      {"active", session.lifecycle ? session.lifecycle->state == "producing" :
                 recordingStatus_ == "recording" || recordingStatus_ == "warning"},
      {"status", publishedStatus},
      {"writerStatus", publishedWriterStatus},
      {"startedAtMs", recordingStartedAtMs_},
      {"elapsedMs", durationMs},
      {"targetFolder", recordingTargetFolder_},
      {"filenamePrefix", recordingFilenamePrefix_},
      {"format", recordingFormat_},
      {"quality", recordingQuality_},
      {"writeQueueDepth", recordingWriteQueueDepth_},
      {"queueSettingWarning", recordingQueueSettingWarning_},
      {"encoder",
       rpc::Json::Object{
           {"codec", session.codec},
           {"hardwareAccelerated", session.hardwareAccelerated},
           {"targetBitrateMbps", session.targetBitrateMbps},
           {"audioBitrateKbps", session.recordingAudioBitrateKbps > 0 ? session.recordingAudioBitrateKbps : recordingAudioBitrateKbps_},
       }},
      {"estimatedDiskRateMBps", durationMs > 0.0
                                     ? static_cast<double>(totalBytesWritten) / (1024.0 * 1024.0) /
                                           (durationMs / 1000.0)
                                     : 0.0},
      {"programPath", programPath},
      {"streams", streams},
      {"proof",
       rpc::Json::Object{
           {"durationMs", durationMs},
           {"programFrameCount", static_cast<double>(programFramesWritten)},
           {"recordingVideoPrerollFrameCount", static_cast<double>(session.recordingVideoPrerollFrameCount)},
           {"recordingVideoTailFrameCount", static_cast<double>(session.recordingVideoTailFrameCount)},
           {"recordingMuxVideoFrameCount", static_cast<double>(session.recordingMuxVideoFrameCount)},
           {"programMissingFrames", static_cast<double>(session.recordingProgramMissingFrames)},
           {"programContinuityObserved", session.recordingProgramContinuityObserved},
           {"recordingAudioPrerollSampleCount", static_cast<double>(session.recordingAudioPrerollSampleCount)},
           {"recordingAudioTailSampleCount", static_cast<double>(session.recordingAudioTailSampleCount)},
           {"recordingRequestedAt100ns", static_cast<double>(session.recordingRequestedAt100ns)},
           {"recordingWriterReadyAt100ns", static_cast<double>(session.recordingWriterReadyAt100ns)},
           {"recordingMuxEpoch100ns", static_cast<double>(session.recordingMuxEpoch100ns)},
           {"recordingStartupDroppedAudioPackets", static_cast<double>(session.recordingStartupDroppedAudioPackets)},
           {"recordingStartupDroppedVideoFrames", static_cast<double>(session.recordingStartupDroppedVideoFrames)},
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
           {"encoderQueueDroppedVideoFrames", static_cast<double>(session.encoderQueueDroppedVideoFrames)},
           {"encoderQueueDroppedAudioPackets", static_cast<double>(session.encoderQueueDroppedAudioPackets)},
       }},
      {"totalFramesWritten", static_cast<double>(programFramesWritten + isoFramesWritten)},
      {"totalDroppedFrames", static_cast<double>(droppedVideoFrames)},
      {"totalBytesWritten", static_cast<double>(totalBytesWritten)},
  };
  if (session.lifecycle) {
    recording.emplace("lifecycle", contracts::toJson(*session.lifecycle));
  }
  if (!session.recordingArtifactPath.empty()) {
    recording.emplace("artifactPath", session.recordingArtifactPath);
  }
  if (!session.recordingSessionDir.empty()) {
    recording.emplace("sessionDir", session.recordingSessionDir);
  }
  if (!session.recordingManifestPath.empty()) {
    recording.emplace("manifestPath", session.recordingManifestPath);
  }
  if (!recordingError_.empty()) {
    recording.emplace("error", recordingError_);
  }
  if (!recordingQueueSettingWarning_.empty()) {
    recording.emplace("warning", recordingQueueSettingWarning_ +
        (recordingWarning_.empty() ? "" : " " + recordingWarning_));
  } else if (!recordingWarning_.empty()) {
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

rpc::Json CoreStateCapture::captionTrackState() const {
  rpc::Json::Array warnings;
  for (const auto& warning : captionWarnings_) {
    warnings.emplace_back(warning);
  }
  if (!captionEnabled_) {
    return rpc::Json::Object{
        {"enabled", false},
        {"status", "idle"},
        {"warnings", warnings},
    };
  }
  if (captionText_.empty()) {
    return rpc::Json::Object{
        {"enabled", true},
        {"status", "idle"},
        {"warnings", warnings},
    };
  }

  rpc::Json::Object cue{
      {"text", captionText_},
      {"atMs", captionAtMs_},
  };
  if (!captionSpeaker_.empty()) {
    cue.emplace("speaker", captionSpeaker_);
  }

  return rpc::Json::Object{
      {"enabled", true},
      {"status", warnings.empty() ? "live" : "warning"},
      {"currentCue", cue},
      {"warnings", warnings},
  };
}

rpc::Json CoreStateCapture::brandKitState() const {
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

rpc::Json CoreStateCapture::overlayState() const {
  rpc::Json::Array overlays;
  int lowerThirdCount = 0;
  int buildingCount = 0;
  int onAirCount = 0;
  int hiddenCount = 0;

  std::vector<const MediaCore::OverlayAssetState*> orderedOverlays;
  orderedOverlays.reserve(overlayAssets_.size());
  for (const auto& [overlayId, asset] : overlayAssets_) {
    orderedOverlays.push_back(&asset);
  }
  std::sort(
      orderedOverlays.begin(),
      orderedOverlays.end(),
      [](const MediaCore::OverlayAssetState* left, const MediaCore::OverlayAssetState* right) {
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
        {"visible", asset->keyPhase != "hidden" && asset->keyPhase != "building-out" &&
            (!isLowerThird || asset->sourceId.empty() || renderedProgramSources_.containsOverlay(
                "overlay:" + asset->overlayId + ":lower-third", asset->sourceId,
                asset->title, asset->org, asset->text, asset->imageUri, asset->keyPosition,
                asset->keyer, asset->keyPhase))},
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

rpc::Json CoreStateCapture::autoProductionState() const {
  const auto recommendation = recommendAutoProduction();
  rpc::Json::Array slotBindings;
  for (const auto& binding : recommendation.slotBindings) {
    slotBindings.emplace_back(rpc::Json::Object{
        {"slotIndex", binding.slotIndex}, {"personId", binding.personId}, {"sourceId", binding.sourceId}});
  }
  return rpc::Json::Object{
      {"ruleId", recommendation.ruleId},
      {"recommendedSceneId", recommendation.recommendedSceneId},
      {"confidence", recommendation.confidence},
      {"rationale", recommendation.rationale},
      {"slotBindings", std::move(slotBindings)},
  };
}

rpc::Json CoreStateCapture::mediaPlaybackState() const {
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

  // #535 slice 3b: the state of the selection is its transport state, never a
  // flag the shell asserted. A selected asset on neither bus has no transport
  // and therefore no play state - "unavailable" says exactly that, instead of
  // the "paused" a stale flag used to report.
  std::string status = "unavailable";
  if (mediaTransports_) {
    const std::string sourceId = "media:" + mediaPlaybackAssetId_;
    for (const auto& row : mediaTransports_->snapshot()) {
      if (row.sourceId != sourceId) continue;
      status = core::mediaTransportStateName(row.state);
      break;
    }
  }
  const bool playing = status == "live";
  const std::string& name = mediaPlaybackAssetName_;
  return rpc::Json::Object{
      {"status", status},
      {"mediaAssetId", mediaPlaybackAssetId_},
      {"mediaAssetName", name},
      {"mediaAssetKind", mediaPlaybackAssetKind_},
      {"mediaAssetPath", mediaPlaybackAssetPath_},
      // Retired (#535 slice 3b); kept as an empty string only so an older
      // shell reading the field finds a value rather than a missing key.
      {"mediaPlaybackKey", std::string{}},
      {"playing", playing},
      {"summary", name + " " + status + "."},
      {"warnings", warnings},
  };
}

rpc::Json CoreStateCapture::takeRecordsState() const {
  rpc::Json::Array records;
  for (const auto& record : takeRecords_) {
    records.emplace_back(rpc::Json::Object{
        {"fromSceneId", record.fromSceneId},
        {"toSceneId", record.toSceneId},
        {"fromRenderPlanId", record.fromRenderPlanId},
        {"toRenderPlanId", record.toRenderPlanId},
        {"operationId", record.operationId},
        {"revision", static_cast<double>(record.revision)},
        {"mode", record.mode},
        {"fromLayerIds", stringArray(record.fromLayerIds)},
        {"toLayerIds", stringArray(record.toLayerIds)},
        {"fromWallKey", record.fromWallKey},
        {"toWallKey", record.toWallKey},
        {"wall", std::string(record.verdict.wall)},
        {"verdict", std::string(record.verdict.verdict)},
        {"liveBackgroundExpected", record.observation.liveBackgroundExpected},
        {"liveBackgroundEmitted", record.observation.liveBackgroundEmitted},
        {"backgroundDropped", record.verdict.backgroundDropped},
        {"subscriptionChurnDelta", static_cast<double>(record.observation.subscriptionChurnDelta)},
        {"subscriptionsChurned", record.verdict.subscriptionsChurned},
        {"fromSourceIds", stringArray(record.fromSourceIds)},
        {"sources", [&] {
           rpc::Json::Array sources;
           for (const auto& s : record.observation.sharedSources) {
             const bool restarted = s.generationAfter != s.generationBefore;
             sources.emplace_back(rpc::Json::Object{
                 {"sourceId", s.sourceId},
                 {"generationBefore", static_cast<double>(s.generationBefore)},
                 {"generationAfter", static_cast<double>(s.generationAfter)},
                 {"frameIdBefore", static_cast<double>(s.frameIdBefore)},
                 {"frameIdAfter", static_cast<double>(s.frameIdAfter)},
                 {"restarted", restarted}});
           }
           return sources;
         }()},
        {"restartedSources", stringArray(record.verdict.restartedSources)},
        {"sharedSourceRestarted", record.verdict.sharedSourceRestarted},
        {"missingSources", stringArray(record.verdict.missingSources)},
        {"sourceMissing", record.verdict.sourceMissing},
        {"armedAtMs", record.armedAtMs},
        {"armedAtFrame", static_cast<double>(record.armedAtFrame)},
    });
  }
  return rpc::Json::Object{
      {"count", static_cast<double>(takeRecords_.size())},
      {"pending", pendingTakeRecord_.has_value()},
      {"records", records},
  };
}

rpc::Json CoreStateCapture::zoomSubscriptionChurnState() const {
  if (zoomEngineRuntime_ && zoomEngineRuntime_->configured()) {
    return zoomEngineRuntime_->subscriptionChurnState();
  }
  // No engine: the node still exists, and says so, rather than vanishing.
  return rpc::Json::Object{
      {"engine", false},
      {"sourceCount", 0.0},
      {"subscribedCount", 0.0},
      {"totalChurn", 0.0},
      {"lastResolutionChanges", 0.0},
      {"lastCapEvictions", 0.0},
      {"lastUnrouted", 0.0},
      {"lastVideoOff", 0.0},
      {"fullResolutionCap", 0.0},
      {"fullResolutionDemoted", 0.0},
      {"lastDepartures", 0.0},
      {"sources", rpc::Json::Array{}},
  };
}

std::string CoreStateCapture::resolveMeetingStateForSession() const {
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

rpc::Json CoreStateCapture::zoomReadinessState() const {
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

  if (zoomJoinRequiresMissingEngine()) {
    return rpc::Json::Object{
        {"status", "blocked"},
        {"mode", "no-engine"},
        {"sdkAvailable", false},
        {"sdkVersion", ""},
        {"meetingState", "idle"},
        {"checks",
         rpc::Json::Array{
             rpc::Json::Object{{"id", "zoom-engine-runtime"}, {"status", "blocked"},
                               {"label", "COREVIDEO_ZOOM_ENGINE_PATH is not set: no Zoom engine, so Join is refused"}},
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

rpc::Json CoreStateCapture::zoomEvidenceState() const {
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
      {"activeSpeakerId", zoomJoined_ ? kStubActiveSpeakerId : ""},
      {"snapshotTick", zoomSnapshotTick_},
  };
}

rpc::Json CoreStateCapture::browserSourcesState() const {
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

rpc::Json CoreStateCapture::captureDevicesState() const {
  auto devices = modules_.captureDevice->enumerate();
  // Browser sources present as capture devices (kind "browser") so the shell's
  // Sources pickers list them exactly like screens.
  auto browserDevices = browserSources_->enumerate();
  devices.insert(devices.end(), browserDevices.begin(), browserDevices.end());
  return captureDeviceArray(devices);
}

rpc::Json CoreStateCapture::virtualCameraState() const {
  const auto status = virtualCamera_->status();
  return rpc::Json::Object{
      {"enabled", status.enabled},
      {"status", status.state},
      {"deviceName", status.deviceName},
      {"resolution", rpc::Json::Object{{"width", status.width}, {"height", status.height}}},
      {"fps", status.fps},
      {"framesPublished", static_cast<double>(status.framesPublished)},
      {"framesAccepted", static_cast<double>(status.framesAccepted)},
      {"pendingFramesReplaced", static_cast<double>(status.pendingFramesReplaced)},
      {"publicationExceptions", static_cast<double>(status.publicationExceptions)},
      {"framerEnabled", status.framerEnabled},
      {"framerState", status.framerState},
      {"framerWarning", status.framerWarning},
      {"framerFrames", static_cast<double>(status.framerFrames)},
      {"framerFailures", static_cast<double>(status.framerFailures)},
      {"warning", status.warning},
  };
}

std::vector<std::string> CoreStateCapture::canonicalIsoSourceIds() const {
  std::vector<std::string> out;
  out.reserve(recordingIsoParticipantIds_.size());
  for (const auto& rawId : recordingIsoParticipantIds_) {
    out.push_back(normalizeIsoSourceId(rawId));
  }
  return out;
}

rpc::Json CoreStateCapture::masterMeterState(const AudioOutputObservation& observation) const {
  return rpc::Json::Object{
      {"momentaryLufs", observation.momentaryLufs},
      {"shortTermLufs", observation.shortTermLufs},
      {"integratedLufs", observation.integratedLufs},
      {"truePeakDbfs", observation.truePeakDbfs},
      {"windowMs", observation.windowMs},
  };
}

rpc::Json CoreStateCapture::audioMonitorControlState() const {
  rpc::Json::Array recentResults;
  for (const auto& operationId : audioMonitorOperationOrder_) {
    recentResults.emplace_back(audioMonitorOperationResults_.at(operationId));
  }
  return rpc::Json::Object{
      {"authorityEpoch", audioMonitorControlEpoch_},
      {"revision", static_cast<double>(audioMonitorControlRevision_)},
      {"lastResult", audioMonitorControlResult_},
      {"recentResults", recentResults},
  };
}

DirectorRecommendation CoreStateCapture::recommendAutoProduction() const {
  const auto nowMs = static_cast<std::uint64_t>(monotonicMs());
  return recommendScene(deriveDirectorSignals(), speakerFloor_.snapshot(nowMs), nowMs);
}

DirectorSignals CoreStateCapture::deriveDirectorSignals() const {
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
    const auto* rawTalking = participant.get("rawTalking");
    const bool talking = rawTalking ? rawTalking->asBool() :
        (participant.get("talking") && participant.get("talking")->asBool());
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

rpc::Json CoreStateCapture::zoomSnapshot() const {
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

  const std::string speaker = !shellActiveSpeakerId_.empty()
      ? shellActiveSpeakerId_
      : std::string(kStubActiveSpeakerId);
  rpc::Json::Array participants = !shellZoomRoster_.isNull() && shellZoomRoster_.isArray() && !shellZoomRoster_.asArray().empty()
      ? shellZoomRoster_.asArray()
      : rpc::Json::Array{
           rpc::Json::Object{
               {"userId", "operator-1"},
               {"displayName", zoomDisplayName_},
               {"role", "Host"},
               {"videoOn", true},
               {"muted", false},
               {"talking", true},
           },
           rpc::Json::Object{
               {"userId", "guest-1"},
               {"displayName", "Guest 1"},
               {"role", "Guest"},
               {"videoOn", true},
               {"muted", false},
               {"talking", false},
           },
       };
  return rpc::Json::Object{
      {"meetingState", "in_meeting"},
      {"activeSpeakerId", speaker},
      {"screenShareParticipantId", shellScreenShareParticipantId_},
      {"caption", ""},
      {"readiness", zoomReadinessState()},
      {"evidence", zoomEvidenceState()},
      {"tick", zoomSnapshotTick_},
      {"participants", std::move(participants)},
  };
}

bool CoreStateCapture::zoomJoinRequiresMissingEngine() const {
  return !modules_.permitStubZoomSession && !zoomEngineConfigured();
}

contracts::OutputLifecycle CoreStateCapture::evaluateSenderLifecycle(
    const modules::OutputSender& sender, bool desiredActive, int64_t nowMs) const {
  const std::string key = sender.senderId.empty() ? sender.destination : sender.senderId;
  auto& evidence = (*senderLifecycles_)[key];
  const int64_t sent = modules::outputSenderProgressUnits(sender);
  if (sent > evidence.lastFramesSent) {
    evidence.lastFramesSent = sent;
    evidence.lastProgressMs = nowMs;
    evidence.everProduced = true;
  } else if (sent < evidence.lastFramesSent) {
    // A restarted sender resets its counters; that is a new run, not progress.
    evidence.lastFramesSent = sent;
    evidence.everProduced = sent > 0;
    evidence.lastProgressMs = sent > 0 ? nowMs : 0;
  }
  core::SenderObservation observation;
  observation.status = sender.status;
  observation.destinationHealth = sender.destinationHealth;
  observation.desiredActive = desiredActive;
  observation.framesSent = sent;
  observation.everProduced = evidence.everProduced;
  observation.lastProgressMs = evidence.lastProgressMs;
  observation.nowMs = nowMs;
  observation.hasError = !sender.lastError.empty();
  const auto decision = core::SenderLifecyclePolicy::evaluate(observation);
  contracts::OutputLifecycle lifecycle;
  lifecycle.sessionId = key.empty() ? std::string("output-sender") : key;
  lifecycle.desiredActive = desiredActive;
  lifecycle.state = decision.state;
  lifecycle.health = decision.health;
  lifecycle.finalized = core::SenderLifecyclePolicy::finalizedFor(decision, evidence.everProduced);
  // The error field is diagnostic context for a state that is already bad. It is
  // NOT attached to a healthy destination: `lastError` is sticky history, and a
  // producing stream carrying an old string would read as broken in a bundle.
  if (decision.state == "failed" && !sender.lastError.empty()) {
    lifecycle.error = sender.lastError;
  } else if (decision.state == "interrupted") {
    lifecycle.error = sender.lastError.empty()
                          ? std::string("Destination stopped producing without being asked to stop")
                          : "Destination stopped producing without being asked to stop: " + sender.lastError;
  } else if (decision.state == "failed" && !sender.warning.empty()) {
    lifecycle.error = sender.warning;
  }
  if (core::OutputLifecyclePolicy::isTerminal(decision.state)) evidence.terminal = lifecycle;
  else if (decision.state == "idle" && evidence.terminal) return *evidence.terminal;
  return lifecycle;
}

rpc::Json CoreStateCapture::outputSenderSessionState(const AudioOutputObservation& observation) const {
  const auto& senderSession = observation.sender;
  const int64_t senderNowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  rpc::Json::Array senders;
  for (const auto& sender : senderSession.senders) {
    const bool desiredActive =
        std::find(outputDestinations_.begin(), outputDestinations_.end(), sender.destination) !=
        outputDestinations_.end();
    const auto lifecycle = evaluateSenderLifecycle(sender, desiredActive, senderNowMs);
    // #468 / T2.10: status and destinationHealth are PROJECTIONS of the
    // lifecycle and the supervisor, never the adapter's last launch state - a
    // dead destination never disturbs that state, so it read "live"/"ok" for a
    // stream nothing was receiving.
    const std::string lifecycleState = lifecycle.state;
    rpc::Json::Object senderJson{
        {"senderId", sender.senderId},
        {"destination", sender.destination},
        {"status", core::publishedSenderStatus(sender.status, lifecycleState, sender.supervisor)},
        {"framesSent", static_cast<double>(sender.framesSent)},
        {"retryCount", sender.retryCount},
        {"latencyMs", sender.latencyMs},
        {"bitrateMbps", sender.bitrateMbps},
        {"destinationHealth", core::publishedSenderDestinationHealth(sender.destinationHealth, lifecycleState, sender.supervisor)},
        {"lastResultCode", sender.lastResultCode},
        {"bytesSent", static_cast<double>(sender.bytesSent)},
        {"audioFramesSent", static_cast<double>(sender.audioFramesSent)},
        {"audioBytesSent", static_cast<double>(sender.audioBytesSent)},
        {"droppedAudioPackets", static_cast<double>(sender.droppedAudioPackets)},
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
    if (sender.muxInputVideo) {
      const auto& mux = *sender.muxInputVideo;
      senderJson.emplace("muxInputVideo", rpc::Json::Object{
          {"payloadBytes", static_cast<double>(mux.payloadBytes)},
          {"packets", static_cast<double>(mux.packets)},
          {"mbps", mux.mbps},
          {"fps", mux.fps},
          {"lastWriteAgeMs", static_cast<double>(mux.lastWriteAgeMs)},
          {"boundary", "compressed video written to local FFmpeg input; remote delivery unverified"},
      });
    }
    if (sender.streamClock) {
      const auto& clock = *sender.streamClock;
      senderJson.emplace("streamClock", rpc::Json::Object{
          {"muxInput", clock.muxInput},
          {"firstVideoPts100ns", static_cast<double>(clock.firstVideoPts100ns)},
          {"firstAudioPts100ns", static_cast<double>(clock.firstAudioPts100ns)},
          {"audioUnits", static_cast<double>(clock.audioUnits)},
          {"audioRefused", static_cast<double>(clock.audioRefused)},
          {"audioUnanchored", static_cast<double>(clock.audioUnanchored)},
          {"videoSlotShift", static_cast<double>(clock.videoSlotShift)},
          {"videoSlotShifts", static_cast<double>(clock.videoSlotShifts)},
      });
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
    // PR19: the output supervisor's per-destination state, verbatim. /snapshot
    // serves sessionState as-is, so this is the whole path from "a destination
    // failed on a machine we cannot see" to "the support bundle says which one,
    // how many times, and why we stopped restarting it".
    if (sender.supervisor) {
      const auto& supervisor = *sender.supervisor;
      senderJson.emplace("supervisor", rpc::Json::Object{
          {"generation", static_cast<double>(supervisor.generation)},
          {"healthy", supervisor.healthy},
          {"gaveUp", supervisor.gaveUp},
          {"consecutiveFailures", supervisor.consecutiveFailures},
          {"restarts", supervisor.restarts},
          {"nextAttemptInMs", static_cast<double>(supervisor.nextAttemptInMs)},
          {"lastProgressAgeMs", static_cast<double>(supervisor.lastProgressAgeMs)},
          {"acceptedUnits", static_cast<double>(supervisor.acceptedUnits)},
          {"staleEventsRejected", static_cast<double>(supervisor.staleEventsRejected)},
          {"malformedObservations", static_cast<double>(supervisor.malformedObservations)},
          {"failureClass", supervisor.failureClass},
          {"reason", supervisor.reason},
          {"inProcessRisk", supervisor.inProcessRisk},
          {"interruptible", supervisor.interruptible},
      });
    }
    if (sender.asyncWorker) {
      const auto& worker = *sender.asyncWorker;
      senderJson.emplace("asyncWorker", rpc::Json::Object{
          {"operation", worker.operation},
          {"stage", worker.stage},
          {"operationAgeMs", static_cast<double>(worker.operationAgeMs)},
          {"queuedItems", static_cast<double>(worker.queuedItems)},
          {"droppedSyncs", static_cast<double>(worker.droppedSyncs)},
      });
    }
    // #597 Task 6: this destination's own view of Lever A/B - published
    // whenever the sender populated it (GPU-direct senders only; see
    // OutputBackpressureState in Interfaces.h). The one global fact this node
    // deliberately omits (how many frames the compositor actually shed) is
    // published once, unconditionally, at realtimeEvidence.encoderExport below.
    if (sender.backpressure) {
      const auto& bp = *sender.backpressure;
      senderJson.emplace("backpressure", rpc::Json::Object{
          // `divisor` is the destination's legacy pressure recommendation.
          // `appliedDivisor` is the actual compositor export rate, now held at
          // full cadence while each destination owns its own queue recovery.
          {"divisor", bp.divisor},
          {"appliedDivisor", modules_.compositor ? modules_.compositor->encoderExportDivisor() : 1},
          {"level", bp.level},
          {"bufferedMs", static_cast<double>(bp.bufferedMs)},
          {"queuedChunks", static_cast<double>(bp.queuedChunks)},
          {"inFlightWriteMs", static_cast<double>(bp.inFlightWriteMs)},
          {"maxWriteMs", static_cast<double>(bp.maxWriteMs)},
          {"slowWriteCount", static_cast<double>(bp.slowWriteCount)},
          {"enteredCount", static_cast<double>(bp.enteredCount)},
          {"discardedChunks", static_cast<double>(bp.discardedChunks)},
          {"discardEvents", static_cast<double>(bp.discardEvents)},
          // #597 fix round 3, item 3: the two counters above count different
          // populations, and this says so WHERE A READER MEETS THEM instead of
          // only in a C++ comment they will never see.
          {"discardCounterNote", std::string(bp.discardCounterNote)},
          {"lastReason", bp.lastReason},
          {"lastTransitionBufferedMs", static_cast<double>(bp.lastTransitionBufferedMs)},
          {"runId", static_cast<double>(bp.runId)},
          {"observedAtMs", bp.observedAtMs},
      });
    }
    senderJson.emplace("lifecycle", contracts::toJson(lifecycle));
    senders.emplace_back(std::move(senderJson));
  }

  return rpc::Json::Object{
      {"status", senderSession.status},
      {"activeSenderCount", senderSession.activeSenderCount},
      {"senders", senders},
      {"warnings", stringArray(senderSession.warnings)},
  };
}

}
