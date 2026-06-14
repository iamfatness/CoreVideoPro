/**
 * Stdio JSON-RPC wire protocol between the desktop supervisor
 * (`mediaCoreClient.ts`) and the spawned media-core process (`coreStub.ts`,
 * later replaced by Track B's native binary). One JSON object per line.
 *
 * This is intentionally distinct from the renderer-facing `nativeBridgeProtocol`:
 * the IPC router translates bridge commands into these core requests.
 */
import type { NativeMediaCoreCommand, NativeMediaCoreProfile, NativeMediaCoreStateSnapshot } from "../src/engine/nativeMediaCoreProtocol";
import type { ZoomMediaSpineNativeSnapshot } from "../src/engine/zoomMediaSpineNativeSync";
import type { ZoomMediaSpineSyncPayload } from "../src/engine/zoomMediaSpineSync";

export type CoreRequest =
  | { id: string; type: "handshake" }
  | { id: string; type: "ping" }
  | { id: string; type: "media-core-sync"; commands: NativeMediaCoreCommand[]; elapsedMs: number }
  /** Zoom media spine sync: forward the typed payload to the core. Track B mirrors this. */
  | { id: string; type: "zoom-media-spine-sync"; spinePayload: ZoomMediaSpineSyncPayload; elapsedMs: number }
  /** Test-only hook: makes the stub exit non-zero to exercise crash isolation. */
  | { id: string; type: "__crash" };

export type CoreResponse =
  | { id: string; ok: true; type: "handshake"; profile: NativeMediaCoreProfile }
  | { id: string; ok: true; type: "ping" }
  | { id: string; ok: true; type: "media-core-sync"; snapshot: NativeMediaCoreStateSnapshot }
  | { id: string; ok: true; type: "zoom-media-spine-sync"; spineSnapshot: ZoomMediaSpineNativeSnapshot }
  | { id: string; ok: false; error: { code: "invalid-request" | "media-core-failed" | "zoom-spine-failed"; message: string } };

/**
 * Unsolicited messages pushed from the core to the supervisor (no request `id`).
 * Live per-participant video frames stream this way: the core emits one event
 * per decoded thumbnail; the supervisor decodes `rgbaBase64` and forwards it to
 * the renderer's `onZoomVideoFrame`. RGBA is `width * height * 4` bytes,
 * base64-encoded so it survives the line-delimited JSON transport.
 */
export type CoreEvent = {
  type: "zoom-video-frame";
  participantId: string;
  width: number;
  height: number;
  frameId: number;
  rgbaBase64: string;
};

/**
 * Parse a single stdio line into an unsolicited CoreEvent, or null when it is
 * not one. Events are distinguished from responses by having no `id` and a known
 * event `type`, so callers can try `parseCoreResponse` first and fall back here.
 */
export function parseCoreEvent(line: string): CoreEvent | null {
  const trimmed = line.trim();
  if (trimmed.length === 0) {
    return null;
  }
  try {
    const value = JSON.parse(trimmed) as Partial<CoreEvent> & { id?: unknown };
    if (value?.id !== undefined || value?.type !== "zoom-video-frame") {
      return null;
    }
    if (
      typeof value.participantId !== "string" ||
      typeof value.width !== "number" ||
      typeof value.height !== "number" ||
      typeof value.frameId !== "number" ||
      typeof value.rgbaBase64 !== "string"
    ) {
      return null;
    }
    return {
      type: "zoom-video-frame",
      participantId: value.participantId,
      width: value.width,
      height: value.height,
      frameId: value.frameId,
      rgbaBase64: value.rgbaBase64
    };
  } catch {
    return null;
  }
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
