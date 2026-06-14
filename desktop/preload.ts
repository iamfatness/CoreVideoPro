/**
 * Electron preload. Exposes `window.coreVideoNative` implementing the renderer's
 * NativeHostBridge contract over contextBridge — no Node globals leak into the
 * renderer. Every command rides the single `corevideo:request` invoke channel;
 * the capability profile is fetched synchronously so the renderer detects the
 * native runtime on first paint.
 */
import { contextBridge, ipcRenderer } from "electron";
import type { NativeBridgeCommand, NativeBridgeResponse } from "../src/engine/nativeBridgeProtocol";
import type { NativeMediaCoreCommand, NativeMediaCoreProfile, NativeMediaCoreStateSnapshot } from "../src/engine/nativeMediaCoreProtocol";
import type { ZoomMediaSpineNativeSnapshot } from "../src/engine/zoomMediaSpineNativeSync";
import type { ZoomMediaSpineSyncPayload } from "../src/engine/zoomMediaSpineSync";

const mediaCoreProfile = (ipcRenderer.sendSync("corevideo:handshake") as NativeMediaCoreProfile | null) ?? undefined;

let syncCounter = 0;
let spineCounter = 0;

function request(command: NativeBridgeCommand): Promise<NativeBridgeResponse> {
  return ipcRenderer.invoke("corevideo:request", command) as Promise<NativeBridgeResponse>;
}

async function syncMediaCore(commands: NativeMediaCoreCommand[], elapsedMs: number): Promise<NativeMediaCoreStateSnapshot> {
  syncCounter += 1;
  const response = await request({ id: `media-core-sync-${syncCounter}`, type: "media-core-sync", payload: { commands, elapsedMs } });
  if (response.ok && "snapshot" in response && !("meetingState" in response.snapshot)) {
    return response.snapshot;
  }
  const message = response.ok ? "Bridge returned a non-snapshot response." : response.error.message;
  throw new Error(`media-core sync failed: ${message}`);
}

async function syncZoomMediaSpine(spinePayload: ZoomMediaSpineSyncPayload, elapsedMs: number): Promise<ZoomMediaSpineNativeSnapshot> {
  spineCounter += 1;
  const response = await request({ id: `zoom-media-spine-sync-${spineCounter}`, type: "zoom-media-spine-sync", payload: { spinePayload, elapsedMs } });
  if (response.ok && "spineSnapshot" in response) {
    return response.spineSnapshot;
  }
  const message = response.ok ? "Bridge returned a non-spine-snapshot response." : response.error.message;
  throw new Error(`zoom-media-spine sync failed: ${message}`);
}

contextBridge.exposeInMainWorld("coreVideoNative", {
  host: "electron",
  platform: process.platform,
  mediaCoreProfile,
  request,
  syncMediaCore,
  syncZoomMediaSpine
});
