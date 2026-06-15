/**
 * Stdio JSON-RPC wire protocol between the desktop supervisor
 * (`mediaCoreClient.ts`) and the spawned media-core process (`coreStub.ts`,
 * later replaced by Track B's native binary). One JSON object per line.
 *
 * This is intentionally distinct from the renderer-facing `nativeBridgeProtocol`:
 * the IPC router translates bridge commands into these core requests.
 */
import type { CaptureDeviceState } from "../src/domain/production";
import type { NativeMediaCoreCommand, NativeMediaCoreProfile, NativeMediaCoreStateSnapshot } from "../src/engine/nativeMediaCoreProtocol";
import type { RawCaptureSnapshot } from "../src/engine/captureSnapshotMapper";
import type { ZoomJoinRequest } from "../src/engine/contracts";
import type { ZoomMediaSpineNativeSnapshot } from "../src/engine/zoomMediaSpineNativeSync";
import type { ZoomMediaSpineSyncPayload } from "../src/engine/zoomMediaSpineSync";
import type { ZoomVideoFrame } from "../src/engine/zoomVideoFrames";

export type CoreRequest =
  | { id: string; type: "handshake" }
  | { id: string; type: "ping" }
  | { id: string; type: "media-core-sync"; commands: NativeMediaCoreCommand[]; elapsedMs: number }
  | { id: string; type: "zoom-join"; payload: ZoomJoinRequest }
  | { id: string; type: "zoom-leave" }
  | { id: string; type: "zoom-snapshot" }
  /** Zoom media spine sync: forward the typed payload to the core. Track B mirrors this. */
  | { id: string; type: "zoom-media-spine-sync"; spinePayload: ZoomMediaSpineSyncPayload; elapsedMs: number }
  /** Test-only hook: makes the stub exit non-zero to exercise crash isolation. */
  | { id: string; type: "__crash" }
  /** Capture-device bridge commands forwarded to the media core (mirrors nativeBridgeProtocol). */
  | { id: string; type: "list-capture-devices" }
  | { id: string; type: "select-capture-input"; payload: { deviceId: string; inputId: string } }
  | { id: string; type: "set-capture-audio-sync-offset"; payload: { deviceId: string; offsetMs: number } }
  | { id: string; type: "connect-capture-device"; payload: { deviceId: string } };

export type CoreCaptureBridgeRequest = Extract<
  CoreRequest,
  { type: "list-capture-devices" | "select-capture-input" | "set-capture-audio-sync-offset" | "connect-capture-device" }
>;

export type CoreResponse =
  | { id: string; ok: true; type: "handshake"; profile: NativeMediaCoreProfile }
  | { id: string; ok: true; type: "ping" }
  | { id: string; ok: true; type: "media-core-sync"; snapshot: NativeMediaCoreStateSnapshot }
  | { id: string; ok: true; type: "zoom-join" | "zoom-leave" | "zoom-snapshot"; snapshot: RawCaptureSnapshot }
  | { id: string; ok: true; type: "zoom-media-spine-sync"; spineSnapshot: ZoomMediaSpineNativeSnapshot }
  | { id: string; ok: true; devices: CaptureDeviceState[] }
  | { id: string; ok: false; error: { code: "invalid-request" | "media-core-failed" | "zoom-failed" | "zoom-spine-failed" | "capture-failed"; message: string } };

type ZoomVideoFrameWire = {
  participantId: string;
  width: number;
  height: number;
  frameId: number;
  rgba: number[] | Uint8ClampedArray;
};

export type CoreEvent = { type: "zoom-video-frame"; frame: ZoomVideoFrame };

export function normalizeZoomVideoFrameWire(wire: ZoomVideoFrameWire): ZoomVideoFrame {
  return {
    participantId: wire.participantId,
    width: wire.width,
    height: wire.height,
    frameId: wire.frameId,
    rgba: wire.rgba instanceof Uint8ClampedArray ? wire.rgba : new Uint8ClampedArray(wire.rgba)
  };
}

/** Parse a single stdio line into a CoreResponse, or null when unparseable. */
export function parseCoreResponse(line: string): CoreResponse | null {
  const trimmed = line.trim();
  if (trimmed.length === 0) {
    return null;
  }
  try {
    const value = JSON.parse(trimmed) as CoreResponse;
    return typeof value?.id === "string" ? value : null;
  } catch {
    return null;
  }
}

/** Parse a single unsolicited stdio line into a CoreEvent, or null when unparseable. */
export function parseCoreEvent(line: string): CoreEvent | null {
  const trimmed = line.trim();
  if (trimmed.length === 0) {
    return null;
  }
  try {
    const value = JSON.parse(trimmed) as { id?: unknown; type?: string; frame?: ZoomVideoFrameWire };
    if (value?.id !== undefined || value?.type !== "zoom-video-frame" || !value.frame) {
      return null;
    }
    const { participantId, width, height, frameId, rgba } = value.frame;
    if (
      typeof participantId !== "string" ||
      typeof width !== "number" ||
      typeof height !== "number" ||
      typeof frameId !== "number" ||
      !Array.isArray(rgba)
    ) {
      return null;
    }
    return {
      type: "zoom-video-frame",
      frame: normalizeZoomVideoFrameWire(value.frame)
    };
  } catch {
    return null;
  }
}