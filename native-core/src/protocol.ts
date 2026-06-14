export type MediaCoreRouteMode = "fixed" | "active-speaker" | "spotlight" | "screen-share" | "none";
export type MediaCoreAudioRole = "mix" | "isolated" | "audience";
export type MediaCoreDestination = "rtmp" | "ndi" | "srt" | "webrtc" | "recording";

export type MediaCoreFrameKind = "participant-video" | "screen-share";

export type MediaCoreFrameHealth = "live" | "stale" | "dropped" | "low-resolution";

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
  framesWritten: number;
};

export type MediaCoreRecordingSession = {
  active: boolean;
  status: "recording" | "warning";
  startedAtMs: number;
  elapsedMs: number;
  estimatedDiskRateMBps: number;
  programPath: string;
  streams: MediaCoreRecordingStream[];
  totalFramesWritten: number;
  warning?: string;
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
  recording?: MediaCoreRecordingSession;
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
