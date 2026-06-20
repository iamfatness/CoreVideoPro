export type MediaCoreRouteMode = "fixed" | "active-speaker" | "spotlight" | "screen-share" | "none";
export type MediaCoreAudioRole = "mix" | "isolated" | "audience";
export type MediaCoreDestination = "rtmp" | "ndi" | "srt" | "webrtc" | "recording";
export const coreRequestTypes = ["sync", "snapshot", "tick", "zoom-join", "zoom-leave", "zoom-snapshot", "zoom-media-spine-sync"] as const;
export const coreEventTypes = ["zoom-video-frame", "program-frame-preview", "program-shared-texture"] as const;
export const zoomMediaSpineSyncTypeNames = ["ZoomMediaSpineSyncPayload", "ZoomMediaSpineNativeSnapshot"] as const;

export type CoreVideoFrameEvent = {
  type: "zoom-video-frame";
  participantId: string;
  frameId: string;
};

export type MediaCoreFrameKind = "participant-video" | "screen-share";

export type MediaCoreFrameHealth = "live" | "stale" | "dropped" | "low-resolution";
export type MediaCoreProgramFrameHealth = "live" | "degraded" | "dropped";
export type MediaCoreCompositorHealth = "idle" | "live" | "degraded" | "failed";
export type MediaCoreMediaSourceKind = "zoom-sdk" | "local-camera" | "test-pattern";
export type MediaCoreFrameSourceStatus = "idle" | "subscribed" | "degraded" | "failed";
export type MediaCoreZoomSourceHealth = "live" | "low-resolution" | "recovering" | "video-off";
export type MediaCoreRecordingStatus = "recording" | "warning" | "stopped" | "failed";
export type MediaCoreRecordingWriterStatus = "writing" | "warning" | "stopped" | "failed";
export type MediaCoreOutputHealthStatus = "idle" | "live" | "warning" | "failed";
export type MediaCoreEncoderTargetStatus = "idle" | "attached" | "warning" | "failed";
export type MediaCoreEncoderSessionStatus = "idle" | "encoding" | "warning" | "failed";
export type MediaCoreEncoderLifecycleStatus = "idle" | "prepared" | "encoding" | "stopped" | "failed";
export type MediaCoreFrameTransportStatus = "idle" | "publishing" | "degraded";
export type MediaCoreOutputSenderStatus = "idle" | "starting" | "live" | "warning" | "stopped" | "failed";
export type MediaCoreRecordingFormat = "mp4" | "mov" | "mkv";
export type MediaCoreRecordingQuality = "standard" | "high" | "archive";
export type MediaCoreColorGradeLut = "none" | "neutral" | "warm-film" | "cool-broadcast" | "punch";
export type MediaCoreOperatorActionSeverity = "info" | "warning" | "critical";
export type MediaCoreOperatorActionArea = "source" | "routing" | "program" | "recording" | "sender" | "encoder";
export type MediaCoreEventSeverity = "info" | "warning" | "critical";
export type MediaCoreEventArea = "source" | "routing" | "program" | "recording" | "sender" | "encoder" | "system";

export type MediaCoreOutputProfile = {
  profileId: string;
  resolution: string;
  width: number;
  height: number;
  fps: number;
  targetBitrateMbps: number;
};

export type MediaCoreColorGrade = {
  lut: MediaCoreColorGradeLut;
  exposure: number;
  contrast: number;
  saturation: number;
  temperature: number;
};

export type MediaCoreZoomSource = {
  sourceId: string;
  participantId: string;
  displayName: string;
  role: string;
  breakoutRoomId: string;
  breakoutRoomName: string;
  hasVideo: boolean;
  hasAudio: boolean;
  isMuted: boolean;
  isActiveSpeaker: boolean;
  isScreenSharing: boolean;
  audioLevel: number;
  health: MediaCoreZoomSourceHealth;
};

export type MediaCoreResolvedRoute = {
  routeId: string;
  mode: MediaCoreRouteMode;
  audioRole: MediaCoreAudioRole;
  sourceId?: string;
  participantId?: string;
  kind?: MediaCoreFrameKind;
  status: "resolved" | "missing" | "disabled";
  fitMode?: "fill" | "fit" | "stretch";
  borderStyle?: "none" | "solid" | "accent" | "program" | "warning";
  borderColor?: string;
  borderThickness?: number;
  colorGrade?: MediaCoreColorGrade;
  warning?: string;
};

export type MediaCoreRenderPlanLayer = {
  layerId: string;
  kind: MediaCoreFrameKind | "overlay";
  sourceId?: string;
  participantId?: string;
  overlayId?: string;
  order: number;
  routeId?: string;
  fitMode?: "fill" | "fit" | "stretch";
  borderStyle?: "none" | "solid" | "accent" | "program" | "warning";
  borderColor?: string;
  borderThickness?: number;
  colorGrade?: MediaCoreColorGrade;
  position?: "top-right" | "bottom-right" | "center" | "lower-third";
};

export type MediaCoreRenderPlan = {
  renderPlanId: string;
  sceneId?: string;
  outputProfile: MediaCoreOutputProfile;
  colorGrade: MediaCoreColorGrade;
  sourceCount: number;
  resolvedRouteCount: number;
  layers: MediaCoreRenderPlanLayer[];
  routes: MediaCoreResolvedRoute[];
  warnings: string[];
};

export type MediaCoreProgramFrame = {
  frameNumber: number;
  timestampMs: number;
  renderPlanId: string;
  sceneId?: string;
  width: number;
  height: number;
  fps: number;
  layerCount: number;
  colorGrade: MediaCoreColorGrade;
  health: MediaCoreProgramFrameHealth;
  warning?: string;
};

export type MediaCoreFrameSourceSnapshot = {
  adapterId: string;
  kind: MediaCoreMediaSourceKind;
  status: MediaCoreFrameSourceStatus;
  subscribedSourceCount: number;
  liveFrameCount: number;
  staleFrameCount: number;
  droppedFrameCount: number;
  lowResolutionFrameCount: number;
  lastFrameTimestampMs?: number;
  issues?: MediaCoreSourceHealthIssue[];
  warnings: string[];
};

export type MediaCoreSourceHealthIssue = {
  sourceId: string;
  participantId?: string;
  displayName?: string;
  health: MediaCoreZoomSourceHealth;
  severity: MediaCoreOperatorActionSeverity;
  detail: string;
};

export type MediaCoreCompositorState = {
  status: MediaCoreCompositorHealth;
  renderPlanId?: string;
  programFrameCount: number;
  droppedFrameCount: number;
  degradedFrameCount: number;
  lastReconfigureReason?: string;
  lastFrame?: MediaCoreProgramFrame;
};

export type MediaCoreProgramFrameTransport = {
  transportId: "in-process-preview";
  status: MediaCoreFrameTransportStatus;
  frameNumber?: number;
  renderPlanId?: string;
  timestampMs?: number;
  latencyMs: number;
  warning?: string;
};

export type MediaCoreEncoderTarget = {
  targetId: string;
  destination: MediaCoreDestination;
  streamKind: "program" | "iso";
  participantId?: string;
  status: MediaCoreEncoderTargetStatus;
  attachedFrameCount: number;
  warning?: string;
};

export type MediaCoreEncoderLifecycle = {
  status: MediaCoreEncoderLifecycleStatus;
  preparedAtMs?: number;
  startedAtMs?: number;
  stoppedAtMs?: number;
  lastTransition: string;
};

export type MediaCoreEncoderSession = {
  status: MediaCoreEncoderSessionStatus;
  renderPlanId?: string;
  programFrameCount: number;
  targets: MediaCoreEncoderTarget[];
  lifecycle: MediaCoreEncoderLifecycle;
  warnings: string[];
};

export type MediaCoreFrame = {
  sourceId: string;
  participantId?: string;
  kind: MediaCoreFrameKind;
  frameNumber: number;
  timestampMs: number;
  width: number;
  height: number;
  fps: number;
  health: MediaCoreFrameHealth;
};

export type MediaCoreRecordingStream = {
  kind: "program" | "iso";
  participantId?: string;
  path: string;
  status: MediaCoreRecordingWriterStatus;
  readiness?: "ready" | "missing" | "video-off" | "unsubscribable";
  expectedFrames?: number;
  framesWritten: number;
  missingFrames?: number;
  droppedFrames: number;
  bytesWritten: number;
  lastFrameTimestampMs?: number;
  warning?: string;
};

export type MediaCoreRecordingSession = {
  sessionId: string;
  active: boolean;
  status: MediaCoreRecordingStatus;
  writerStatus: MediaCoreRecordingWriterStatus;
  startedAtMs: number;
  stoppedAtMs?: number;
  elapsedMs: number;
  targetFolder: string;
  filenamePrefix: string;
  format: MediaCoreRecordingFormat;
  quality: MediaCoreRecordingQuality;
  encoder: {
    codec: "h264" | "hevc";
    hardwareAccelerated: boolean;
    targetBitrateMbps: number;
  };
  estimatedDiskRateMBps: number;
  programPath: string;
  streams: MediaCoreRecordingStream[];
  totalFramesWritten: number;
  totalDroppedFrames: number;
  totalBytesWritten: number;
  warning?: string;
  error?: string;
};

export type MediaCoreRecordingTargets = {
  targetFolder: string;
  filenamePrefix: string;
  format: MediaCoreRecordingFormat;
  quality: MediaCoreRecordingQuality;
  isoParticipantIds: string[];
};

export type ZoomMediaSpineReadinessReportPayload = {
  status: "ready" | "warning" | "blocked";
  platform: "macos" | "windows";
  sdkVersion: string;
  checks: Array<{ id: string; status: "ready" | "warning" | "blocked"; label: string; detail: string }>;
  blockers: string[];
  warnings: string[];
  summary: string;
};

export type ZoomMediaSpineParticipantPayload = {
  sdkUserId: string;
  displayName: string;
  role: "host" | "cohost" | "panelist" | "attendee" | "guest";
  videoOn: boolean;
  muted: boolean;
  talking: boolean;
  sharingScreen: boolean;
  audioLevel: number;
  networkQuality: "good" | "low" | "recovering";
};

export type ZoomMediaSpineSubscriptionRequestPayload = {
  participantId: string;
  kind: "participant-video" | "participant-audio" | "screen-share";
  purpose: "program" | "iso" | "active-speaker" | "screen-share" | "mix";
  priority: number;
};

export type ZoomMediaSpineSyncPayload = {
  readiness: ZoomMediaSpineReadinessReportPayload;
  participants: ZoomMediaSpineParticipantPayload[];
  subscriptions: ZoomMediaSpineSubscriptionRequestPayload[];
  recording?: MediaCoreRecordingTargets;
  blocked: boolean;
  warnings: string[];
  summary: string;
};

export type MediaCoreOutputHealth = {
  destination: MediaCoreDestination;
  status: MediaCoreOutputHealthStatus;
  message: string;
  droppedFrames: number;
};

export type MediaCoreOutputSender = {
  senderId: string;
  destination: Exclude<MediaCoreDestination, "recording">;
  status: MediaCoreOutputSenderStatus;
  startedAtMs?: number;
  stoppedAtMs?: number;
  lastFrameNumber?: number;
  framesSent: number;
  retryCount: number;
  latencyMs: number;
  bitrateMbps: number;
  warning?: string;
};

export type MediaCoreOutputSenderSession = {
  status: "idle" | "live" | "warning" | "failed";
  activeSenderCount: number;
  senders: MediaCoreOutputSender[];
  warnings: string[];
};

export type MediaCoreOperatorAction = {
  actionId: string;
  severity: MediaCoreOperatorActionSeverity;
  area: MediaCoreOperatorActionArea;
  title: string;
  detail: string;
  command?: string;
  relatedId?: string;
};

export type MediaCoreEvent = {
  eventId: string;
  atMs: number;
  severity: MediaCoreEventSeverity;
  area: MediaCoreEventArea;
  title: string;
  detail: string;
  relatedId?: string;
  commandType?: MediaCoreCommand["type"];
};

export type MediaCoreCaptureAudioSource = {
  captureDeviceId: string;
  audioDeviceId?: string | null;
  audioDeviceName?: string | null;
  audioSyncOffsetMs: number;
  paired: boolean;
};

export type MediaCoreCaptureAudioSources = {
  status: "idle" | "ready";
  sourceCount: number;
  pairedCount: number;
  sources: MediaCoreCaptureAudioSource[];
  summary: string;
};

// Capture-device snapshot mirror (F1 — real frame-pixel transport). Mirrors the
// JSON emitted by MediaCore::captureDeviceJson (native/src/core/Protocol.h
// kCaptureDeviceSnapshotFields) byte-for-byte. The F1 additions are the real
// per-source counters `framesIngested` (populated-pixel frames published) and
// `droppedFrames` (frame-pool back-pressure).
export type MediaCoreCaptureDeviceInput = {
  id: string;
  label: string;
  hasEmbeddedAudio: boolean;
};

export type MediaCoreCaptureDeviceSnapshot = {
  id: string;
  vendor: string;
  name: string;
  inputs: MediaCoreCaptureDeviceInput[];
  selectedInputId: string;
  resolution: { width: number; height: number };
  frameRate: number;
  connectionState: string;
  signalPresent: boolean;
  droppedFrames: number;
  framesIngested: number;
  audioSyncOffsetMs: number;
  warning?: string;
};

export type MediaCoreDiagnosticsSnapshot = {
  generatedAtMs: number;
  sceneId?: string;
  routeCount: number;
  frameCount: number;
  programFrameCount: number;
  outputs: MediaCoreDestination[];
  outputProfile: MediaCoreOutputProfile;
  outputHealth: MediaCoreOutputHealth[];
  outputSenderSession: MediaCoreOutputSenderSession;
  sourceSnapshot: MediaCoreFrameSourceSnapshot;
  renderPlan: MediaCoreRenderPlan;
  compositor: MediaCoreCompositorState;
  programFrame?: MediaCoreProgramFrame;
  programTransport: MediaCoreProgramFrameTransport;
  encoderSession: MediaCoreEncoderSession;
  recording?: MediaCoreRecordingSession;
  audioMixSession: MediaCoreAudioMixSession;
  audioRoutingMatrix: MediaCoreAudioRoutingMatrix;
  captureAudioSources: MediaCoreCaptureAudioSources;
  captionTrack: MediaCoreCaptionTrack;
  brandKit: MediaCoreBrandKit;
  mediaPlayback: MediaCoreMediaPlayback;
  operatorActions: MediaCoreOperatorAction[];
  eventLog: MediaCoreEvent[];
  warnings: string[];
  lastCommandTypes: string[];
};

export type MediaCoreCommand =
  | {
      type: "load-scene-graph";
      sceneId: string;
      routes: Array<{
        routeId: string;
        mode: MediaCoreRouteMode;
        participantId?: string;
        audioRole: MediaCoreAudioRole;
        fitMode?: "fill" | "fit" | "stretch";
        borderStyle?: "none" | "solid" | "accent" | "program" | "warning";
        borderColor?: string;
        borderThickness?: number;
        colorGrade?: MediaCoreColorGrade;
      }>;
    }
  | {
      type: "set-participant-transform";
      participantId: string;
      crop: { x: number; y: number; width: number; height: number };
      scale: number;
      chromaKey?: { enabled: boolean; color: "green" | "blue"; spillSuppression: number };
    }
  | {
      type: "set-overlay-asset";
      overlayId: string;
      text?: string;
      imageUri?: string;
      position: "top-right" | "bottom-right" | "center" | "lower-third";
      enabled?: boolean;
      sourceId?: string;
      sourceName?: string;
      title?: string;
      org?: string;
      keyPosition?: "lower-left" | "upper-left";
      keyPhase?: "hidden" | "building-in" | "on-air" | "building-out";
      keyer?: "upstream" | "downstream";
    }
  | {
      type: "set-zoom-source-roster";
      sources: MediaCoreZoomSource[];
    }
  | {
      type: "set-media-source-adapter";
      kind: MediaCoreMediaSourceKind;
      adapterId?: string;
    }
  | {
      type: "set-active-speaker";
      participantId?: string;
    }
  | {
      type: "set-screen-share-source";
      participantId?: string;
    }
  | ({
      type: "set-color-grade";
    } & MediaCoreColorGrade)
  | ({
      type: "set-output-profile";
    } & MediaCoreOutputProfile)
  | {
      type: "start-program-output";
      destinations: MediaCoreDestination[];
      isoParticipantIds: string[];
    }
  | {
      type: "prepare-encoder-session";
      preparedAtMs?: number;
      reason?: string;
    }
  | {
      type: "start-encoder-session";
      startedAtMs?: number;
    }
  | {
      type: "stop-encoder-session";
      stoppedAtMs?: number;
      reason?: string;
    }
  | {
      type: "fail-output-sender";
      destination: Exclude<MediaCoreDestination, "recording">;
      message: string;
      failedAtMs?: number;
    }
  | {
      type: "recover-output-sender";
      destination: Exclude<MediaCoreDestination, "recording">;
      recoveredAtMs?: number;
      reason?: string;
    }
  | ({
      type: "set-recording-targets";
    } & MediaCoreRecordingTargets)
  | ({
      type: "start-recording-session";
      sessionId?: string;
      startedAtMs?: number;
    } & Partial<MediaCoreRecordingTargets>)
  | {
      type: "stop-recording-session";
      reason?: string;
    }
  | {
      type: "fail-recording-session";
      message: string;
    }
  | {
      type: "recover-recording-session";
      recoveredAtMs?: number;
      reason?: string;
    }
  | {
      type: "sync-participant-audio-mix";
      limiterEnabled?: boolean;
      channels: Array<{
        participantId: string;
        inputLevel: number;
        muted: boolean;
        noiseSuppression: boolean;
        manualGainDb?: number;
        pan?: number;
        solo?: boolean;
        pluginInserts?: string[];
      }>;
    }
  | {
      type: "sync-audio-routing-matrix";
      sends: MediaCoreAudioRoutingSend[];
    }
  | {
      type: "sync-capture-audio-sources";
      sources: Array<{
        captureDeviceId: string;
        audioDeviceId?: string | null;
        audioDeviceName?: string | null;
        audioSyncOffsetMs?: number;
      }>;
    }
  | {
      type: "push-caption-cue";
      text: string;
      speaker?: string;
      atMs: number;
    }
  | {
      type: "set-caption-enabled";
      enabled: boolean;
    }
  | {
      type: "set-brand-kit";
      name: string;
      logoText: string;
      brandColor: string;
      accentColor: string;
      backgroundColor: string;
      fontFamily: "Inter" | "Poppins" | "Roboto" | "Georgia";
      lowerThirdStyle: "solid" | "minimal" | "gradient";
      captionStyle: string;
      defaultOverlayBehavior: string;
    }
  | {
      type: "set-media-playback";
      mediaAssetId: string;
      mediaAssetName: string;
      playing: boolean;
    };

export type MediaCoreMediaPlaybackStatus = "idle" | "playing" | "paused";

export type MediaCoreMediaPlayback = {
  status: MediaCoreMediaPlaybackStatus;
  mediaAssetId?: string;
  mediaAssetName?: string;
  playing: boolean;
  summary: string;
  warnings: string[];
};

export type MediaCoreBrandKit = {
  name: string;
  logoText: string;
  brandColor: string;
  accentColor: string;
  backgroundColor: string;
  fontFamily: "Inter" | "Poppins" | "Roboto" | "Georgia";
  lowerThirdStyle: "solid" | "minimal" | "gradient";
  captionStyle: string;
  defaultOverlayBehavior: string;
  appliedOverlayCount: number;
  summary: string;
  warnings: string[];
};

export type MediaCoreParticipantAudioChannel = {
  participantId: string;
  inputLevel: number;
  outputLevel: number;
  gainDb: number;
  manualGainDb?: number;
  pan?: number;
  solo?: boolean;
  pluginInserts?: Array<{
    name: string;
    format: "builtin" | "vst3" | "vst2";
    status: "available" | "scan-only" | "failed";
    processingEnabled: boolean;
  }>;
  noiseSuppression: boolean;
  limiterActive: boolean;
  muted: boolean;
  status: "balanced" | "boosting" | "ducking" | "muted";
};

export type MediaCoreAudioMixSession = {
  status: "idle" | "live" | "warning";
  masterLevel: number;
  loudnessLufs: number;
  limiterEnabled: boolean;
  limiterActive: boolean;
  mixedFrameCount: number;
  participants: MediaCoreParticipantAudioChannel[];
  summary: string;
  warnings: string[];
};

export type MediaCoreAudioRoutingBus =
  | "master"
  | "pgm-l"
  | "pgm-r"
  | "iso-1"
  | "iso-2"
  | "iso-3"
  | "iso-4"
  | "iso-5"
  | "iso-6"
  | "iso-7"
  | "iso-8"
  | "mon"
  | "stream"
  | "aux-1"
  | "aux-2"
  | `bus-${string}`;

export type MediaCoreAudioRoutingSend = {
  sourceId: string;
  busId: MediaCoreAudioRoutingBus;
  gainDb: number;
};

export type MediaCoreAudioRoutingBusSummary = {
  busId: MediaCoreAudioRoutingBus;
  sourceCount: number;
};

export type MediaCoreAudioRoutingMatrix = {
  status: "idle" | "live" | "warning";
  routedSendCount: number;
  routedSourceCount: number;
  busSourceCounts: MediaCoreAudioRoutingBusSummary[];
  sends: MediaCoreAudioRoutingSend[];
  summary: string;
  warnings: string[];
};

export type MediaCoreCaptionCue = {
  text: string;
  speaker?: string;
  atMs: number;
  confidence: number;
};

export type MediaCoreCaptionTrack = {
  enabled: boolean;
  status: "idle" | "live" | "warning";
  currentCue?: MediaCoreCaptionCue;
  latencyMs: number;
  warnings: string[];
};

export type MediaCoreRequest =
  | {
      id: string;
      type: "sync";
      commands: MediaCoreCommand[];
    }
  | {
      id: string;
      type: "snapshot";
    }
  | {
      id: string;
      type: "tick";
      elapsedMs: number;
    }
  | {
      id: string;
      type: "zoom-media-spine-sync";
      payload: ZoomMediaSpineSyncPayload;
      elapsedMs: number;
    };

export type MediaCoreStateSnapshot = {
  sceneId?: string;
  routeCount: number;
  frameCount: number;
  frames: MediaCoreFrame[];
  sourceSnapshot: MediaCoreFrameSourceSnapshot;
  programFrame?: MediaCoreProgramFrame;
  programFrameCount: number;
  programTransport: MediaCoreProgramFrameTransport;
  compositor: MediaCoreCompositorState;
  participantTransformCount: number;
  overlayCount: number;
  outputs: MediaCoreDestination[];
  isoParticipantIds: string[];
  outputProfile: MediaCoreOutputProfile;
  outputHealth: MediaCoreOutputHealth[];
  outputSenderSession: MediaCoreOutputSenderSession;
  sourceCount: number;
  resolvedRouteCount: number;
  renderPlan: MediaCoreRenderPlan;
  encoderSession: MediaCoreEncoderSession;
  recording?: MediaCoreRecordingSession;
  audioMixSession: MediaCoreAudioMixSession;
  audioRoutingMatrix: MediaCoreAudioRoutingMatrix;
  captureAudioSources: MediaCoreCaptureAudioSources;
  captionTrack: MediaCoreCaptionTrack;
  brandKit: MediaCoreBrandKit;
  mediaPlayback: MediaCoreMediaPlayback;
  operatorActions: MediaCoreOperatorAction[];
  eventLog: MediaCoreEvent[];
  diagnostics: MediaCoreDiagnosticsSnapshot;
  lastCommandTypes: string[];
  warnings: string[];
};

export type MediaCoreResponse =
  | {
      id: string;
      ok: true;
      appliedCommandCount: number;
      state: MediaCoreStateSnapshot;
    }
  | {
      id: string;
      ok: true;
      state: MediaCoreStateSnapshot;
    }
  | {
      id: string;
      ok: false;
      error: {
        code: "invalid-request" | "unsupported-command";
        message: string;
      };
    };
