#pragma once

#include "core/Director.h"
#include "modules/AudioDsp.h"
#include "modules/Interfaces.h"
#include "modules/ZoomEngineRuntime.h"
#include "rpc/Json.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace corevideo::core {

class MediaCore {
 public:
  explicit MediaCore(modules::ModuleSet modules = modules::createDefaultModules());

  [[nodiscard]] rpc::Json profile() const;
  [[nodiscard]] rpc::Json sessionState() const;
  [[nodiscard]] rpc::Json health() const;
  [[nodiscard]] rpc::Json captureDevices() const;
  [[nodiscard]] rpc::Json selectCaptureInput(const std::string& deviceId, const std::string& inputId);
  [[nodiscard]] rpc::Json setCaptureAudioSyncOffset(const std::string& deviceId, int offsetMs);
  [[nodiscard]] rpc::Json connectCaptureDevice(const std::string& deviceId);
  [[nodiscard]] rpc::Json joinZoom(const rpc::Json& payload);
  [[nodiscard]] rpc::Json leaveZoom();
  [[nodiscard]] rpc::Json zoomSnapshot() const;
  [[nodiscard]] rpc::Json syncZoomMediaSpine(const rpc::Json& payload, double elapsedMs);
  [[nodiscard]] std::vector<rpc::Json> drainZoomVideoFrameEvents();
  [[nodiscard]] std::vector<rpc::Json> drainProgramFramePreviewEvents();
  [[nodiscard]] std::vector<rpc::Json> drainProgramSharedTextureEvents();
  [[nodiscard]] rpc::Json applyCommand(const rpc::Json& command);
  [[nodiscard]] rpc::Json applyCommands(const rpc::Json::Array& commands, double elapsedMs = 0.0);

  // Light, video-only render tick for the operator program display. Renders the
  // GPU compositor and emits the shared-texture handle, but skips the audio mix,
  // monitor/output senders, encoder submit and base64 preview readback — i.e. no
  // blocking I/O — so it can run at ~60fps on the command-processing thread
  // without starving RPC command handling. The full renderSyntheticTick (encoder/
  // recording/streaming/audio) still runs on the host-sync cadence.
  void renderDisplayTick();

  // Deterministic on-device AI director: derives the richer signal bundle from
  // the core's current state (participant feeds, audio metrics, screen share,
  // feed health) and returns a scene recommendation (ruleId, recommendedSceneId,
  // confidence, rationale). Pure read of state; mutates nothing.
  [[nodiscard]] DirectorSignals deriveDirectorSignals() const;
  [[nodiscard]] DirectorRecommendation recommendAutoProduction() const;

  // Real-time PCM taps produced by the routing-matrix bus mix each tick, for the
  // outputs to consume (program encode, ISO record). The program tap is the
  // stereo "master" bus; ISO taps are the "iso-*" buses. Interleaved stereo
  // float in [-1, 1]; empty until a synced routing matrix routes PCM-bearing
  // sources to a bus.
  [[nodiscard]] const std::vector<float>& programAudioTapPcm() const;
  [[nodiscard]] std::vector<std::string> routedAudioBusIds() const;
  [[nodiscard]] const std::vector<float>& audioBusTapPcm(const std::string& busId) const;

 private:
  void loadSceneGraph(const rpc::Json& command);
  void setParticipantTransform(const rpc::Json& command);
  void setOverlayAsset(const rpc::Json& command);
  void setColorGrade(const rpc::Json& command);
  void setOutputProfile(const rpc::Json& command);
  void startProgramOutput(const rpc::Json& command);
  void prepareEncoderSession(const rpc::Json& command);
  void startEncoderSession(const rpc::Json& command);
  void stopEncoderSession(const rpc::Json& command);
  void failOutputSender(const rpc::Json& command);
  void recoverOutputSender(const rpc::Json& command);
  void setRecordingTargets(const rpc::Json& command);
  void startRecordingSession(const rpc::Json& command);
  void stopRecordingSession(const rpc::Json& command);
  void failRecordingSession(const rpc::Json& command);
  void recoverRecordingSession(const rpc::Json& command);
  void configureEncoderRecordingRequest();
  void syncParticipantAudioMix(const rpc::Json& command);
  void syncAudioMonitor(const rpc::Json& command);
  void syncAudioRoutingMatrix(const rpc::Json& command);
  void syncCaptureAudioSources(const rpc::Json& command);
  void pushCaptionCue(const rpc::Json& command);
  void setCaptionEnabled(const rpc::Json& command);
  void setBrandKit(const rpc::Json& command);
  void setMediaPlayback(const rpc::Json& command);
  void configureSrtIngestSources(const rpc::Json& command);
  void simulateBreakoutRoomChange(const rpc::Json& command);
  void renderSyntheticTick(bool videoOnly = false);
  void enqueueProgramFramePreviewEvent();
  void enqueueProgramSharedTextureEvent();
  [[nodiscard]] rpc::Json encoderSessionState(const modules::OutputSession& session) const;
  [[nodiscard]] rpc::Json audioMixSessionState() const;
  void updateProgramLoudnessMeter(const std::vector<float>& interleaved, int channels, int sampleRate);
  [[nodiscard]] rpc::Json masterMeterState() const;
  [[nodiscard]] rpc::Json audioRoutingMatrixState() const;
  [[nodiscard]] rpc::Json captureAudioSourcesState() const;
  [[nodiscard]] rpc::Json captionTrackState() const;
  [[nodiscard]] rpc::Json brandKitState() const;
  [[nodiscard]] rpc::Json overlayState() const;
  [[nodiscard]] rpc::Json mediaPlaybackState() const;
  [[nodiscard]] rpc::Json autoProductionState() const;
  [[nodiscard]] rpc::Json outputSenderSessionState() const;
  [[nodiscard]] rpc::Json captureDevicesState() const;
  [[nodiscard]] rpc::Json recordingState(const modules::OutputSession& session) const;
  [[nodiscard]] rpc::Json zoomReadinessState() const;
  [[nodiscard]] rpc::Json zoomEvidenceState() const;
  [[nodiscard]] std::string resolveMeetingStateForSession() const;

  struct SceneRouteState {
    std::string routeId;
    std::string mode;
    std::string participantId;
    std::string captureDeviceId;
    std::string audioRole;
    std::string mediaAssetId;
    std::string mediaAssetName;
    std::string mediaAssetKind;
    std::string mediaAssetPath;
    std::string mediaPlaybackKey;
    bool mediaAssetPlaying = false;
    float rectX = 0.f;
    float rectY = 0.f;
    float rectWidth = 0.f;
    float rectHeight = 0.f;
    int zIndex = 0;
    bool hasRect = false;
    std::string fitMode = "fill";
    std::string borderStyle = "accent";
    std::string borderColor = "#44C1A1";
    float borderThickness = 2.f;
    float sourceScale = 1.f;
    float sourceOffsetX = 0.f;
    float sourceOffsetY = 0.f;
    bool hasColorGrade = false;
    modules::CompositorColorGrade colorGrade;
  };

  struct SceneBackgroundState {
    bool enabled = false;
    std::string mediaAssetId;
    std::string mediaAssetName;
    std::string mediaAssetKind;
    std::string mediaAssetPath;
    bool playing = true;
  };

  [[nodiscard]] modules::CompositorRenderPlan buildCompositorRenderPlan(const std::vector<modules::VideoFrame>& videoFrames) const;
  // Advances the overlay animation clock and each overlay's keyPhase progress by
  // one render tick, retiring overlays whose building-out animation has settled.
  void advanceOverlayAnimation(double frameIntervalMs);

  modules::ModuleSet modules_;
  std::string sceneId_ = "unloaded";
  std::vector<SceneRouteState> sceneRoutes_;
  SceneBackgroundState sceneBackground_;
  int routeCount_ = 0;
  int transformCount_ = 0;
  int overlayCount_ = 0;
  std::unordered_set<std::string> overlayIds_;
  // Rich overlay-asset payloads captured from set-overlay-asset, carried into
  // the compositor render plan for real text/image/keyer rendering. Keyed by
  // overlayId; insertionOrder preserves a stable, deterministic z-order.
  struct OverlayAssetState {
    std::string overlayId;
    std::string text;
    std::string imageUri;
    std::string sourceId;
    std::string sourceName;
    std::string position = "lower-third";
    std::string title;
    std::string org;
    std::string keyPosition = "lower-left";
    std::string keyPhase = "on-air";
    std::string keyer = "downstream";
    int buildInMs = 420;
    int buildOutMs = 420;
    int insertionOrder = 0;
    // Animation clock: the keyPhase the layer is animating toward, and the
    // monotonically-advanced progress [0,1] within the current phase.
    float keyProgress = 0.f;
  };
  std::map<std::string, OverlayAssetState> overlayAssets_;
  int overlayInsertionCounter_ = 0;
  // Monotonic compositor animation clock (ms), advanced each render tick, that
  // drives overlay keyPhase progress deterministically.
  double overlayAnimationClockMs_ = 0.0;
  std::string outputProfileId_ = "canvas-1080p60";
  std::string outputResolution_ = "1920x1080";
  int outputWidth_ = 1920;
  int outputHeight_ = 1080;
  int outputFps_ = 60;
  double outputTargetBitrateMbps_ = 8.2;
  std::string streamVideoCodec_ = "h264";
  int recordingOutputWidth_ = 1920;
  int recordingOutputHeight_ = 1080;
  int recordingOutputFps_ = 60;
  double recordingTargetBitrateMbps_ = 8.2;
  int recordingAudioBitrateKbps_ = 192;
  std::string recordingVideoCodec_ = "h264";
  std::vector<modules::OutputDestinationSettings> outputDestinationSettings_;
  modules::CompositorColorGrade colorGrade_;
  std::vector<std::string> sceneValidationWarnings_;
  int64_t mixedAudioFrameCount_ = 0;
  std::map<std::string, std::vector<float>> routedBusPcm_;
  // Rolling deinterleaved program audio (<= 3 s) feeding the BS.1770 master
  // meter, plus the gated running accumulation for integrated loudness.
  std::vector<float> programMeterL_;
  std::vector<float> programMeterR_;
  modules::Bs1770IntegratedMeter programIntegratedMeter_{48000.0};
  double programLufsMomentary_ = -120.0;
  double programLufsShortTerm_ = -120.0;
  double programLufsIntegrated_ = -120.0;
  double programTruePeakDbfs_ = -120.0;
  std::chrono::steady_clock::time_point lastLoudnessCompute_{};
  modules::ProgramFrame lastProgramFrame_;
  std::string encoderLifecycleStatus_ = "idle";
  std::string encoderLastTransition_ = "Encoder session idle.";
  double encoderPreparedAtMs_ = 0;
  double encoderStartedAtMs_ = 0;
  double encoderStoppedAtMs_ = 0;
  std::string recordingSessionId_;
  std::string recordingStatus_ = "stopped";
  std::string recordingWriterStatus_ = "stopped";
  std::string recordingTargetFolder_ = "Recordings/CoreVideo Pro/native-core";
  std::string recordingFilenamePrefix_ = "program";
  std::string recordingFormat_ = "mp4";
  std::string recordingQuality_ = "high";
  std::vector<std::string> recordingIsoParticipantIds_;
  double recordingStartedAtMs_ = 0;
  double recordingElapsedMs_ = 0;
  int64_t recordingProgramFramesWritten_ = 0;
  int64_t recordingIsoFramesWritten_ = 0;
  int64_t recordingDroppedFrames_ = 0;
  int64_t recordingAudioPacketsObserved_ = 0;
  int recordingFailureCount_ = 0;
  int recordingRecoveryCount_ = 0;
  std::string recordingError_;
  std::string recordingWarning_;
  std::string recordingLastFailure_;
  std::string recordingLastRecovery_;
  std::unique_ptr<modules::ZoomEngineRuntime> zoomEngineRuntime_;
  bool zoomJoined_ = false;
  mutable int zoomSnapshotTick_ = 0;
  std::string zoomDisplayName_ = "Guest Producer";
  std::string breakoutRoomId_ = "main";
  std::string breakoutRoomName_ = "Main room";
  struct ParticipantAudioChannelInput {
    std::string participantId;
    int inputLevel = 0;
    bool muted = false;
    bool noiseSuppression = false;
    double manualGainDb = 0;
    bool hasManualGain = false;
    double pan = 0;
    bool solo = false;
    std::vector<std::string> pluginInserts;
  };
  std::vector<ParticipantAudioChannelInput> audioChannels_;
  bool audioLimiterEnabled_ = true;
  bool audioMonitorEnabled_ = false;
  std::string audioMonitorDeviceId_;
  std::string audioMonitorDeviceName_;
  double audioMonitorVolume_ = 0.0;
  std::string audioMonitorStatus_ = "muted";
  int64_t audioMonitorFramesPlayed_ = 0;
  std::string audioMonitorWarning_;
  struct AudioRoutingSendInput {
    std::string sourceId;
    std::string busId;
    double gainDb = 0;
    std::vector<std::string> busPluginInserts;
  };
  std::vector<AudioRoutingSendInput> audioRoutingSends_;
  std::vector<std::string> audioRoutingWarnings_;
  bool audioRoutingSynced_ = false;
  struct CaptureAudioSourceInput {
    std::string captureDeviceId;
    std::string audioDeviceId;
    std::string audioDeviceName;
    std::string audioSourceKind = "none";
    std::string nativeAudioDeviceId;
    std::string audioDriverName;
    int audioSyncOffsetMs = 0;
    bool embedded = false;
  };
  std::vector<CaptureAudioSourceInput> captureAudioSources_;
  std::vector<modules::CaptureAudioSourceConfig> lastCaptureAudioSourceConfigs_;
  bool captureAudioSourcesSynced_ = false;
  bool captionEnabled_ = true;
  std::string captionText_;
  std::string captionSpeaker_;
  double captionAtMs_ = 0;
  int captionConfidence_ = 0;
  std::vector<std::string> captionWarnings_;
  std::string brandName_ = "CoreVideo Pro House";
  std::string brandLogoText_ = "CoreVideo Pro";
  std::string brandColor_ = "#44c1a1";
  std::string brandAccentColor_ = "#f0a85c";
  std::string brandBackgroundColor_ = "#0c1118";
  std::string brandFontFamily_ = "Inter";
  std::string brandLowerThirdStyle_ = "gradient";
  std::string brandCaptionStyle_ = "medium sentence captions";
  std::string brandDefaultOverlayBehavior_ = "all-off";
  std::vector<std::string> brandWarnings_;
  std::string mediaPlaybackAssetId_;
  std::string mediaPlaybackAssetName_;
  std::string mediaPlaybackAssetKind_;
  std::string mediaPlaybackAssetPath_;
  std::string mediaPlaybackKey_;
  bool mediaPlaybackPlaying_ = false;
  std::vector<std::string> mediaPlaybackWarnings_;
  std::vector<rpc::Json> pendingProgramFramePreviewEvents_;
  std::vector<rpc::Json> pendingProgramSharedTextureEvents_;
  // Throttles base64 program-preview/shared-texture stdout events so they don't
  // flood the RPC channel and starve command responses (see JsonRpcServer::run).
  std::chrono::steady_clock::time_point lastFrameEventEmit_{};
};

}  // namespace corevideo::core
