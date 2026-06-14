import type { NativeBridgeCommand, NativeBridgeResponse, NativeZoomTransport } from "./nativeBridgeProtocol";

export type NativeHostBridge = {
  request(command: NativeBridgeCommand): Promise<NativeBridgeResponse>;
  platform: "win32" | "darwin" | "linux" | string;
  host: "native-shell" | "tauri" | "electron" | "test-host" | string;
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
