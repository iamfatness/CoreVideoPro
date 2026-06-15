/**
 * Main-process IPC router. Receives a {@link NativeBridgeCommand} from the
 * renderer (via `ipcMain.handle`) and returns a {@link NativeBridgeResponse}
 * with the matching `id`. Media-core commands are delegated to the child-process
 * supervisor; everything else is backed by the existing simulated engines ported
 * to Node, so the desktop shell runs end-to-end with zero native code.
 */
import { MockCaptureDeviceEngine } from "../src/engine/captureDevices";
import { MockOutputEngine } from "../src/engine/mockEngines";
import type {
  NativeAudioBusState,
  NativeBridgeCommand,
  NativeBridgeResponse,
  NativeCaptionCueState,
  NativeCaptionTrackState
} from "../src/engine/nativeBridgeProtocol";
import type { ZoomJoinRequest } from "../src/engine/contracts";
import type { MediaCoreHealth, NativeMediaCoreCommand, NativeMediaCoreProfile, NativeMediaCoreStateSnapshot } from "../src/engine/nativeMediaCoreProtocol";
import type { RawCaptureSnapshot } from "../src/engine/captureSnapshotMapper";
import type { ZoomMediaSpineNativeSnapshot } from "../src/engine/zoomMediaSpineNativeSync";
import type { ZoomMediaSpineSyncPayload } from "../src/engine/zoomMediaSpineSync";
import type { SupportBundle } from "../src/domain/production.ts";
import type { CrashRecoveryEvent } from "./crashRecoveryLog.ts";
import { writeSupportBundleExport } from "./diagnosticsExport.ts";
import type { ZoomOAuthService } from "./zoomOAuthService.ts";

/** The slice of the supervisor the router depends on (eases testing). */
export type MediaCoreBackend = {
  getProfile(): NativeMediaCoreProfile | undefined;
  handshake(): Promise<NativeMediaCoreProfile | undefined>;
  syncMediaCore(commands: NativeMediaCoreCommand[], elapsedMs: number): Promise<NativeMediaCoreStateSnapshot>;
  syncZoomMediaSpine(payload: ZoomMediaSpineSyncPayload, elapsedMs: number): Promise<ZoomMediaSpineNativeSnapshot>;
  joinZoom(payload: ZoomJoinRequest): Promise<RawCaptureSnapshot>;
  leaveZoom(): Promise<RawCaptureSnapshot>;
  getZoomSnapshot(): Promise<RawCaptureSnapshot>;
  getHealth(): MediaCoreHealth;
};

export type IpcRouterOptions = {
  mediaCore: MediaCoreBackend;
  zoomOAuth?: ZoomOAuthService;
  output?: MockOutputEngine;
  captureDevices?: MockCaptureDeviceEngine;
  diagnosticsDirectory?: string;
  readCrashEvents?: () => Promise<CrashRecoveryEvent[]>;
};

export type IpcRouter = (command: NativeBridgeCommand) => Promise<NativeBridgeResponse>;

/** In-memory audio mix stub (until audio is delegated to native). */
class AudioStub {
  private readonly buses: NativeAudioBusState[] = [
    { busId: "program", label: "Program", gainDb: 0, muted: false, peakDb: -18 },
    { busId: "mics", label: "Microphones", gainDb: -3, muted: false, peakDb: -22 },
    { busId: "media", label: "Media", gainDb: -6, muted: false, peakDb: -30 }
  ];

  list(): NativeAudioBusState[] {
    return this.buses.map((bus) => ({ ...bus }));
  }

  setGain(busId: string, gainDb: number): NativeAudioBusState[] {
    const bus = this.buses.find((candidate) => candidate.busId === busId);
    if (bus) {
      bus.gainDb = Math.max(-60, Math.min(12, gainDb));
    }
    return this.list();
  }

  setMute(busId: string, muted: boolean): NativeAudioBusState[] {
    const bus = this.buses.find((candidate) => candidate.busId === busId);
    if (bus) {
      bus.muted = muted;
    }
    return this.list();
  }
}

/** In-memory caption track stub (until captions are delegated to native). */
class CaptionStub {
  private enabled = true;
  private readonly cues: NativeCaptionCueState[] = [];
  private nextId = 1;

  track(): NativeCaptionTrackState {
    return { enabled: this.enabled, cues: this.cues.map((cue) => ({ ...cue })) };
  }

  setEnabled(enabled: boolean): NativeCaptionTrackState {
    this.enabled = enabled;
    return this.track();
  }

  push(text: string, atMs: number, speaker?: string): NativeCaptionTrackState {
    this.cues.push({ id: `cue-${this.nextId++}`, text, atMs, speaker });
    if (this.cues.length > 100) {
      this.cues.splice(0, this.cues.length - 100);
    }
    return this.track();
  }
}

async function joinPayloadWithOAuth(
  zoomOAuth: ZoomOAuthService | undefined,
  payload: ZoomJoinRequest
): Promise<ZoomJoinRequest> {
  if (!zoomOAuth) {
    return payload;
  }
  const creds = await zoomOAuth.ensureJoinCredentials();
  return {
    ...payload,
    ...(creds.sdkJwt ? { sdkJwt: creds.sdkJwt } : {}),
    ...(creds.userZak ? { userZak: creds.userZak } : {})
  };
}

export function createIpcRouter(options: IpcRouterOptions): IpcRouter {
  const { mediaCore, zoomOAuth, diagnosticsDirectory, readCrashEvents } = options;
  const output = options.output ?? new MockOutputEngine();
  const captureDevices = options.captureDevices ?? new MockCaptureDeviceEngine();
  const audio = new AudioStub();
  const caption = new CaptionStub();

  return async function route(command: NativeBridgeCommand): Promise<NativeBridgeResponse> {
    const id = command.id;
    try {
      switch (command.type) {
        // ----- Zoom -----
        case "join":
          return {
            id,
            ok: true,
            snapshot: await mediaCore.joinZoom(await joinPayloadWithOAuth(zoomOAuth, command.payload))
          };
        case "get-zoom-oauth-status":
          if (!zoomOAuth) {
            return {
              id,
              ok: true,
              oauthStatus: { brokerConfigured: false, signedIn: false, expiresAt: 0, pendingAuthorization: false }
            };
          }
          return { id, ok: true, oauthStatus: await zoomOAuth.getStatus() };
        case "zoom-oauth-begin":
          if (!zoomOAuth) {
            return { id, ok: false, error: { code: "native-unavailable", message: "Zoom OAuth is only available in the Electron shell." } };
          }
          await zoomOAuth.beginAuthorization();
          return { id, ok: true, oauthMessage: "Browser opened for Zoom sign-in. Approve the request and return to CoreVideo Pro." };
        case "zoom-oauth-sign-out":
          if (!zoomOAuth) {
            return { id, ok: true, oauthMessage: "Signed out." };
          }
          await zoomOAuth.signOut();
          return { id, ok: true, oauthMessage: "Signed out of Zoom." };
        case "leave":
          return { id, ok: true, snapshot: await mediaCore.leaveZoom() };
        case "snapshot":
          return { id, ok: true, snapshot: await mediaCore.getZoomSnapshot() };

        // ----- Output -----
        case "set-output-profile":
          return { id, ok: true, session: await output.setOutputProfile(command.payload) };
        case "start-recording":
          return { id, ok: true, session: await output.startRecording(command.payload) };
        case "stop-recording":
          return { id, ok: true, session: await output.stopRecording() };
        case "start-stream":
          return { id, ok: true, session: await output.startStream(command.payload.destinations) };
        case "stop-stream":
          return { id, ok: true, session: await output.stopStream() };
        case "get-output-health":
          return { id, ok: true, health: await output.getHealth() };
        case "get-output-session":
          return { id, ok: true, session: await output.getSession() };

        // ----- Capture devices -----
        case "list-capture-devices":
          return { id, ok: true, devices: await captureDevices.listDevices() };
        case "select-capture-input":
          return { id, ok: true, devices: await captureDevices.selectInput(command.payload.deviceId, command.payload.inputId) };
        case "set-capture-audio-sync-offset":
          return { id, ok: true, devices: await captureDevices.setAudioSyncOffset(command.payload.deviceId, command.payload.offsetMs) };
        case "connect-capture-device":
          return { id, ok: true, devices: await captureDevices.connectDevice(command.payload.deviceId) };

        // ----- Media core (delegated to the supervised child process) -----
        case "media-core-handshake": {
          const profile = mediaCore.getProfile() ?? (await mediaCore.handshake());
          if (!profile) {
            return { id, ok: false, error: { code: "media-core-unreachable", message: "Media core did not announce a profile." } };
          }
          return { id, ok: true, profile };
        }
        case "media-core-sync": {
          const snapshot = await mediaCore.syncMediaCore(command.payload.commands, command.payload.elapsedMs);
          return { id, ok: true, snapshot };
        }

        // ----- Media core health (handled at router layer) -----
        case "get-media-core-health":
          return { id, ok: true, health: mediaCore.getHealth() };

        case "export-support-bundle": {
          if (!diagnosticsDirectory) {
            return { id, ok: false, error: { code: "native-unavailable", message: "Diagnostics export is only available in the Electron shell." } };
          }
          const crashEvents = readCrashEvents ? await readCrashEvents() : [];
          const bundle = mergeCrashEventsIntoBundle(command.payload.bundle, crashEvents);
          const exported = await writeSupportBundleExport(diagnosticsDirectory, bundle);
          return { id, ok: true, export: exported };
        }

        // ----- Zoom media spine (delegated to the supervised child process) -----
        case "zoom-media-spine-sync": {
          const spineSnapshot = await mediaCore.syncZoomMediaSpine(command.payload.spinePayload, command.payload.elapsedMs);
          return { id, ok: true, spineSnapshot };
        }

        // ----- Audio (stub) -----
        case "get-audio-mix":
          return { id, ok: true, buses: audio.list() };
        case "set-audio-bus-gain":
          return { id, ok: true, buses: audio.setGain(command.payload.busId, command.payload.gainDb) };
        case "set-audio-bus-mute":
          return { id, ok: true, buses: audio.setMute(command.payload.busId, command.payload.muted) };

        // ----- Captions (stub) -----
        case "get-caption-track":
          return { id, ok: true, track: caption.track() };
        case "set-caption-enabled":
          return { id, ok: true, track: caption.setEnabled(command.payload.enabled) };
        case "push-caption-cue":
          return { id, ok: true, track: caption.push(command.payload.text, command.payload.atMs, command.payload.speaker) };

        default: {
          const exhaustive: never = command;
          return { id: (exhaustive as { id: string }).id, ok: false, error: { code: "protocol-error", message: "Unsupported command." } };
        }
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : "Router failure.";
      if (command.type === "media-core-sync" || command.type === "media-core-handshake") {
        return { id, ok: false, error: { code: "media-core-failed", message } };
      }
      if (command.type === "join") {
        return { id, ok: false, error: { code: "join-failed", message } };
      }
      if (command.type === "leave") {
        return { id, ok: false, error: { code: "leave-failed", message } };
      }
      if (command.type === "snapshot") {
        return { id, ok: false, error: { code: "snapshot-failed", message } };
      }
      if (command.type === "zoom-oauth-begin" || command.type === "zoom-oauth-sign-out") {
        return { id, ok: false, error: { code: "oauth-failed", message } };
      }
      return { id, ok: false, error: { code: "protocol-error", message } };
    }
  };
}

function mergeCrashEventsIntoBundle(bundle: SupportBundle, crashEvents: CrashRecoveryEvent[]): SupportBundle {
  if (!crashEvents.length) {
    return bundle;
  }
  return {
    ...bundle,
    crashRecovery: { events: crashEvents }
  };
}
