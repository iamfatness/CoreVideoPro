/**
 * Electron preload. Exposes `window.coreVideoNative` implementing the renderer's
 * NativeHostBridge contract over contextBridge — no Node globals leak into the
 * renderer. Every command rides the single `corevideo:request` invoke channel;
 * the capability profile is fetched synchronously so the renderer detects the
 * native runtime on first paint.
 */
import { contextBridge, ipcRenderer } from "electron";
import type { SupportBundle } from "../src/domain/production";
import type { NativeBridgeCommand, NativeBridgeResponse, ZoomOAuthSessionStatus } from "../src/engine/nativeBridgeProtocol";
import type { MediaCoreHealth, NativeMediaCoreCommand, NativeMediaCoreProfile, NativeMediaCoreStateSnapshot } from "../src/engine/nativeMediaCoreProtocol";
import type { ZoomMediaSpineNativeSnapshot } from "../src/engine/zoomMediaSpineNativeSync";
import type { ZoomMediaSpineSyncPayload } from "../src/engine/zoomMediaSpineSync";
import type { ZoomVideoFrame } from "../src/engine/zoomVideoFrames";

const mediaCoreProfile = (ipcRenderer.sendSync("corevideo:handshake") as NativeMediaCoreProfile | null) ?? undefined;

let syncCounter = 0;
let spineCounter = 0;
let healthCounter = 0;

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

async function getMediaCoreHealth(): Promise<MediaCoreHealth> {
  healthCounter += 1;
  const response = await request({ id: `media-core-health-${healthCounter}`, type: "get-media-core-health" });
  if (response.ok && "health" in response) {
    return response.health as MediaCoreHealth;
  }
  return { restartCount: 0, recovering: false, stopped: false };
}

async function getZoomOAuthStatus(): Promise<ZoomOAuthSessionStatus> {
  const response = await request({ id: `zoom-oauth-status-${Date.now()}`, type: "get-zoom-oauth-status" });
  if (response.ok && "oauthStatus" in response) {
    return response.oauthStatus;
  }
  return { brokerConfigured: false, signedIn: false, expiresAt: 0, pendingAuthorization: false };
}

async function beginZoomOAuth(): Promise<string> {
  const response = await request({ id: `zoom-oauth-begin-${Date.now()}`, type: "zoom-oauth-begin" });
  if (response.ok && "oauthMessage" in response) {
    return response.oauthMessage;
  }
  const message = response.ok ? "Unexpected OAuth response." : response.error.message;
  throw new Error(message);
}

async function signOutZoomOAuth(): Promise<string> {
  const response = await request({ id: `zoom-oauth-sign-out-${Date.now()}`, type: "zoom-oauth-sign-out" });
  if (response.ok && "oauthMessage" in response) {
    return response.oauthMessage;
  }
  const message = response.ok ? "Unexpected OAuth response." : response.error.message;
  throw new Error(message);
}

function onZoomOAuthUpdated(listener: () => void): () => void {
  const channelListener = () => listener();
  ipcRenderer.on("corevideo:zoom-oauth-updated", channelListener);
  return () => ipcRenderer.off("corevideo:zoom-oauth-updated", channelListener);
}

function onZoomOAuthError(listener: (message: string) => void): () => void {
  const channelListener = (...args: unknown[]) => listener(args[1] as string);
  ipcRenderer.on("corevideo:zoom-oauth-error", channelListener);
  return () => ipcRenderer.off("corevideo:zoom-oauth-error", channelListener);
}

async function exportSupportBundle(bundle: SupportBundle): Promise<{ path: string; bytes: number }> {
  const response = await request({ id: `export-support-bundle-${Date.now()}`, type: "export-support-bundle", payload: { bundle } });
  if (response.ok && "export" in response) {
    return response.export;
  }
  const message = response.ok ? "Unexpected export response." : response.error.message;
  throw new Error(message);
}

function onZoomVideoFrame(listener: (frame: ZoomVideoFrame) => void): () => void {
  const channelListener = (...args: unknown[]) => {
    const frame = args[1] as ZoomVideoFrame;
    listener({
      ...frame,
      rgba: new Uint8ClampedArray(frame.rgba)
    });
  };
  ipcRenderer.on("corevideo:zoom-video-frame", channelListener);
  return () => ipcRenderer.off("corevideo:zoom-video-frame", channelListener);
}

contextBridge.exposeInMainWorld("coreVideoNative", {
  host: "electron",
  platform: process.platform,
  mediaCoreProfile,
  request,
  syncMediaCore,
  syncZoomMediaSpine,
  getMediaCoreHealth,
  onZoomVideoFrame,
  getZoomOAuthStatus,
  beginZoomOAuth,
  signOutZoomOAuth,
  onZoomOAuthUpdated,
  onZoomOAuthError,
  exportSupportBundle
});
