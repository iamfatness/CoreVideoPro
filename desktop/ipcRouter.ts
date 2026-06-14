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
import type { NativeMediaCoreCommand, NativeMediaCoreProfile, NativeMediaCoreStateSnapshot } from "../src/engine/nativeMediaCoreProtocol";
import type { RawCaptureSnapshot } from "../src/engine/captureSnapshotMapper";
import type { ZoomMediaSpineNativeSnapshot } from "../src/engine/zoomMediaSpineNativeSync";
import type { ZoomMediaSpineSyncPayload } from "../src/engine/zoomMediaSpineSync";

/** The slice of the supervisor the router depends on (eases testing). */
export type MediaCoreBackend = {
  getProfile(): NativeMediaCoreProfile | undefined;
  handshake(): Promise<NativeMediaCoreProfile | undefined>;
  syncMediaCore(commands: NativeMediaCoreCommand[], elapsedMs: number): Promise<NativeMediaCoreStateSnapshot>;
  syncZoomMediaSpine(payload: ZoomMediaSpineSyncPayload, elapsedMs: number): Promise<ZoomMediaSpineNativeSnapshot>;
};

export type IpcRouterOptions = {
  mediaCore: MediaCoreBackend;
  output?: MockOutputEngine;
  captureDevices?: MockCaptureDeviceEngine;
};

export type IpcRouter = (command: NativeBridgeCommand) => Promise<NativeBridgeResponse>;

/** Minimal in-memory Zoom capture simulation producing RawCaptureSnapshots. */
class SimulatedZoom {
  private tick = 0;
  private joined = false;

  join(): RawCaptureSnapshot {
    this.joined = true;
    this.tick += 1;
    return this.snapshot();
  }

  leave(): RawCaptureSnapshot {
    this.joined = false;
    this.tick += 1;
    return this.snapshot();
  }

  snapshot(): RawCaptureSnapshot {
    this.tick += 1;
    if (!this.joined) {
      return { meetingState: "idle", participants: [], tick: this.tick };
    }
    return {
      meetingState: "in_meeting",
      activeSpeakerId: "host-1",
      caption: "",
      tick: this.tick,
      participants: [
        { userId: "host-1", displayName: "Host", role: "Host", videoOn: true, talking: true, audioLevel: 0.6 },
        { userId: "guest-1", displayName: "Guest", role: "Guest", videoOn: true, talking: false, audioLevel: 0.1 }
      ]
    };
  }
}

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

export function createIpcRouter(options: IpcRouterOptions): IpcRouter {
  const { mediaCore } = options;
  const output = options.output ?? new MockOutputEngine();
  const captureDevices = options.captureDevices ?? new MockCaptureDeviceEngine();
  const zoom = new SimulatedZoom();
  const audio = new AudioStub();
  const caption = new CaptionStub();

  return async function route(command: NativeBridgeCommand): Promise<NativeBridgeResponse> {
    const id = command.id;
    try {
      switch (command.type) {
        // ----- Zoom -----
        case "join":
          return { id, ok: true, snapshot: zoom.join() };
        case "leave":
          return { id, ok: true, snapshot: zoom.leave() };
        case "snapshot":
          return { id, ok: true, snapshot: zoom.snapshot() };

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
      return { id, ok: false, error: { code: "protocol-error", message } };
    }
  };
}
