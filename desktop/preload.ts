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
import type { AppUpdateSnapshot } from "./appUpdate.ts";
import type { CaptionBrokerCue, CaptionBrokerSession } from "./captionBrokerTypes.ts";
import type { LicenseEntitlements } from "./licenseTypes.ts";
import type { TelemetryConsentState } from "./telemetryConsent.ts";
import type { TelemetryEvent } from "./telemetryClient.ts";

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

async function startMediaCore(): Promise<NativeMediaCoreProfile> {
  const response = await request({ id: `start-media-core-${Date.now()}`, type: "start-media-core" });
  if (response.ok && "profile" in response) {
    return response.profile;
  }
  const message = response.ok ? "Unexpected start response." : response.error.message;
  throw new Error(message);
}

async function stopMediaCore(): Promise<void> {
  const response = await request({ id: `stop-media-core-${Date.now()}`, type: "stop-media-core" });
  if (response.ok && "stopped" in response) {
    return;
  }
  const message = response.ok ? "Unexpected stop response." : response.error.message;
  throw new Error(message);
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

async function getAppUpdateStatus(): Promise<AppUpdateSnapshot> {
  return (await ipcRenderer.invoke("corevideo:get-update-status")) as AppUpdateSnapshot;
}

async function checkForAppUpdates(): Promise<AppUpdateSnapshot> {
  return (await ipcRenderer.invoke("corevideo:check-for-updates")) as AppUpdateSnapshot;
}

async function downloadAppUpdate(): Promise<AppUpdateSnapshot> {
  return (await ipcRenderer.invoke("corevideo:download-update")) as AppUpdateSnapshot;
}

async function installAppUpdate(): Promise<void> {
  await ipcRenderer.invoke("corevideo:install-update");
}

function onAppUpdateStatus(listener: (status: AppUpdateSnapshot) => void): () => void {
  const channelListener = (...args: unknown[]) => listener(args[1] as AppUpdateSnapshot);
  ipcRenderer.on("corevideo:app-update-status", channelListener);
  return () => ipcRenderer.off("corevideo:app-update-status", channelListener);
}

async function getTelemetryConsent(): Promise<TelemetryConsentState> {
  return (await ipcRenderer.invoke("corevideo:get-telemetry-consent")) as TelemetryConsentState;
}

async function setTelemetryConsent(analyticsEnabled: boolean): Promise<TelemetryConsentState> {
  return (await ipcRenderer.invoke("corevideo:set-telemetry-consent", analyticsEnabled)) as TelemetryConsentState;
}

async function recordTelemetryEvent(event: TelemetryEvent): Promise<void> {
  await ipcRenderer.invoke("corevideo:record-telemetry-event", event);
}

function reportRendererError(message: string, source?: string): void {
  void ipcRenderer.invoke("corevideo:renderer-error", { message, source });
}

window.addEventListener("error", (event) => {
  reportRendererError(event.message, event.filename);
});

window.addEventListener("unhandledrejection", (event) => {
  const reason = event.reason instanceof Error ? event.reason.message : String(event.reason);
  reportRendererError(reason, "unhandledrejection");
});

async function getLicenseEntitlements(): Promise<LicenseEntitlements> {
  return (await ipcRenderer.invoke("corevideo:license-get-entitlements")) as LicenseEntitlements;
}

async function startLicenseTrial(email?: string): Promise<{ ok: boolean; licenseKey?: string; entitlements?: LicenseEntitlements; message?: string }> {
  return (await ipcRenderer.invoke("corevideo:license-start-trial", email)) as {
    ok: boolean;
    licenseKey?: string;
    entitlements?: LicenseEntitlements;
    message?: string;
  };
}

async function activateLicense(licenseKey: string, email?: string): Promise<{ ok: boolean; activated?: boolean; entitlements?: LicenseEntitlements; message?: string }> {
  return (await ipcRenderer.invoke("corevideo:license-activate", { licenseKey, email })) as {
    ok: boolean;
    activated?: boolean;
    entitlements?: LicenseEntitlements;
    message?: string;
  };
}

async function createLicenseCheckout(tier: "creator" | "pro" | "team", email?: string, successUrl?: string): Promise<{ ok: boolean; checkoutUrl?: string; sessionId?: string; message?: string }> {
  return (await ipcRenderer.invoke("corevideo:license-checkout", { tier, email, successUrl })) as {
    ok: boolean;
    checkoutUrl?: string;
    sessionId?: string;
    message?: string;
  };
}

async function startCaptionBrokerSession(productionSessionId?: string, language?: string): Promise<{ ok: boolean; session?: CaptionBrokerSession; message?: string }> {
  return (await ipcRenderer.invoke("corevideo:caption-broker-start", { productionSessionId, language })) as {
    ok: boolean;
    session?: CaptionBrokerSession;
    message?: string;
  };
}

async function pushCaptionBrokerChunk(payload: {
  brokerSessionId?: string;
  atMs: number;
  speakerId?: string;
  speakerName?: string;
  audioLevel?: number;
  audioBase64?: string;
}): Promise<{ ok: boolean; latencyMs?: number; cues?: CaptionBrokerCue[]; message?: string }> {
  return (await ipcRenderer.invoke("corevideo:caption-broker-push-chunk", payload)) as {
    ok: boolean;
    latencyMs?: number;
    cues?: CaptionBrokerCue[];
    message?: string;
  };
}

async function endCaptionBrokerSession(brokerSessionId?: string): Promise<{ ok: boolean; ended?: boolean; message?: string }> {
  return (await ipcRenderer.invoke("corevideo:caption-broker-end", brokerSessionId)) as {
    ok: boolean;
    ended?: boolean;
    message?: string;
  };
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
  startMediaCore,
  stopMediaCore,
  onZoomVideoFrame,
  getZoomOAuthStatus,
  beginZoomOAuth,
  signOutZoomOAuth,
  onZoomOAuthUpdated,
  onZoomOAuthError,
  exportSupportBundle,
  getAppUpdateStatus,
  checkForAppUpdates,
  downloadAppUpdate,
  installAppUpdate,
  onAppUpdateStatus,
  getTelemetryConsent,
  setTelemetryConsent,
  recordTelemetryEvent,
  getLicenseEntitlements,
  startLicenseTrial,
  activateLicense,
  createLicenseCheckout,
  startCaptionBrokerSession,
  pushCaptionBrokerChunk,
  endCaptionBrokerSession
});
