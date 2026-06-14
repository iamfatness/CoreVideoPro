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
import type { ZoomVideoFrame } from "../src/engine/zoomVideoFrames";

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

export type CoreEvent = { type: "zoom-video-frame"; frame: ZoomVideoFrame };

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
    const value = JSON.parse(trimmed) as CoreEvent;
    return value?.type === "zoom-video-frame" && value.frame ? value : null;
  } catch {
    return null;
  }
}
