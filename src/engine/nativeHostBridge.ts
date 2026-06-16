import type { SupportBundle } from "../domain/production";
import type {
  NativeBridgeCommand,
  NativeBridgeResponse,
  NativeZoomTransport,
  ZoomOAuthSessionStatus
} from "./nativeBridgeProtocol";
import {
  NativeZoomBridgeError,
  createMediaCoreHandshakeCommand,
  createMediaCoreSyncCommand,
  isMediaCoreProfileResponse,
  isMediaCoreSnapshotResponse,
  nextBridgeCommandId
} from "./nativeBridgeProtocol";
import type { MediaCoreHealth, NativeMediaCoreCommand, NativeMediaCoreProfile, NativeMediaCoreStateSnapshot } from "./nativeMediaCoreProtocol";
import type { ZoomMediaSpineNativeSnapshot } from "./zoomMediaSpineNativeSync";
import type { ZoomMediaSpineSyncPayload } from "./zoomMediaSpineSync";
import type { ZoomVideoFrame } from "./zoomVideoFrames";

export type NativeHostBridge = {
  request(command: NativeBridgeCommand): Promise<NativeBridgeResponse>;
  platform: "win32" | "darwin" | "linux" | string;
  host: "native-shell" | "tauri" | "electron" | "test-host" | string;
  mediaCoreProfile?: NativeMediaCoreProfile;
  syncMediaCore?(commands: NativeMediaCoreCommand[], elapsedMs: number): Promise<NativeMediaCoreStateSnapshot>;
  syncZoomMediaSpine?(payload: ZoomMediaSpineSyncPayload, elapsedMs: number): Promise<ZoomMediaSpineNativeSnapshot>;
  getMediaCoreHealth?(): Promise<MediaCoreHealth>;
  startMediaCore?(): Promise<NativeMediaCoreProfile>;
  stopMediaCore?(): Promise<void>;
  /**
   * Subscribe to the live per-participant RGBA video frame stream pushed from the
   * native Zoom capture engine. Returns an unsubscribe function. Absent on hosts
   * without the engine (e.g. the in-container mock), in which case tiles fall
   * back to the simulated frame placeholder.
   */
  onZoomVideoFrame?(listener: (frame: ZoomVideoFrame) => void): () => void;
  getZoomOAuthStatus?(): Promise<ZoomOAuthSessionStatus>;
  beginZoomOAuth?(): Promise<string>;
  signOutZoomOAuth?(): Promise<string>;
  onZoomOAuthUpdated?(listener: () => void): () => void;
  onZoomOAuthError?(listener: (message: string) => void): () => void;
  exportSupportBundle?(bundle: SupportBundle): Promise<{ path: string; bytes: number }>;
};

declare global {
  interface Window {
    coreVideoNative?: NativeHostBridge;
  }
}

export function createNativeHostTransport(bridge: NativeHostBridge): NativeZoomTransport {
  return {
    request(command: NativeBridgeCommand) {
      return bridge.request(command);
    }
  };
}

export function getNativeHostBridge() {
  return typeof window !== "undefined" ? window.coreVideoNative : undefined;
}

/**
 * Push a batch of media-core commands through the unified bridge `request` RPC
 * and unwrap the resulting state snapshot. The renderer end of the seam that
 * connects `buildNativeMediaCoreCommands()` to the native media core.
 */
export async function syncMediaCoreThroughBridge(
  bridge: NativeHostBridge,
  commands: NativeMediaCoreCommand[],
  elapsedMs: number
): Promise<NativeMediaCoreStateSnapshot> {
  const response = await bridge.request(createMediaCoreSyncCommand(commands, elapsedMs));
  if (isMediaCoreSnapshotResponse(response)) {
    return response.snapshot;
  }
  if (response.ok === false) {
    throw new NativeZoomBridgeError(response.error.code, response.error.message);
  }
  throw new NativeZoomBridgeError("protocol-error", "Bridge did not return a media-core snapshot.");
}

/**
 * Ask the native media core to announce its capability profile via the bridge.
 * Returns `undefined` when the core declines or is unreachable so startup can
 * fall back to a degraded runtime status rather than throwing.
 */
export async function handshakeMediaCoreThroughBridge(
  bridge: NativeHostBridge
): Promise<NativeMediaCoreProfile | undefined> {
  try {
    const response = await bridge.request(createMediaCoreHandshakeCommand());
    return isMediaCoreProfileResponse(response) ? response.profile : undefined;
  } catch {
    return undefined;
  }
}

/**
 * Ensure a bridge exposes `syncMediaCore`, backing it with the unified `request`
 * RPC when the host did not provide a dedicated method. Mutates and returns the
 * bridge so renderer bootstrap can wire it in one call.
 */
export function attachBridgeMediaCoreSync(bridge: NativeHostBridge): NativeHostBridge {
  if (!bridge.syncMediaCore) {
    bridge.syncMediaCore = (commands, elapsedMs) => syncMediaCoreThroughBridge(bridge, commands, elapsedMs);
  }
  return bridge;
}

/**
 * Forward a `ZoomMediaSpineSyncPayload` through the bridge `request` RPC and
 * unwrap the resulting `ZoomMediaSpineNativeSnapshot`. The renderer end of the
 * seam that connects `NativeHostZoomMediaSpineSyncEngine` to the native core.
 */
export async function syncZoomMediaSpineThroughBridge(
  bridge: NativeHostBridge,
  payload: ZoomMediaSpineSyncPayload,
  elapsedMs: number
): Promise<ZoomMediaSpineNativeSnapshot> {
  const id = nextBridgeCommandId("zoom-media-spine-sync");
  const response = await bridge.request({ id, type: "zoom-media-spine-sync", payload: { spinePayload: payload, elapsedMs } });
  if (response.ok && "spineSnapshot" in response) {
    return response.spineSnapshot;
  }
  const message = response.ok ? "Bridge did not return a spine snapshot." : response.error.message;
  throw new NativeZoomBridgeError("protocol-error", `zoom-media-spine sync failed: ${message}`);
}

/**
 * Ensure a bridge exposes `syncZoomMediaSpine`, backing it with the unified
 * `request` RPC. Mutates and returns the bridge.
 */
export function attachBridgeZoomMediaSpineSync(bridge: NativeHostBridge): NativeHostBridge {
  if (!bridge.syncZoomMediaSpine) {
    bridge.syncZoomMediaSpine = (payload, elapsedMs) => syncZoomMediaSpineThroughBridge(bridge, payload, elapsedMs);
  }
  return bridge;
}

/**
 * Ensure a bridge exposes `getMediaCoreHealth`, backed by the unified `request`
 * RPC. Returns synthetic zero-health when the core is unreachable.
 */
export function attachBridgeMediaCoreHealth(bridge: NativeHostBridge): NativeHostBridge {
  if (!bridge.getMediaCoreHealth) {
    bridge.getMediaCoreHealth = async (): Promise<MediaCoreHealth> => {
      try {
        const id = nextBridgeCommandId("media-core-health");
        const response = await bridge.request({ id, type: "get-media-core-health" });
        if (response.ok && "health" in response) {
          return response.health as MediaCoreHealth;
        }
      } catch {
        // Bridge unavailable — return a safe default.
      }
      return { restartCount: 0, recovering: false, stopped: false };
    };
  }
  return bridge;
}

export function attachBridgeMediaCoreLifecycle(bridge: NativeHostBridge): NativeHostBridge {
  if (!bridge.startMediaCore) {
    bridge.startMediaCore = async () => {
      const id = nextBridgeCommandId("start-media-core");
      const response = await bridge.request({ id, type: "start-media-core" });
      if (response.ok && "profile" in response) {
        bridge.mediaCoreProfile = response.profile;
        return response.profile;
      }
      const message = response.ok ? "Bridge did not return a media-core profile." : response.error.message;
      throw new NativeZoomBridgeError("media-core-unreachable", message);
    };
  }
  if (!bridge.stopMediaCore) {
    bridge.stopMediaCore = async () => {
      const id = nextBridgeCommandId("stop-media-core");
      const response = await bridge.request({ id, type: "stop-media-core" });
      if (response.ok && "stopped" in response) {
        return;
      }
      const message = response.ok ? "Bridge did not confirm media-core stop." : response.error.message;
      throw new NativeZoomBridgeError("media-core-failed", message);
    };
  }
  return bridge;
}

export function attachBridgeSupportBundleExport(bridge: NativeHostBridge): NativeHostBridge {
  if (!bridge.exportSupportBundle) {
    bridge.exportSupportBundle = async (bundle: SupportBundle) => {
      const id = nextBridgeCommandId("export-support-bundle");
      const response = await bridge.request({ id, type: "export-support-bundle", payload: { bundle } });
      if (response.ok && "export" in response) {
        return response.export;
      }
      const message = response.ok ? "Bridge did not return an export result." : response.error.message;
      throw new NativeZoomBridgeError("protocol-error", `support bundle export failed: ${message}`);
    };
  }
  return bridge;
}
