import type { OutputDestination, OutputHealth, OutputProfile, OutputSessionState, RecordingSettings } from "../domain/production";
import type { ZoomJoinRequest } from "./contracts";
import type { RawCaptureSnapshot } from "./captureSnapshotMapper";

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

export type NativeBridgeCommand = NativeZoomCommand | NativeOutputCommand;

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

export type NativeBridgeResponse = NativeZoomResponse | NativeOutputResponse;

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
