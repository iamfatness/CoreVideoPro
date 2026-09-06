#pragma once

#include "compositor/TilesMembership.h"
#include "core/Director.h"
#include "core/RenderedProgramSources.h"
#include "core/PluginHostScan.h"
#include "modules/BrowserSourceHostAdapter.h"
#include "modules/PluginHostClient.h"
#include "modules/PluginHostRespawnPolicy.h"
#include "modules/AudioDsp.h"
#include "modules/AudioMastering.h"
#include "modules/VirtualCameraPublisher.h"
#include "modules/Interfaces.h"
#include "modules/StillMediaFrameCache.h"
#include "modules/ZoomEngineRuntime.h"
#include "rpc/Json.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace corevideo::core {

// Tiles wall layer (T1, docs/2026-08-15-corevideo-tiles-T1-core-wall):
// declared at namespace scope (NOT nested in MediaCore, unlike SceneRouteState)
// because MediaCore::tilesLayerForTest() returns a const reference to it from
// the public section, which needs the type complete at that point in the
// single-pass class parse — a nested definition further down (beside
// SceneRouteState) would still be an incomplete type there and fail to compile.
struct TilesStyle {
  std::string tileAspect = "16:9";     // 16:9|4:3|5:4|1:1|3:4|9:16|custom
  double customAspectRatio = 16.0 / 9.0;
  double gutterPercent = 0.741;
  double marginPercent = 0.741;
  std::string backgroundColor = "#000000";
};

struct TilesLayerState {
  bool present = false;
  std::string layerId;
  int order = 0;
  // NOTE the namespace: MediaCore.h is in `corevideo::core`, the rect type is
  // in `corevideo::modules`. Both CompositorLayerRect and the compositor's
  // LayerRect are {x, y, width, height} floats with identical layout, so
  // braced conversion between them in Task 4 is safe — but the qualification
  // is not optional and will not compile without it.
  modules::CompositorLayerRect rect{0.f, 0.f, 1.f, 1.f};
  std::vector<std::string> members;   // ordered "zoom:<pid>" / "capture:<id>"
  TilesStyle style;
};

class MediaCore {
 public:
  explicit MediaCore(modules::ModuleSet modules = modules::createDefaultModules());
  // Clears the compositor's vcam sink before members are destroyed (see the
  // definition — the publisher outlives nothing without it).
  ~MediaCore();

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
  // Browser sources (docs/capture-sources-spec.md Phase BR, slice BR-1). Each adds a
  // dedicated corevideo-browser-host.exe (WebView2) subprocess whose frames ride the
  // capture ingest seam keyed "capture:browser:<n>". Returns the browserSources state
  // (with per-source health) on success; a Json string error message on rejection.
  [[nodiscard]] rpc::Json addBrowserSource(const rpc::Json& payload, std::string& error);
  [[nodiscard]] rpc::Json removeBrowserSource(const std::string& browserId, std::string& error);
  [[nodiscard]] rpc::Json reloadBrowserSource(const std::string& browserId, std::string& error);
  [[nodiscard]] rpc::Json browserSourcesState() const;
  [[nodiscard]] rpc::Json joinZoom(const rpc::Json& payload, const std::function<bool()>& cancelled = {});
  // True when a real Zoom engine subprocess is configured. Lock-free (the
  // runtime pointer and its executable path are fixed at construction). The
  // RPC server uses this to route zoom-join AROUND coreMutex: in this mode
  // joinZoom is a pure runtime passthrough that blocks for seconds (process
  // spawn + SDK auth + join handshake) and must not freeze the render thread.
  [[nodiscard]] bool zoomEngineConfigured() const;
  [[nodiscard]] rpc::Json leaveZoom();
  // Capture-off (rpc "zoom-stop-capture"): stops Zoom raw media in the engine
  // (StopRawRecording clears the participant-facing recording indicator +
  // unsubscribe_all stops frames) while STAYING in the meeting. Non-blocking:
  // the engine command rides ZoomEngineRuntime's sender thread.
  [[nodiscard]] rpc::Json stopZoomCapture();
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
  // blocking I/O — driven at ~60fps by the dedicated display worker under
  // coreMutex. Live command batches apply state without rendering; the next
  // display tick publishes the corresponding frame. Audio/output has its own worker.
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
  // Program VIDEO out on the video cadence (60Hz), so recordings/senders are not
  // sampled through the audio worker's 20ms grid. Call setVideoOutputTickRunning
  // before driving it, or the audio worker will submit video too.
  void renderVideoOutputTick(std::mutex& coreMutex);
  // Wake the video-out tick after a render. MUST be called with coreMutex
  // RELEASED — notifying under it wakes a thread that instantly blocks on it.
  void notifyProgramFramePublished() { videoOutCv_.notify_one(); }
  // Render pacer telemetry arrives from JsonRpcServer's render thread outside
  // coreMutex. Keep a monotonic atomic total so UI/support evidence cannot lose
  // the 120-frame summaries that are printed and then reset in the log loop.
  void reportRenderDeadlineMisses(int64_t count) {
    if (count > 0) {
      renderDeadlineMisses_.fetch_add(count, std::memory_order_relaxed);
    }
  }
  void setVideoOutputTickRunning(bool running) {
    videoOutputTickRunning_.store(running, std::memory_order_release);
  }
  // Enables live worker ownership: audio/output drives renderAudioOutputTick and
  // the display worker drives video rendering. Command batches and startup command
  // helpers stop rendering synthetic ticks, including nonempty batches and polls.
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

  // T1: the PROGRAM-bus tiles wall parsed off the load-scene-graph command,
  // and the scene validation warnings a bad/unrecognised value gets recorded
  // into (loud, never silent — see parseTilesLayer in MediaCore.cpp).
  const TilesLayerState& tilesLayerForTest() const { return tilesLayer_; }
  // Task 4 review fix (C3): the PREVIEW bus's own wall (applyPreviewScene),
  // now a SEPARATE field from tilesLayer_ above — see previewTilesLayer_'s
  // declaration for why sharing one field was a live-show bug, not just a
  // test-seam gap.
  const TilesLayerState& previewTilesLayerForTest() const { return previewTilesLayer_; }
  const std::vector<std::string>& sceneValidationWarningsForTest() const {
    return sceneValidationWarnings_;
  }
  // Task 4: injects the per-member frame-age snapshot the wall expansion
  // consults to decide who is drawn (compositor::admitTilesMembers). In
  // production this is populated from the live videoFrames gather each render
  // tick (see renderSyntheticTick); tests drive it directly since a unit test
  // has no real decoded frames to age.
  //
  // Also refreshes lastRenderPlanForTest()'s cached plan. A full render tick
  // (renderSyntheticTick) would immediately re-derive tilesMemberFrameAges_
  // from the live videoFrames gather and overwrite what this call just set
  // (empty in a bare test core, with no real source) before the plan is even
  // built -- so this calls buildCompositorRenderPlan directly, the same
  // production builder the tick uses, without going through that gather.
  // Tests call this AFTER loading the scene and expect lastRenderPlanForTest()
  // to reflect it immediately, with no further command in between.
  void setTilesMemberFrameAgesForTest(std::vector<compositor::TilesMemberFrameAge> ages) {
    tilesMemberFrameAges_ = std::move(ages);
    lastRenderPlan_ = buildCompositorRenderPlan({});
  }
  // buildRenderPlanForScene takes 10+ arguments (MediaCore.cpp) — do NOT try to
  // call it directly from a test. This exposes what the render tick actually
  // built (assigned at the buildCompositorRenderPlan call site), so the test
  // observes the production path's own output rather than a fabricated call.
  const modules::CompositorRenderPlan& lastRenderPlanForTest() const {
    return lastRenderPlan_;
  }

  // A2 (round-2 PR 2): param bridge + state persistence commands, called by
  // JsonRpcServer's dedicated routes (hence public). All control-plane — they
  // touch only leaf mutexes and the client's dedicated param/state events,
  // NEVER the realtime audio req/done exchange.
  void setVstInsertParam(const rpc::Json& command);
  // Pull command: fetches the plugin's CURRENT component state fresh from the
  // isolated host; returns {stateBase64,bytes} or {error} — loud, never a
  // silent default blob.
  [[nodiscard]] rpc::Json getVstInsertState(const rpc::Json& command);
  // Caches a saved state blob per selection query and injects it into every
  // host generation that has not received it yet — including after each
  // respawn (closes "respawn loses state"). Injection retries on scan
  // completion + host launch, so pushing states before the first scan
  // resolves is safe.
  void setVstInsertState(const rpc::Json& command);
  // Injection worker body (detached threads + the serve launch thread call
  // it; never the audio worker, never under coreMutex).
  void injectPendingVstStates();

 private:
  // Batch commands apply in order, then capture one response. Building and
  // discarding a full session snapshot per command repeatedly takes module locks.
  void applyCommandMutation(const rpc::Json& command);
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
  // Read the ISO source selection from a recording command: prefers
  // `isoSourceIds` (scheme-qualified `zoom:<pid>`/`capture:<id>`), falls back to
  // legacy `isoParticipantIds` (bare ids). Returns the RAW list as given.
  [[nodiscard]] std::vector<std::string> readIsoSourceIds(const rpc::Json& command) const;
  // Normalize a raw ISO id to its canonical form: a bare (scheme-less) id is a
  // Zoom participant, so `zoom:<id>`; anything already scheme-qualified is kept.
  [[nodiscard]] static std::string normalizeIsoSourceId(const std::string& rawId);
  // Resolve a canonical ISO source id to a human display name (Zoom userId →
  // roster displayName; `capture:<id>` → the capture device name, or a browser
  // source's URL host; ISO-3). Falls back to the bare id tail.
  [[nodiscard]] std::string resolveIsoDisplayName(const std::string& sourceId) const;
  // ISO-3 audio pairing: does this ISO source carry its own audio stem? A Zoom
  // participant always does; a `capture:<id>` source does ONLY when the operator
  // paired an audio input to that capture device (captureAudioSources_). A pure
  // camera → false → a VIDEO-ONLY ISO (no all-silence audio track).
  [[nodiscard]] bool isoSourceHasAudio(const std::string& sourceId) const;
  // The current ISO selection as canonical scheme-qualified ids (`zoom:<pid>` /
  // `capture:<id>`), for the snapshot `isoSourceIds` mirror.
  [[nodiscard]] std::vector<std::string> canonicalIsoSourceIds() const;
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
  void openVstPluginEditor(const rpc::Json& command);
  // P2c: resolves a "vst:<name>" insert query against the scan results.
  // Worker-safe: takes only the leaf pluginHostMutex_, briefly. Failures are
  // recorded (snapshot serve.lastError) + logged rate-capped — loud, never fake.
  [[nodiscard]] VstInsertSelection resolveVstInsertForWorker(const std::string& query);
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
    // "none" by default: borders are opt-in styling; PROGRAM (and the vcam/
    // recording/stream outputs downstream of it) must composite clean.
    std::string borderStyle = "none";
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
    bool hasChromaKey = false;
    modules::CompositorChromaKey chromaKey;
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
  // T1: the parsed PROGRAM-bus tiles wall layer (present==false when the
  // current scene carries none — reset every load-scene-graph sync so a wall
  // from a PRIOR scene can never survive a sync that omits it).
  //
  // Task 4 review fix (C3): this used to be ALSO written by applyPreviewScene
  // — a single field shared by both buses, read unconditionally by
  // buildRenderPlanForScene. Since applyPreviewScene rides the frequent spine
  // sync (not just an operator action), the first preview sync carrying no
  // `tiles` silently wiped the live PROGRAM wall mid-show, and one carrying a
  // DIFFERENT wall put a preview-only wall on air. previewTilesLayer_ below is
  // now the preview bus's own copy; buildRenderPlanForScene takes the wall to
  // expand as an explicit parameter (mirroring sceneRoutes), so each bus can
  // only ever see its own.
  TilesLayerState tilesLayer_;
  // Task 4 review fix (C3): the PREVIEW bus's own tiles wall, parsed by
  // applyPreviewScene. See tilesLayer_ above for why this must be a SEPARATE
  // field rather than shared.
  TilesLayerState previewTilesLayer_;
  // Task 4: per-member frame-age snapshot for the wall expansion, refreshed
  // every render tick from the live videoFrames gather (renderSyntheticTick,
  // under coreMutex — geometry bookkeeping, not pixel work). Covers members of
  // EITHER bus's wall (sourceIds are globally unique, so sharing this list is
  // safe and avoids computing it twice).
  std::vector<compositor::TilesMemberFrameAge> tilesMemberFrameAges_;
  // Task 4 review fix (I4): per-sourceId "when did this member's frame last
  // ACTUALLY change" bookkeeping, keyed off frameId rather than a producer's
  // re-stamped timestampMs (every frame producer in this codebase re-stamps
  // `timestampMs` with the current tick's clock even when re-serving a held/
  // frozen frame — see ZoomEngineRuntime.cpp and the capture adapters — so
  // `frameTimestampMs - frame.timestampMs` is ~0 for ANY live-but-frozen
  // source and the staleness filter (Task 3) could never actually fire). A
  // frameId only advances on a genuinely new frame (the ISO dedup relies on
  // the same property), so this is the source of truth for "is this member's
  // picture actually moving." Erased when a member stops appearing in
  // videoFrames at all, so a later return starts fresh rather than replaying
  // stale history.
  struct TilesFrameFreshness {
    bool everSeen = false;
    int64_t lastFrameId = -1;
    int64_t lastChangedTickMs = 0;
  };
  std::unordered_map<std::string, TilesFrameFreshness> tilesFrameFreshness_;
  // Task 4: the render plan the render tick actually built, cached for
  // lastRenderPlanForTest() and for the sessionState() `tiles` node — both
  // read the CORE's own produced layers rather than re-deriving with the
  // solver, so a test/consumer can never observe a plan the compositor did
  // not also receive.
  modules::CompositorRenderPlan lastRenderPlan_;
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
    // Generic overlays disabled directly by the native protocol retire when
    // their build-out completes. Shell-timed lower thirds remain as a fully
    // transparent, settled building-out layer until the shell sends its final
    // explicit hidden command. This prevents a late scene sync from recreating
    // the key for one frame at the transition boundary.
    bool retireAfterBuildOut = false;
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
      const std::vector<modules::VideoFrame>& videoFrames,
      // Task 4 review fix (C3): explicit per-bus wall, mirroring how `routes`
      // is already per-bus rather than a shared member. The PROGRAM caller
      // passes tilesLayer_, the PREVIEW caller passes previewTilesLayer_ —
      // never the same field for both.
      const TilesLayerState& wall) const;
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
  // Streaming true-peak detector state (B2): sinc history persists across
  // 20ms chunks so inter-sample peaks straddling a block edge are measured
  // correctly (the finite-buffer meter misreads there). Worker-only, under
  // audioOutputMutex_ like the loudness members above.
  modules::StreamingTruePeakMeterState programTruePeakMeterL_;
  modules::StreamingTruePeakMeterState programTruePeakMeterR_;
  std::chrono::steady_clock::time_point lastLoudnessCompute_{};
  modules::ProgramFrame lastProgramFrame_;
  RenderedProgramSources renderedProgramSources_;
  // Lock-free mirror of lastProgramFrame_.frameNumber for the audio worker's
  // pre-lock engine poll (see pollZoomAudioUnlocked).
  std::atomic<std::int64_t> lastProgramFrameNumberAtomic_{0};
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
  // Raw ISO source-id selection from the command (accepts `isoSourceIds` with
  // `zoom:<pid>`/`capture:<id>`, back-compat `isoParticipantIds` bare ids). The
  // canonical id + roster display name are resolved at request-build time.
  std::vector<std::string> recordingIsoParticipantIds_;
  // Latest per-ISO-source video frame snapshotted under coreMutex at the render
  // gather, keyed by canonical source id (`zoom:<pid>` / `capture:<id>`). Cheap
  // zero-copy VideoFrame refs (shared_ptr payloads) — NO pixel copy under the
  // lock. The audio worker's gather picks the selected sources into the ISO work.
  std::map<std::string, modules::VideoFrame> latestIsoSourceFrames_;
  double recordingStartedAtMs_ = 0;
  int recordingFailureCount_ = 0;
  int recordingRecoveryCount_ = 0;
  std::string recordingError_;
  std::string recordingWarning_;
  std::string recordingLastFailure_;
  std::string recordingLastRecovery_;
  std::unique_ptr<modules::ZoomEngineRuntime> zoomEngineRuntime_;
  // Owned directly (not inside ModuleSet) so the browser commands and snapshot
  // telemetry have typed access, and so the stub-module test path exercises the
  // same code. Frames merge into the capture-frame stream each render tick.
  std::unique_ptr<modules::BrowserSourceHostAdapter> browserSources_ =
      modules::createBrowserSourceHostAdapter();
  // Persistent decoded still-image frames for media routes (logos/bugs), keyed
  // "media:<assetId>". Decode runs on the cache's own worker thread; the render
  // gather only does cheap shared_ptr copies (see StillMediaFrameCache.h).
  std::unique_ptr<modules::StillMediaFrameCache> stillMediaCache_ =
      std::make_unique<modules::StillMediaFrameCache>();
  std::unique_ptr<modules::IVirtualCameraPublisher> virtualCamera_ = modules::createVirtualCameraPublisher();
  bool virtualCameraEnabled_ = false;
  // The compositor pushes the tap at render cadence, so the output worker must
  // not publish it too (that would double every frame).
  bool compositorPublishesVcam_ = false;
  // Set while a 60Hz video tick is driving program video out; the audio worker
  // then submits audio only. Atomic: read by the audio worker, written by the
  // server thread that owns the tick's lifetime.
  std::atomic<bool> videoOutputTickRunning_{false};
  // True while the senders have destinations. Lets the video tick run one more
  // time after outputs clear, so the stop-carrying sync() is actually delivered.
  std::atomic<bool> senderSyncActive_{false};
  std::atomic<int64_t> renderDeadlineMisses_{0};
  // The newest program NV12 tap. Video owns output submission independently of
  // audio; this small mutex protects only the shared immutable tap reference and
  // dimensions, never DSP, encoder or sender work.
  mutable std::mutex programNv12Mutex_;
  // takeVcamNv12 hands out each generation once, so exactly one caller may take.
  // Edge-trigger state for the video tick (coreMutex-guarded): the last program
  // frame it published, so a tick with nothing new costs one comparison instead
  // of a ProgramFrame copy and a duplicate submit.
  int64_t lastVideoOutFrameNumber_ = -1;
  // Destinations the tick last synced. A change must reach the senders even on a
  // tick with no new frame — that is how they get STOPPED.
  std::vector<std::string> lastVideoOutDestinations_;
  // Program-frame publish signal. The render thread bumps the counter and
  // notifies; the video-out tick waits on it instead of polling, so it wakes
  // once per real frame rather than acquiring coreMutex on a timer.
  std::atomic<uint64_t> programPublishSeq_{0};
  std::condition_variable videoOutCv_;
  std::mutex videoOutWaitMutex_;
  uint64_t lastVideoOutPublishSeq_ = 0;
  std::shared_ptr<const std::vector<std::uint8_t>> latestProgramNv12_;
  int latestProgramNv12Width_ = 0;
  int latestProgramNv12Height_ = 0;
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
  // P2c: last "vst:" insert-name resolution failure ("" = resolving fine),
  // surfaced as pluginHost.serve.lastError so a typo'd insert name is a
  // 10-second diagnosis, not silence.
  std::string pluginHostInsertError_;
  bool pluginHostScanAutoKicked_ = false;  // one-shot scan kick from the worker path
  // A1: serve respawn backoff (5→10→20→40→60s, give up after 5 consecutive
  // failures → insert stays loudly auto-bypassed). Guarded by pluginHostMutex_;
  // operator actions (new selection, "Open controls") reset the ladder.
  modules::PluginHostRespawnPolicy pluginHostRespawnPolicy_;
  std::string pluginHostLastSelectionKey_;
  bool pluginHostGaveUpAnnounced_ = false;  // one-shot stderr on give-up
  // A2: saved component-state blobs per "vst:" selection QUERY (the shell's
  // persistence key), injected into each host generation exactly once.
  // Guarded by pluginHostMutex_ (leaf). injectedHostGeneration tracks the
  // pluginHostClient_.startCount() the blob last landed in, so a respawned
  // host (new generation) gets it re-pushed.
  struct VstSavedState {
    std::vector<uint8_t> blob;
    int64_t injectedHostGeneration = 0;  // 0 = never injected
  };
  std::map<std::string, VstSavedState> vstSavedStates_;
  // A2: snapshot param surface cache — rebuilt ONLY when a host param
  // generation moved (never per snapshot tick). Guarded by pluginHostMutex_.
  mutable rpc::Json cachedVstParamsJson_ = rpc::Json::Array{};
  mutable uint32_t cachedVstParamsListGeneration_ = 0;
  mutable uint32_t cachedVstParamsValuesGeneration_ = 0;
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
    // Per-source ISO video for this tick (ISO-1), zero-copy shared_ptr refs
    // snapshotted from latestIsoSourceFrames_ under coreMutex at gather.
    std::vector<modules::IsoSourceVideoFrame> isoSources;
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
    // Encoder-side recording warning (e.g. "Media Foundation dropped program
    // audio: ..."), published into recordingWarning_ so the snapshot's
    // `recording.warning` surfaces encoder failures. Before this, a recording
    // muxing ZERO audio packets showed a clean recording section for its whole
    // duration (2026-07-13 alpha-blocking zero-audio bug).
    std::string recordingWarning;
  };
  // Gather reads `coreMutex`-domain state (members + zoom/capture/media modules);
  // run touches ONLY `audioOutputMutex_`-domain modules (mixer/monitorOutput/encoder/
  // outputSender) + the BS.1770 loudness members; publish writes `coreMutex`-domain
  // published members. The caller owns the locking (so the same trio serves both the
  // worker — phased/locked — and the synchronous test path — single-threaded, no locks).
  [[nodiscard]] std::vector<modules::AudioFrame> pollZoomAudioUnlocked();
  [[nodiscard]] AudioOutputWorkItem gatherAudioOutputWork(
      std::vector<modules::AudioFrame> prePolledZoomAudio);
  [[nodiscard]] AudioOutputResults runAudioOutputWork(AudioOutputWorkItem& work);
  void publishAudioOutputResults(const AudioOutputResults& results);
  // INNER lock guarding the audio/output module state (mixer, monitorOutput, encoder,
  // outputSender) + the BS.1770 loudness accumulators. Lock order: coreMutex → this.
  mutable std::mutex audioOutputMutex_;
  // Video encoder/sender submission must never make the 20 ms audio worker wait.
  // The concrete sinks are asynchronous/thread-safe; this lock only serializes
  // video ticks with each other.
  mutable std::mutex videoOutputMutex_;
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
  std::uint64_t multiviewTickCounter_ = 0;
  // Published multiview identity, held across throttled composites.
  modules::ProgramFrameSharedTexture lastMultiviewTexture_;
  std::vector<modules::MultiviewTileRect> lastMultiviewTiles_;
  int lastMultiviewWidth_ = 0;
  int lastMultiviewHeight_ = 0;
  std::vector<rpc::Json> pendingMultiviewSharedTextureEvents_;
  std::vector<rpc::Json> pendingProgramFramePreviewEvents_;
  std::vector<rpc::Json> pendingProgramSharedTextureEvents_;
  std::vector<rpc::Json> pendingParticipantSharedTextureEvents_;
  // Throttles base64 program-preview/shared-texture stdout events so they don't
  // flood the RPC channel and starve command responses (see JsonRpcServer::run).
  std::chrono::steady_clock::time_point lastFrameEventEmit_{};
};

}  // namespace corevideo::core
