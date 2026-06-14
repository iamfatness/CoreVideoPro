export type MediaCoreRouteMode = "fixed" | "active-speaker" | "spotlight" | "screen-share" | "none";
export type MediaCoreAudioRole = "mix" | "isolated" | "audience";
export type MediaCoreDestination = "rtmp" | "ndi" | "srt" | "webrtc" | "recording";

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
export type MediaCoreRecordingFormat = "mp4" | "mov" | "mkv";
export type MediaCoreRecordingQuality = "standard" | "high" | "archive";
export type MediaCoreColorGradeLut = "none" | "neutral" | "warm-film" | "cool-broadcast" | "punch";

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
  warnings: string[];
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
  framesWritten: number;
  droppedFrames: number;
  bytesWritten: number;
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

export type MediaCoreOutputHealth = {
  destination: MediaCoreDestination;
  status: MediaCoreOutputHealthStatus;
  message: string;
  droppedFrames: number;
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
  sourceSnapshot: MediaCoreFrameSourceSnapshot;
  renderPlan: MediaCoreRenderPlan;
  compositor: MediaCoreCompositorState;
  programFrame?: MediaCoreProgramFrame;
  programTransport: MediaCoreProgramFrameTransport;
  encoderSession: MediaCoreEncoderSession;
  recording?: MediaCoreRecordingSession;
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
  sourceCount: number;
  resolvedRouteCount: number;
  renderPlan: MediaCoreRenderPlan;
  encoderSession: MediaCoreEncoderSession;
  recording?: MediaCoreRecordingSession;
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
