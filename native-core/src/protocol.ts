export type MediaCoreRouteMode = "fixed" | "active-speaker" | "spotlight" | "screen-share" | "none";
export type MediaCoreAudioRole = "mix" | "isolated" | "audience";
export type MediaCoreDestination = "rtmp" | "ndi" | "srt" | "webrtc" | "recording";

export type MediaCoreFrameKind = "participant-video" | "screen-share";

export type MediaCoreFrameHealth = "live" | "stale" | "dropped" | "low-resolution";
export type MediaCoreRecordingStatus = "recording" | "warning" | "stopped" | "failed";
export type MediaCoreRecordingWriterStatus = "writing" | "warning" | "stopped" | "failed";
export type MediaCoreOutputHealthStatus = "idle" | "live" | "warning" | "failed";
export type MediaCoreRecordingFormat = "mp4" | "mov" | "mkv";
export type MediaCoreRecordingQuality = "standard" | "high" | "archive";

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
  outputs: MediaCoreDestination[];
  outputHealth: MediaCoreOutputHealth[];
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
      type: "start-program-output";
      destinations: MediaCoreDestination[];
      isoParticipantIds: string[];
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
  participantTransformCount: number;
  overlayCount: number;
  outputs: MediaCoreDestination[];
  isoParticipantIds: string[];
  outputHealth: MediaCoreOutputHealth[];
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
