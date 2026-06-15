import type {
  CaptureDeviceState,
  OutputDestination,
  OutputHealth,
  OutputProfile,
  OutputSessionState,
  RecordingSettings
} from "../domain/production";
import type { ZoomJoinRequest } from "./contracts";
import type { RawCaptureSnapshot } from "./captureSnapshotMapper";
import type {
  NativeMediaCoreCommand,
  NativeMediaCoreProfile,
  NativeMediaCoreStateSnapshot
} from "./nativeMediaCoreProtocol";
import type { ZoomMediaSpineNativeSnapshot } from "./zoomMediaSpineNativeSync";
import type { SupportBundle } from "../domain/production";
import type { ZoomMediaSpineSyncPayload } from "./zoomMediaSpineSync";

export type NativeZoomCommand =
  | {
      id: string;
      type: "join";
      payload: ZoomJoinRequest;
    }
  | {
      id: string;
      type: "leave";
    }
  | {
      id: string;
      type: "snapshot";
    };

export type NativeOutputCommand =
  | {
      id: string;
      type: "set-output-profile";
      payload: OutputProfile;
    }
  | {
      id: string;
      type: "start-recording";
      payload: RecordingSettings;
    }
  | {
      id: string;
      type: "stop-recording";
    }
  | {
      id: string;
      type: "start-stream";
      payload: {
        destinations: OutputDestination[];
      };
    }
  | {
      id: string;
      type: "stop-stream";
    }
  | {
      id: string;
      type: "get-output-health";
    }
  | {
      id: string;
      type: "get-output-session";
    };

export type NativeCaptureDeviceCommand =
  | {
      id: string;
      type: "list-capture-devices";
    }
  | {
      id: string;
      type: "select-capture-input";
      payload: {
        deviceId: string;
        inputId: string;
      };
    }
  | {
      id: string;
      type: "set-capture-audio-sync-offset";
      payload: {
        deviceId: string;
        offsetMs: number;
      };
    }
  | {
      id: string;
      type: "connect-capture-device";
      payload: {
        deviceId: string;
      };
    };

/**
 * Media-core RPC carried over the unified bridge. Closes the dangling seam:
 * `buildNativeMediaCoreCommands()` ships a batch of {@link NativeMediaCoreCommand}s
 * with the elapsed clock; the core announces capabilities via the handshake.
 * Track B (native core) mirrors these command shapes.
 */
export type NativeMediaCoreBridgeCommand =
  | {
      id: string;
      type: "media-core-handshake";
    }
  | {
      id: string;
      type: "media-core-sync";
      payload: {
        commands: NativeMediaCoreCommand[];
        elapsedMs: number;
      };
    };

/** Audio bus control commands. Stub-backed in the desktop IPC router for now. */
export type NativeAudioCommand =
  | {
      id: string;
      type: "get-audio-mix";
    }
  | {
      id: string;
      type: "set-audio-bus-gain";
      payload: {
        busId: string;
        gainDb: number;
      };
    }
  | {
      id: string;
      type: "set-audio-bus-mute";
      payload: {
        busId: string;
        muted: boolean;
      };
    };

/**
 * Zoom media spine sync carried over the unified bridge. Forwards the typed
 * `ZoomMediaSpineSyncPayload` to the supervised media-core child process and
 * returns a `ZoomMediaSpineNativeSnapshot`. Track B mirrors this command shape.
 */
export type NativeZoomMediaSpineBridgeCommand = {
  id: string;
  type: "zoom-media-spine-sync";
  payload: {
    spinePayload: ZoomMediaSpineSyncPayload;
    elapsedMs: number;
  };
};

/** Health query — handled at the IPC router layer, not forwarded to the native core. */
export type NativeGetMediaCoreHealthCommand = {
  id: string;
  type: "get-media-core-health";
};

/** Persist a diagnostics support bundle to the desktop user-data directory. */
export type NativeExportSupportBundleCommand = {
  id: string;
  type: "export-support-bundle";
  payload: {
    bundle: SupportBundle;
  };
};

export type ZoomOAuthSessionStatus = {
  brokerConfigured: boolean;
  signedIn: boolean;
  expiresAt: number;
  pendingAuthorization: boolean;
};

export type NativeZoomOAuthCommand =
  | { id: string; type: "get-zoom-oauth-status" }
  | { id: string; type: "zoom-oauth-begin" }
  | { id: string; type: "zoom-oauth-sign-out" };

/** Caption track commands. Stub-backed in the desktop IPC router for now. */
export type NativeCaptionCommand =
  | {
      id: string;
      type: "get-caption-track";
    }
  | {
      id: string;
      type: "set-caption-enabled";
      payload: {
        enabled: boolean;
      };
    }
  | {
      id: string;
      type: "push-caption-cue";
      payload: {
        text: string;
        speaker?: string;
        atMs: number;
      };
    };

export type NativeBridgeCommand =
  | NativeZoomCommand
  | NativeOutputCommand
  | NativeCaptureDeviceCommand
  | NativeMediaCoreBridgeCommand
  | NativeZoomMediaSpineBridgeCommand
  | NativeAudioCommand
  | NativeCaptionCommand
  | NativeGetMediaCoreHealthCommand
  | NativeExportSupportBundleCommand
  | NativeZoomOAuthCommand;

export type NativeZoomResponse =
  | {
      id: string;
      ok: true;
      snapshot: RawCaptureSnapshot;
    }
  | {
      id: string;
      ok: false;
      error: {
        code: "native-unavailable" | "join-failed" | "leave-failed" | "snapshot-failed";
        message: string;
      };
    };

export type NativeOutputResponse =
  | {
      id: string;
      ok: true;
      session: OutputSessionState;
    }
  | {
      id: string;
      ok: true;
      health: OutputHealth;
    }
  | {
      id: string;
      ok: false;
      error: {
        code:
          | "native-unavailable"
          | "output-profile-failed"
          | "recording-failed"
          | "stream-failed"
          | "output-health-failed"
          | "output-session-failed"
          | "protocol-error";
        message: string;
      };
    };

export type NativeCaptureDeviceResponse =
  | {
      id: string;
      ok: true;
      devices: CaptureDeviceState[];
    }
  | {
      id: string;
      ok: false;
      error: {
        code:
          | "native-unavailable"
          | "list-devices-failed"
          | "select-input-failed"
          | "audio-sync-failed"
          | "connect-device-failed"
          | "protocol-error";
        message: string;
      };
    };

export type NativeMediaCoreBridgeResponse =
  | {
      id: string;
      ok: true;
      profile: NativeMediaCoreProfile;
    }
  | {
      id: string;
      ok: true;
      snapshot: NativeMediaCoreStateSnapshot;
    }
  | {
      id: string;
      ok: false;
      error: {
        code: "native-unavailable" | "media-core-failed" | "media-core-unreachable" | "protocol-error";
        message: string;
      };
    };

export type NativeZoomMediaSpineResponse =
  | {
      id: string;
      ok: true;
      spineSnapshot: ZoomMediaSpineNativeSnapshot;
    }
  | {
      id: string;
      ok: false;
      error: {
        code: "native-unavailable" | "zoom-spine-failed" | "media-core-unreachable" | "protocol-error";
        message: string;
      };
    };

export type NativeMediaCoreHealthResponse =
  | {
      id: string;
      ok: true;
      health: import("./nativeMediaCoreProtocol").MediaCoreHealth;
    }
  | {
      id: string;
      ok: false;
      error: { code: "native-unavailable" | "protocol-error"; message: string };
    };

export type NativeAudioBusState = {
  busId: string;
  label: string;
  gainDb: number;
  muted: boolean;
  peakDb: number;
};

export type NativeAudioResponse =
  | {
      id: string;
      ok: true;
      buses: NativeAudioBusState[];
    }
  | {
      id: string;
      ok: false;
      error: {
        code: "native-unavailable" | "audio-failed" | "protocol-error";
        message: string;
      };
    };

export type NativeCaptionCueState = {
  id: string;
  text: string;
  speaker?: string;
  atMs: number;
};

export type NativeCaptionTrackState = {
  enabled: boolean;
  cues: NativeCaptionCueState[];
};

export type NativeCaptionResponse =
  | {
      id: string;
      ok: true;
      track: NativeCaptionTrackState;
    }
  | {
      id: string;
      ok: false;
      error: {
        code: "native-unavailable" | "caption-failed" | "protocol-error";
        message: string;
      };
    };

export type NativeZoomOAuthResponse =
  | { id: string; ok: true; oauthStatus: ZoomOAuthSessionStatus }
  | { id: string; ok: true; oauthMessage: string }
  | {
      id: string;
      ok: false;
      error: { code: "oauth-failed" | "native-unavailable" | "protocol-error"; message: string };
    };

export type NativeExportSupportBundleResponse =
  | {
      id: string;
      ok: true;
      export: {
        path: string;
        bytes: number;
      };
    }
  | {
      id: string;
      ok: false;
      error: { code: "export-failed" | "native-unavailable" | "protocol-error"; message: string };
    };

export type NativeBridgeResponse =
  | NativeZoomResponse
  | NativeOutputResponse
  | NativeCaptureDeviceResponse
  | NativeMediaCoreBridgeResponse
  | NativeZoomMediaSpineResponse
  | NativeAudioResponse
  | NativeCaptionResponse
  | NativeMediaCoreHealthResponse
  | NativeExportSupportBundleResponse
  | NativeZoomOAuthResponse;

export interface NativeZoomTransport {
  request(command: NativeBridgeCommand): Promise<NativeBridgeResponse>;
}

export class NativeZoomBridgeError extends Error {
  constructor(
    public readonly code: NativeBridgeResponse extends { ok: false; error: infer ErrorShape }
      ? ErrorShape extends { code: infer Code }
        ? Code
        : string
      : string,
    message: string
  ) {
    super(message);
    this.name = "NativeZoomBridgeError";
  }
}

let _bridgeCommandCounter = 0;

/** Allocate a unique correlation id for a bridge command. */
export function nextBridgeCommandId(prefix = "bridge"): string {
  _bridgeCommandCounter += 1;
  return `${prefix}-${_bridgeCommandCounter}`;
}

/** Build a media-core sync command carrying a batch of media-core commands. */
export function createMediaCoreSyncCommand(
  commands: NativeMediaCoreCommand[],
  elapsedMs: number,
  id: string = nextBridgeCommandId("media-core-sync")
): Extract<NativeMediaCoreBridgeCommand, { type: "media-core-sync" }> {
  return { id, type: "media-core-sync", payload: { commands, elapsedMs } };
}

/** Build a media-core handshake command that asks the core to announce its profile. */
export function createMediaCoreHandshakeCommand(
  id: string = nextBridgeCommandId("media-core-handshake")
): Extract<NativeMediaCoreBridgeCommand, { type: "media-core-handshake" }> {
  return { id, type: "media-core-handshake" };
}

/** True when a response carries a media-core state snapshot. */
export function isMediaCoreSnapshotResponse(
  response: NativeBridgeResponse
): response is Extract<NativeMediaCoreBridgeResponse, { snapshot: NativeMediaCoreStateSnapshot }> {
  return response.ok === true && "snapshot" in response;
}

/** True when a response carries a media-core profile (handshake result). */
export function isMediaCoreProfileResponse(
  response: NativeBridgeResponse
): response is Extract<NativeMediaCoreBridgeResponse, { profile: NativeMediaCoreProfile }> {
  return response.ok === true && "profile" in response;
}
