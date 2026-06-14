import type { NativeBridgeCommand, NativeBridgeResponse, NativeZoomTransport } from "./nativeBridgeProtocol";
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
