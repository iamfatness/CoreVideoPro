#pragma once

#include "core/Director.h"
#include "core/PluginHostScan.h"
#include "modules/PluginHostClient.h"
#include "modules/AudioDsp.h"
#include "modules/AudioMastering.h"
#include "modules/VirtualCameraPublisher.h"
#include "modules/Interfaces.h"
#include "modules/StillMediaFrameCache.h"
#include "modules/ZoomEngineRuntime.h"
#include "rpc/Json.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
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
  [[nodiscard]] rpc::Json connectCaptureDevice(const std::string& deviceId,
                                               const std::string& outputSourceId = std::string());
  [[nodiscard]] rpc::Json disconnectCaptureDevice(const std::string& deviceId);
  void registerCaptureShm(const std::string& deviceId, const std::string& shmName, int width, int height);
  void unregisterCaptureShm(const std::string& deviceId);
  [[nodiscard]] rpc::Json joinZoom(const rpc::Json& payload);
  // True when a real Zoom engine subprocess is configured. Lock-free (the
  // runtime pointer and its executable path are fixed at construction). The
  // RPC server uses this to route zoom-join AROUND coreMutex: in this mode
  // joinZoom is a pure runtime passthrough that blocks for seconds (process
  // spawn + SDK auth + join handshake) and must not freeze the render thread.
  [[nodiscard]] bool zoomEngineConfigured() const;
  [[nodiscard]] rpc::Json leaveZoom();
  [[nodiscard]] rpc::Json zoomSnapshot() const;
  [[nodiscard]] rpc::Json syncZoomMediaSpine(const rpc::Json& payload, double elapsedMs);
  [[nodiscard]] std::vector<rpc::Json> drainZoomVideoFrameEvents();
  [[nodiscard]] std::vector<rpc::Json> drainProgramFramePreviewEvents();
  [[nodiscard]] std::vector<rpc::Json> drainProgramSharedTextureEvents();
  [[nodiscard]] std::vector<rpc::Json> drainParticipantSharedTextureEvents();
  [[nodiscard]] std::vector<rpc::Json> drainMultiviewSharedTextureEvents();
  [[nodiscard]] std::vector<rpc::Json> drainPreviewSharedTextureEvents();
  [[nodiscard]] rpc::Json applyCommand(const rpc::Json& command);
  [[nodiscard]] rpc::Json applyCommands(const rpc::Json::Array& commands, double elapsedMs = 0.0);

  // Light, video-only render tick for the operator program display. Renders the
  // GPU compositor and emits the shared-texture handle, but skips the audio mix,
  // monitor/output senders, encoder submit and base64 preview readback — i.e. no
  // blocking I/O — so it can run at ~60fps on the command-processing thread
  // without starving RPC command handling. The full renderSyntheticTick (encoder/
  // recording/streaming/audio) still runs on the host-sync cadence.
  void renderDisplayTick();

  // Phase 2 audio/output decouple. The audio mix, routed-bus matrix, monitor
  // device render, BS.1770 loudness, encoder submit, output-sender network sync
  // and recording mux are heavy / blocking and previously ran on the command
  // thread inside the full renderSyntheticTick(videoOnly=false). renderAudioOutputTick
  // runs that work on a dedicated worker thread instead, using a gather→work→publish
  // handoff so the render thread (which holds only `coreMutex`) is never blocked by
  // it: the worker takes `coreMutex` only briefly to gather inputs / publish results,
  // and `audioOutputMutex_` (NEVER both at once) for the long DSP/IO span. The passed
  // `coreMutex` is the JsonRpcServer's single big lock; lock order is
  // coreMutex(outer) → audioOutputMutex_(inner). Call WITHOUT either lock held.
  void renderAudioOutputTick(std::mutex& coreMutex);
  // Marks that a dedicated audio/output worker thread now drives renderAudioOutputTick,
  // so the synchronous command-thread path (renderSyntheticTick(videoOnly=false) and
  // the empty-poll tick in applyCommands) stops doing the audio/output work itself.
  // Direct/unit-test callers leave this false so applyCommands stays synchronous.
  void enableAudioOutputWorker();

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

  // Test seam for the still-media decode path (WIC is Windows-only, so the
  // stub/cross-platform tests inject a fake decoder). Call before loading
  // scenes; recreates the cache with the given decoder + byte budget.
  void setStillImageDecoderForTest(std::unique_ptr<modules::IStillImageDecoder> decoder,
                                   size_t cacheBudgetBytes = modules::StillMediaFrameCache::kDefaultCacheBudgetBytes);
  [[nodiscard]] modules::StillMediaFrameCache* stillMediaCacheForTest() { return stillMediaCache_.get(); }

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
  void syncVirtualCamera(const rpc::Json& command);
  [[nodiscard]] rpc::Json virtualCameraState() const;
  void syncAudioMonitor(const rpc::Json& command);
  // VST host P1 (docs/vst-host-spec.md): operator-initiated plugin discovery.
  // Runs `corevideo-plugin-host --scan` on a detached thread; results live
  // behind pluginHostMutex_ (a leaf lock — held briefly by the thread and by
  // snapshot export, never around any other lock).
  void startPluginHostScan();
  // P2b-2: idempotent async start of the resident serve host (worker-safe:
  // only flips an atomic + detaches a starter thread).
  void ensurePluginHostServeStarted();
  [[nodiscard]] rpc::Json pluginHostState() const;
  void syncAudioRoutingMatrix(const rpc::Json& command);
  void syncCaptureAudioSources(const rpc::Json& command);
  void pushCaptionCue(const rpc::Json& command);
  void setCaptionEnabled(const rpc::Json& command);
  void setBrandKit(const rpc::Json& command);
  void setMediaPlayback(const rpc::Json& command);
  void setMultiviewLayout(const rpc::Json& command);
  // Handles the configure-multiviewer command: stores the user-selected layout
  // mode, tile count, and the label/tally/meters/clock toggles, which the next
  // multiview composite reads. Unknown fields are ignored gracefully.
  void configureMultiviewer(const rpc::Json& command);
  // Parses + applies a multiview layout node ({canvasWidth,canvasHeight,cols,rows,
  // sources:[...]}) shared by the standalone set-multiview-layout command AND the
  // frequent zoom-media-spine-sync `multiview` object. Cheap signature compare:
  // only rebuilds multiviewSources_/dims and resets the structural-emit flag when
  // the layout content actually changed. Returns true when it applied a change.
  bool applyMultiviewLayout(const rpc::Json& layout);
  // Applies a preview-scene node ({sceneId, routes:[...], background, colorGrade,
  // overlays:[...]}) carried by the set-preview-scene command AND the frequent
  // zoom-media-spine-sync `previewScene` object. Cheap signature compare (mirrors
  // applyMultiviewLayout): only rebuilds the preview scene state and forces the
  // next preview composite when the content actually changed. Returns true when it
  // applied a change.
  bool applyPreviewScene(const rpc::Json& previewScene);
  // Recomputes the union of still-image media routes across the PROGRAM and
  // PREVIEW scenes and hands it to the still-media cache (which decodes on its
  // own background thread — never under coreMutex). Called from both scene
  // parse sites; cheap (string scan + leaf-mutex publish, no file I/O).
  void syncStillMediaDesired();
  void configureSrtIngestSources(const rpc::Json& command);
  void simulateBreakoutRoomChange(const rpc::Json& command);
  void renderSyntheticTick(bool videoOnly = false);
  void enqueueProgramFramePreviewEvent();
  void enqueueProgramSharedTextureEvent();
  void enqueueParticipantSharedTextureEvents();
  // Enqueues a multiview-shared-texture event, but only when the multiview
  // structure (handle/dimensions/tile identity+geometry) changes or on the
  // first emit (cold start) — never per-frame. The active-speaker border is
  // baked into the texture in-core, so a speaker change alone does not re-emit
  // (avoids the WinUI churn that the per-tile path crashed on).
  void enqueueMultiviewSharedTextureEvent();
  // Enqueues a preview-shared-texture event whenever a fresh preview handle is
  // present. The handle is tiny (like the program one) so it is emitted every
  // render to drive the preview GPU present at the full render rate.
  void enqueuePreviewSharedTextureEvent();
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
    // Per-layer opacity (scenes redesign S1): the compositor has always
    // supported CompositorRenderPlanLayer.opacity; this carries it through the
    // scene graph so the UI can finally reach it.
    float opacity = 1.f;
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
  // Sibling of buildCompositorRenderPlan that lays out the multiview grid: one
  // layer per layout entry placed in an aspect-aware grid cell, with an
  // active-speaker border on the layer whose participant matches the core's
  // current activeSpeakerId. Reuses the same source-id conventions as the
  // program plan so resolveLayers/frameForParticipant match the same frames.
  [[nodiscard]] modules::CompositorRenderPlan buildMultiviewRenderPlan(const std::vector<modules::VideoFrame>& videoFrames) const;
  // Builds the per-tile rect list (geometry + role + tally + label) for the
  // current multiview layout mode, matching the geometry buildMultiviewRenderPlan
  // places its layers into. Includes the PGM/PVW cells for the pgmPvw modes.
  [[nodiscard]] std::vector<modules::MultiviewTileRect> buildMultiviewTiles(const std::string& activeSpeakerId) const;
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
  // Shared render-plan builder parameterized on the scene state, so the PROGRAM
  // (active-scene members) and PREVIEW (preview-scene members) paths produce the
  // identical layer layout from their own state. Declared here (not with the other
  // build* methods) so OverlayAssetState/SceneRouteState are already in scope.
  // Brand-kit styling is global and still read from members inside.
  [[nodiscard]] modules::CompositorRenderPlan buildRenderPlanForScene(
      const std::string& sceneId,
      int routeCount,
      int overlayCount,
      const SceneBackgroundState& background,
      const std::vector<SceneRouteState>& routes,
      const modules::CompositorColorGrade& colorGrade,
      const std::map<std::string, OverlayAssetState>& overlays,
      bool captionEnabled,
      const std::string& captionText,
      const std::string& captionSpeaker,
      const std::vector<modules::VideoFrame>& videoFrames) const;
  // Builds the render plan for the PREVIEW scene from the preview-scene members,
  // mirroring buildCompositorRenderPlan for the program scene.
  [[nodiscard]] modules::CompositorRenderPlan buildPreviewCompositorRenderPlan(const std::vector<modules::VideoFrame>& videoFrames) const;
  // True when a preview scene with at least one route/overlay/background is set, so
  // the render tick knows to run the (opt-in) third preview composite.
  [[nodiscard]] bool hasPreviewScene() const;
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
  // ---- PREVIEW scene (the preview composite bus) ----
  // Parallel to the active/program scene members above. Synced via set-preview-scene
  // (and the spine `previewScene` object), composited into previewSharedTexture on
  // the light render tick. previewSceneActive_ flips true once a scene is set;
  // previewSceneSignature_ dedups the sync (mirrors multiviewLayoutSignature_).
  bool previewSceneActive_ = false;
  std::string previewSceneId_ = "unloaded";
  std::vector<SceneRouteState> previewSceneRoutes_;
  SceneBackgroundState previewSceneBackground_;
  int previewRouteCount_ = 0;
  int previewOverlayCount_ = 0;
  modules::CompositorColorGrade previewColorGrade_;
  std::map<std::string, OverlayAssetState> previewOverlayAssets_;
  std::string previewSceneSignature_;
  // Structural signature (handle + dims) of the last emitted preview-shared-texture
  // event, so it is emitted only on structural change (and once at cold start) —
  // the live pixels flow through the stable keyed-mutex texture, presented
  // continuously by the host (mirrors the multiview event gating).
  uint32_t lastPreviewStructureSignature_ = 0;
  bool previewStructureEmitted_ = false;
  std::vector<rpc::Json> pendingPreviewSharedTextureEvents_;
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
  // Persistent decoded still-image frames for media routes (logos/bugs), keyed
  // "media:<assetId>". Decode runs on the cache's own worker thread; the render
  // gather only does cheap shared_ptr copies (see StillMediaFrameCache.h).
  std::unique_ptr<modules::StillMediaFrameCache> stillMediaCache_ =
      std::make_unique<modules::StillMediaFrameCache>();
  std::unique_ptr<modules::IVirtualCameraPublisher> virtualCamera_ = modules::createVirtualCameraPublisher();
  bool virtualCameraEnabled_ = false;
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
    // C5b: per-insert parameter overrides {insertName -> {param -> value}};
    // missing entries mean the DSP defaults.
    std::map<std::string, std::map<std::string, double>> insertSettings;
  };
  std::vector<ParticipantAudioChannelInput> audioChannels_;
  bool audioLimiterEnabled_ = true;
  modules::MasteringParams masteringParams_;          // set under coreMutex (sync command)
  modules::MasteringState masteringState_;            // touched ONLY under audioOutputMutex_
  double audioMasteringRideDb_ = 0.0;                 // published telemetry (coreMutex)
  bool audioMonitorEnabled_ = false;
  std::string audioMonitorDeviceId_;
  std::string audioMonitorDeviceName_;
  double audioMonitorVolume_ = 0.0;
  std::string audioMonitorStatus_ = "muted";
  int64_t audioMonitorFramesPlayed_ = 0;
  int64_t audioMonitorUnderruns_ = 0;  // cumulative device-dry gaps (spec R5)
  bool audioMonitorFeedbackRisk_ = false;  // monitor endpoint == loopback endpoint (spec R6)
  // C7b: latest per-source compressor GR (published from the worker; read by
  // the audioMixSession export under audioOutputMutex_).
  std::map<std::string, double> audioCompGainReductionDbBySource_;
  // C7c: persistent per-source DSP state (audioOutputMutex_ domain — built and
  // consumed only inside runAudioOutputWork). Without this, biquads/envelopes
  // restart every 20ms block = audible buzz (owner-reported mic distortion).
  std::map<std::string, modules::ChannelDspState> channelDspStates_;
  // C7d: per-bus limiter gain state (same block-continuity requirement).
  std::map<std::string, modules::LimiterState> busLimiterGains_;
  // Bus-send re-limit state: summing a bus into a target can exceed the
  // target's already-limited ceiling; the post-sum pass needs its own
  // persistent gain (worker domain, same continuity law as above).
  std::map<std::string, modules::LimiterState> busSendLimiterGains_;
  // Spec 4.2: per-source sample-steady feed FIFOs (worker domain).
  std::map<std::string, modules::AudioFeedState> audioFeedStates_;
  // Mix-ingest resampler states (worker domain): every source lands on the
  // bus rate before any sum (Zoom SDK delivers 32k; the mic is 48k).
  std::map<std::string, modules::LinearResampleState> audioResampleStates_;
  // Click-hunt hold-last guards: consecutive empty control-plane syncs seen
  // while live state was non-empty (adopt a true clear-all after 25).
  int emptyRoutingSyncStreak_ = 0;
  int emptyMixSyncStreak_ = 0;
  std::string audioMonitorWarning_;
  // VST host P1: scan results behind a dedicated leaf mutex (the detached scan
  // thread cannot take coreMutex, which the server owns).
  // P2b-2: the live transport client. exchange() runs in the audio worker
  // (audioOutputMutex_ span, single caller); start() runs on a detached
  // starter (ready_ is atomic) - NEVER spawned from the worker.
  modules::PluginHostClient pluginHostClient_;
  std::atomic<bool> pluginHostServeStarting_{false};
  mutable std::mutex pluginHostMutex_;
  std::string pluginHostStatus_ = "absent";  // absent|scanning|ready|error
  std::vector<PluginHostPluginInfo> pluginHostPlugins_;
  bool pluginHostScanInFlight_ = false;
  struct AudioRoutingSendInput {
    std::string sourceId;
    std::string busId;
    double gainDb = 0;
    std::vector<std::string> busPluginInserts;
  };
  std::vector<AudioRoutingSendInput> audioRoutingSends_;
  // Bus OUTPUT routing (mixer topology): an aux/custom bus's mix summed into a
  // fixed destination bus, like a subgroup feeding the master on a real desk.
  // Without these, aux buses were metered dead ends (owner 2026-07-12).
  struct AudioBusSendInput {
    std::string fromBusId;
    std::string toBusId;
    double gainDb = 0;
  };
  std::vector<AudioBusSendInput> audioBusSends_;
  // PFL/listen: when set, the monitor renders THIS bus instead of "mon".
  std::string monitorListenBusId_;
  // Per-source/channel hold-last: previous non-empty config + absence streaks
  // (partial syncs during shell row-rebuild churn must not unroute live audio).
  std::vector<AudioRoutingSendInput> previousRoutingSends_;
  std::map<std::string, int> absentRoutingSourceStreaks_;
  std::vector<ParticipantAudioChannelInput> previousAudioChannels_;
  std::map<std::string, int> absentMixChannelStreaks_;
  std::vector<std::string> audioRoutingWarnings_;
  bool audioRoutingSynced_ = false;
  std::vector<std::string> outputDestinations_;

  // ---- Phase 2 audio/output decouple (gather → work → publish) ----
  // Per-tick inputs gathered under `coreMutex` (plain-data copies + freshly polled
  // audio frames + a copy of the current program frame for the encoder/output).
  struct AudioOutputWorkItem {
    bool valid = false;
    int64_t frameIntervalMs = 16;
    std::vector<modules::AudioFrame> audioFrames;
    std::vector<ParticipantAudioChannelInput> channels;
    std::vector<AudioRoutingSendInput> routingSends;
    std::vector<AudioBusSendInput> busSends;  // bus -> bus outputs (aux/subgroup routing)
    std::string monitorListenBusId;           // PFL: monitor auditions this bus (else "mon")
    bool limiterEnabled = true;  // spec 4.4: toggle now controls the bus limiter
    modules::MasteringParams masteringParams;  // mastering-chain-spec M1
    bool audioMonitorEnabled = false;
    double audioMonitorVolume = 0.0;
    // Resolved endpoint ids of ACTIVE loopback capture sources, gathered under
    // coreMutex; the monitor block compares them against the monitor's own
    // resolved endpoint to flag a feedback loop (spec R6).
    std::vector<std::string> loopbackCaptureEndpointIds;
    bool recordingActive = false;
    std::vector<std::string> recordingIsoParticipantIds;
    std::vector<std::string> outputDestinations;
    std::vector<modules::OutputDestinationSettings> outputDestinationSettings;
    modules::ProgramFrame programFrame;
    int64_t tickId = 0;
  };
  // Results produced under `audioOutputMutex_` and published back under `coreMutex`.
  struct AudioOutputResults {
    bool valid = false;
    double masteringRideDb = 0.0;  // telemetry (never a setting - law 5)
    std::map<std::string, std::vector<float>> routedBusPcm;
    int64_t mixedFrameCount = 0;
    bool monitorTouched = false;  // only overwrite monitor status when the monitor ran
    std::string monitorStatus;
    std::string monitorWarning;
    int64_t monitorFramesPlayedDelta = 0;
    int64_t monitorUnderruns = 0;  // cumulative device-dry gaps (spec R5)
    bool monitorFeedbackRisk = false;
    // C7b: per-source compressor gain reduction this tick (dB, >0 only).
    std::map<std::string, double> compGainReductionDbBySource;
    bool recordingActive = false;
    int64_t recordingProgramFramesDelta = 0;
    int64_t recordingIsoFramesDelta = 0;
    int64_t recordingAudioPacketsObserved = 0;
    double recordingElapsedMsDelta = 0.0;
  };
  // Gather reads `coreMutex`-domain state (members + zoom/capture/media modules);
  // run touches ONLY `audioOutputMutex_`-domain modules (mixer/monitorOutput/encoder/
  // outputSender) + the BS.1770 loudness members; publish writes `coreMutex`-domain
  // published members. The caller owns the locking (so the same trio serves both the
  // worker — phased/locked — and the synchronous test path — single-threaded, no locks).
  [[nodiscard]] AudioOutputWorkItem gatherAudioOutputWork();
  [[nodiscard]] AudioOutputResults runAudioOutputWork(AudioOutputWorkItem& work);
  void publishAudioOutputResults(const AudioOutputResults& results);
  // INNER lock guarding the audio/output module state (mixer, monitorOutput, encoder,
  // outputSender) + the BS.1770 loudness accumulators. Lock order: coreMutex → this.
  mutable std::mutex audioOutputMutex_;
  // Set once (before threads start) when a dedicated worker drives the audio/output
  // tick; flips the command-thread path to video-only. Plain bool: written before the
  // worker/render/command threads exist, then only read.
  bool audioWorkerActive_ = false;
  // Monotonic worker tick id (diagnostics; the single worker runs phases sequentially
  // so results can never reorder — they are dropped, never reordered, on overrun).
  int64_t audioOutputTickId_ = 0;
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
  // Ordered multiview layout (the Show Input roster the WinUI sends via
  // set-multiview-layout). Each entry is one tile; kind selects which feed the
  // grid cell samples. Empty until a layout is set, which keeps the multiview
  // pass (a second GPU composite) entirely opt-in.
  struct MultiviewSource {
    std::string sourceId;
    std::string kind;  // participant | capture | media
    std::string participantId;
    std::string captureDeviceId;
    std::string mediaAssetId;
    int slot = 0;
    std::string label;
  };
  std::vector<MultiviewSource> multiviewSources_;
  int multiviewCanvasWidth_ = 1920;
  int multiviewCanvasHeight_ = 1080;
  // User-selectable multiviewer configuration (configure-multiviewer command).
  // layoutMode: "grid" | "pgmPvwTop" | "pgmPvwLarge" | "pgmPvwSide".
  std::string multiviewLayoutMode_ = "grid";
  int multiviewTileCount_ = 8;
  bool multiviewShowLabels_ = true;
  bool multiviewShowTally_ = true;
  bool multiviewShowMeters_ = true;
  bool multiviewShowClock_ = true;
  // Content signature of the currently-applied multiview layout. The spine sync calls
  // applyMultiviewLayout every tick; this lets it cheaply skip the clear/rebuild +
  // structural-emit reset when the layout has not actually changed.
  std::string multiviewLayoutSignature_;
  // Structural signature of the last emitted multiview event, so the event is
  // emitted only on structural change (and once at cold start).
  uint32_t lastMultiviewStructureSignature_ = 0;
  bool multiviewStructureEmitted_ = false;
  std::vector<rpc::Json> pendingMultiviewSharedTextureEvents_;
  std::vector<rpc::Json> pendingProgramFramePreviewEvents_;
  std::vector<rpc::Json> pendingProgramSharedTextureEvents_;
  std::vector<rpc::Json> pendingParticipantSharedTextureEvents_;
  // Throttles base64 program-preview/shared-texture stdout events so they don't
  // flood the RPC channel and starve command responses (see JsonRpcServer::run).
  std::chrono::steady_clock::time_point lastFrameEventEmit_{};
};

}  // namespace corevideo::core
