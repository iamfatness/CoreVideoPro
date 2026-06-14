export type NativeMediaCoreCapability =
  | "zoom-raw-video"
  | "zoom-raw-audio"
  | "gpu-compositor"
  | "scene-graph-rendering"
  | "dynamic-overlays"
  | "chroma-key"
  | "smart-framing"
  | "audio-mixer"
  | "program-recording"
  | "iso-recording"
  | "rtmp-output"
  | "ndi-output"
  | "srt-output"
  | "webrtc-output";

export type NativeMediaCoreProfile = {
  name: string;
  renderer: "metal" | "direct3d11" | "direct3d12" | "vulkan" | "opengl" | "software";
  maxProgramResolution: "1920x1080" | "3840x2160";
  maxProgramFps: 30 | 60;
  maxParticipantFeeds: number;
  maxIsoRecordings: number;
  capabilities: NativeMediaCoreCapability[];
};

export type NativeMediaCoreCommand =
  | {
      type: "load-scene-graph";
      sceneId: string;
      routes: Array<{
        routeId: string;
        mode: "fixed" | "active-speaker" | "spotlight" | "screen-share" | "none";
        participantId?: string;
        audioRole: "mix" | "isolated" | "audience";
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
      sources: NativeMediaCoreZoomSource[];
    }
  | {
      type: "set-media-source-adapter";
      kind: NativeMediaCoreMediaSourceKind;
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
    } & NativeMediaCoreColorGrade)
  | ({
      type: "set-output-profile";
    } & NativeMediaCoreOutputProfile)
  | {
      type: "start-program-output";
      destinations: Array<"rtmp" | "ndi" | "srt" | "webrtc" | "recording">;
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
      destination: "rtmp" | "ndi" | "srt" | "webrtc";
      message: string;
      failedAtMs?: number;
    }
  | {
      type: "recover-output-sender";
      destination: "rtmp" | "ndi" | "srt" | "webrtc";
      recoveredAtMs?: number;
      reason?: string;
    }
  | ({
      type: "set-recording-targets";
    } & NativeMediaCoreRecordingTargets)
  | ({
      type: "start-recording-session";
      sessionId?: string;
      startedAtMs?: number;
    } & Partial<NativeMediaCoreRecordingTargets>)
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
    };

export type NativeMediaCoreFrame = {
  sourceId: string;
  participantId?: string;
  kind: "participant-video" | "screen-share";
  frameNumber: number;
  timestampMs: number;
  width: number;
  height: number;
  fps: number;
  health: "live" | "stale" | "dropped" | "low-resolution";
};

export type NativeMediaCoreMediaSourceKind = "zoom-sdk" | "local-camera" | "test-pattern";

export type NativeMediaCoreFrameSourceSnapshot = {
  adapterId: string;
  kind: NativeMediaCoreMediaSourceKind;
  status: "idle" | "subscribed" | "degraded" | "failed";
  subscribedSourceCount: number;
  liveFrameCount: number;
  staleFrameCount: number;
  droppedFrameCount: number;
  lowResolutionFrameCount: number;
  lastFrameTimestampMs?: number;
  warnings: string[];
};

export type NativeMediaCoreProgramFrame = {
  frameNumber: number;
  timestampMs: number;
  renderPlanId: string;
  sceneId?: string;
  width: number;
  height: number;
  fps: number;
  layerCount: number;
  colorGrade: NativeMediaCoreColorGrade;
  health: "live" | "degraded" | "dropped";
  warning?: string;
};

export type NativeMediaCoreProgramFrameTransport = {
  transportId: "in-process-preview";
  status: "idle" | "publishing" | "degraded";
  frameNumber?: number;
  renderPlanId?: string;
  timestampMs?: number;
  latencyMs: number;
  warning?: string;
};

export type NativeMediaCoreCompositorState = {
  status: "idle" | "live" | "degraded" | "failed";
  renderPlanId?: string;
  programFrameCount: number;
  droppedFrameCount: number;
  degradedFrameCount: number;
  lastReconfigureReason?: string;
  lastFrame?: NativeMediaCoreProgramFrame;
};

export type NativeMediaCoreZoomSource = {
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
  health: "live" | "low-resolution" | "recovering" | "video-off";
};

export type NativeMediaCoreColorGrade = {
  lut: "none" | "neutral" | "warm-film" | "cool-broadcast" | "punch";
  exposure: number;
  contrast: number;
  saturation: number;
  temperature: number;
};

export type NativeMediaCoreResolvedRoute = {
  routeId: string;
  mode: "fixed" | "active-speaker" | "spotlight" | "screen-share" | "none";
  audioRole: "mix" | "isolated" | "audience";
  sourceId?: string;
  participantId?: string;
  kind?: "participant-video" | "screen-share";
  status: "resolved" | "missing" | "disabled";
  warning?: string;
};

export type NativeMediaCoreRenderPlanLayer = {
  layerId: string;
  kind: "participant-video" | "screen-share" | "overlay";
  sourceId?: string;
  participantId?: string;
  overlayId?: string;
  order: number;
  routeId?: string;
  position?: "top-right" | "bottom-right" | "center" | "lower-third";
};

export type NativeMediaCoreRecordingStream = {
  kind: "program" | "iso";
  participantId?: string;
  path: string;
  status: "writing" | "warning" | "stopped" | "failed";
  framesWritten: number;
  droppedFrames: number;
  bytesWritten: number;
};

export type NativeMediaCoreRecordingSession = {
  sessionId: string;
  active: boolean;
  status: "recording" | "warning" | "stopped" | "failed";
  writerStatus: "writing" | "warning" | "stopped" | "failed";
  startedAtMs: number;
  stoppedAtMs?: number;
  elapsedMs: number;
  targetFolder: string;
  filenamePrefix: string;
  format: "mp4" | "mov" | "mkv";
  quality: "standard" | "high" | "archive";
  encoder: {
    codec: "h264" | "hevc";
    hardwareAccelerated: boolean;
    targetBitrateMbps: number;
  };
  estimatedDiskRateMBps: number;
  programPath: string;
  streams: NativeMediaCoreRecordingStream[];
  totalFramesWritten: number;
  totalDroppedFrames: number;
  totalBytesWritten: number;
  warning?: string;
  error?: string;
};

export type NativeMediaCoreRecordingTargets = {
  targetFolder: string;
  filenamePrefix: string;
  format: "mp4" | "mov" | "mkv";
  quality: "standard" | "high" | "archive";
  isoParticipantIds: string[];
};

export type NativeMediaCoreOutputProfile = {
  profileId: string;
  resolution: string;
  width: number;
  height: number;
  fps: number;
  targetBitrateMbps: number;
};

export type NativeMediaCoreRenderPlan = {
  renderPlanId: string;
  sceneId?: string;
  outputProfile: NativeMediaCoreOutputProfile;
  colorGrade: NativeMediaCoreColorGrade;
  sourceCount: number;
  resolvedRouteCount: number;
  layers: NativeMediaCoreRenderPlanLayer[];
  routes: NativeMediaCoreResolvedRoute[];
  warnings: string[];
};

export type NativeMediaCoreEncoderTarget = {
  targetId: string;
  destination: "rtmp" | "ndi" | "srt" | "webrtc" | "recording";
  streamKind: "program" | "iso";
  participantId?: string;
  status: "idle" | "attached" | "warning" | "failed";
  attachedFrameCount: number;
  warning?: string;
};

export type NativeMediaCoreEncoderLifecycle = {
  status: "idle" | "prepared" | "encoding" | "stopped" | "failed";
  preparedAtMs?: number;
  startedAtMs?: number;
  stoppedAtMs?: number;
  lastTransition: string;
};

export type NativeMediaCoreEncoderSession = {
  status: "idle" | "encoding" | "warning" | "failed";
  renderPlanId?: string;
  programFrameCount: number;
  targets: NativeMediaCoreEncoderTarget[];
  lifecycle: NativeMediaCoreEncoderLifecycle;
  warnings: string[];
};

export type NativeMediaCoreOutputHealth = {
  destination: "rtmp" | "ndi" | "srt" | "webrtc" | "recording";
  status: "idle" | "live" | "warning" | "failed";
  message: string;
  droppedFrames: number;
};

export type NativeMediaCoreOutputSender = {
  senderId: string;
  destination: "rtmp" | "ndi" | "srt" | "webrtc";
  status: "idle" | "starting" | "live" | "warning" | "stopped" | "failed";
  startedAtMs?: number;
  stoppedAtMs?: number;
  lastFrameNumber?: number;
  framesSent: number;
  retryCount: number;
  latencyMs: number;
  bitrateMbps: number;
  warning?: string;
};

export type NativeMediaCoreOutputSenderSession = {
  status: "idle" | "live" | "warning" | "failed";
  activeSenderCount: number;
  senders: NativeMediaCoreOutputSender[];
  warnings: string[];
};

export type NativeMediaCoreDiagnosticsSnapshot = {
  generatedAtMs: number;
  sceneId?: string;
  routeCount: number;
  frameCount: number;
  programFrameCount: number;
  outputs: Array<"rtmp" | "ndi" | "srt" | "webrtc" | "recording">;
  outputProfile: NativeMediaCoreOutputProfile;
  outputHealth: NativeMediaCoreOutputHealth[];
  outputSenderSession: NativeMediaCoreOutputSenderSession;
  sourceSnapshot: NativeMediaCoreFrameSourceSnapshot;
  renderPlan: NativeMediaCoreRenderPlan;
  compositor: NativeMediaCoreCompositorState;
  programFrame?: NativeMediaCoreProgramFrame;
  programTransport: NativeMediaCoreProgramFrameTransport;
  encoderSession: NativeMediaCoreEncoderSession;
  recording?: NativeMediaCoreRecordingSession;
  warnings: string[];
  lastCommandTypes: string[];
};

export type NativeMediaCoreStateSnapshot = {
  sceneId?: string;
  routeCount: number;
  frameCount: number;
  frames: NativeMediaCoreFrame[];
  sourceSnapshot: NativeMediaCoreFrameSourceSnapshot;
  programFrame?: NativeMediaCoreProgramFrame;
  programFrameCount: number;
  programTransport: NativeMediaCoreProgramFrameTransport;
  compositor: NativeMediaCoreCompositorState;
  participantTransformCount: number;
  overlayCount: number;
  outputs: Array<"rtmp" | "ndi" | "srt" | "webrtc" | "recording">;
  isoParticipantIds: string[];
  outputProfile: NativeMediaCoreOutputProfile;
  outputHealth: NativeMediaCoreOutputHealth[];
  outputSenderSession: NativeMediaCoreOutputSenderSession;
  sourceCount: number;
  resolvedRouteCount: number;
  renderPlan: NativeMediaCoreRenderPlan;
  encoderSession: NativeMediaCoreEncoderSession;
  recording?: NativeMediaCoreRecordingSession;
  diagnostics: NativeMediaCoreDiagnosticsSnapshot;
  lastCommandTypes: string[];
  warnings: string[];
};

export type NativeMediaCoreValidation = {
  ready: boolean;
  missingCapabilities: NativeMediaCoreCapability[];
  warnings: string[];
};

export const requiredMvpMediaCoreCapabilities: NativeMediaCoreCapability[] = [
  "zoom-raw-video",
  "zoom-raw-audio",
  "gpu-compositor",
  "scene-graph-rendering",
  "dynamic-overlays",
  "chroma-key",
  "smart-framing",
  "audio-mixer",
  "program-recording",
  "iso-recording",
  "rtmp-output"
];

export function validateNativeMediaCoreProfile(profile: NativeMediaCoreProfile): NativeMediaCoreValidation {
  const missingCapabilities = requiredMvpMediaCoreCapabilities.filter((capability) => !profile.capabilities.includes(capability));
  const warnings: string[] = [];

  if (profile.renderer === "software") {
    warnings.push("Software rendering is not suitable for production 1080p/4K switching.");
  }

  if (profile.maxParticipantFeeds < 6) {
    warnings.push("MVP target expects at least 6 clean Zoom participant feeds.");
  }

  if (profile.maxIsoRecordings < 2) {
    warnings.push("MVP target expects program recording plus selected ISO recovery paths.");
  }

  if (profile.maxProgramResolution !== "3840x2160") {
    warnings.push("4K output will be unavailable on this media core profile.");
  }

  return {
    ready: missingCapabilities.length === 0 && profile.renderer !== "software" && profile.maxParticipantFeeds >= 6,
    missingCapabilities,
    warnings
  };
}
