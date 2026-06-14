import type { NativeBridgeCommand, NativeBridgeResponse, NativeZoomTransport } from "./nativeBridgeProtocol";
import {
  NativeZoomBridgeError,
  createMediaCoreHandshakeCommand,
  createMediaCoreSyncCommand,
  isMediaCoreProfileResponse,
  isMediaCoreSnapshotResponse
} from "./nativeBridgeProtocol";
import type { NativeMediaCoreCommand, NativeMediaCoreProfile, NativeMediaCoreStateSnapshot } from "./nativeMediaCoreProtocol";
import type { ZoomMediaSpineNativeSnapshot } from "./zoomMediaSpineNativeSync";
import type { ZoomMediaSpineSyncPayload } from "./zoomMediaSpineSync";

export type NativeHostBridge = {
  request(command: NativeBridgeCommand): Promise<NativeBridgeResponse>;
  platform: "win32" | "darwin" | "linux" | string;
  host: "native-shell" | "tauri" | "electron" | "test-host" | string;
  mediaCoreProfile?: NativeMediaCoreProfile;
  syncMediaCore?(commands: NativeMediaCoreCommand[], elapsedMs: number): Promise<NativeMediaCoreStateSnapshot>;
  syncZoomMediaSpine?(payload: ZoomMediaSpineSyncPayload, elapsedMs: number): Promise<ZoomMediaSpineNativeSnapshot>;
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
