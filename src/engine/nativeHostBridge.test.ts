import { describe, expect, it } from "vitest";
import {
  attachBridgeMediaCoreSync,
  createNativeHostTransport,
  handshakeMediaCoreThroughBridge,
  syncMediaCoreThroughBridge,
  type NativeHostBridge
} from "./nativeHostBridge";
import type { NativeBridgeCommand, NativeBridgeResponse } from "./nativeBridgeProtocol";
import { IDLE_NATIVE_AUDIO_MIX_SESSION, IDLE_NATIVE_AUDIO_ROUTING_MATRIX, IDLE_NATIVE_CAPTION_TRACK } from "./nativeMediaCoreAudioCaption";
import { IDLE_NATIVE_BRAND_KIT } from "./nativeMediaCoreBrandKit";
import type { NativeMediaCoreMediaPlayback, NativeMediaCoreProfile, NativeMediaCoreStateSnapshot } from "./nativeMediaCoreProtocol";

const IDLE_NATIVE_MEDIA_PLAYBACK: NativeMediaCoreMediaPlayback = {
  status: "idle",
  playing: false,
  summary: "No media asset selected.",
  warnings: []
};

const profile: NativeMediaCoreProfile = {
  name: "Stub core",
  renderer: "software",
  maxProgramResolution: "1920x1080",
  maxProgramFps: 60,
  maxParticipantFeeds: 8,
  maxIsoRecordings: 4,
  capabilities: ["zoom-raw-video"]
};

const outputProfile = { profileId: "1080p60", resolution: "1920x1080", width: 1920, height: 1080, fps: 60, targetBitrateMbps: 8 };
const sourceSnapshot = {
  adapterId: "stub",
  kind: "test-pattern" as const,
  status: "idle" as const,
  subscribedSourceCount: 0,
  liveFrameCount: 0,
  staleFrameCount: 0,
  droppedFrameCount: 0,
  lowResolutionFrameCount: 0,
  warnings: []
};
const renderPlan = {
  renderPlanId: "plan-0",
  outputProfile,
  colorGrade: { lut: "none" as const, exposure: 0, contrast: 0, saturation: 0, temperature: 0 },
  sourceCount: 0,
  resolvedRouteCount: 0,
  layers: [],
  routes: [],
  warnings: []
};
const encoderSession = { status: "idle" as const, programFrameCount: 0, targets: [], lifecycle: { status: "idle" as const, lastTransition: "idle" }, warnings: [] };
const outputSenderSession = { status: "idle" as const, activeSenderCount: 0, senders: [], warnings: [] };
const compositor = { status: "idle" as const, programFrameCount: 0, droppedFrameCount: 0, degradedFrameCount: 0 };
const programTransport = { transportId: "in-process-preview" as const, status: "idle" as const, latencyMs: 0 };

function fakeSnapshot(): NativeMediaCoreStateSnapshot {
  return {
    routeCount: 0,
    frameCount: 0,
    frames: [],
    sourceSnapshot,
    programFrameCount: 0,
    programTransport,
    compositor,
    participantTransformCount: 0,
    overlayCount: 0,
    outputs: [],
    isoParticipantIds: [],
    outputProfile,
    outputHealth: [],
    outputSenderSession,
    sourceCount: 0,
    resolvedRouteCount: 0,
    renderPlan,
    encoderSession,
    audioMixSession: IDLE_NATIVE_AUDIO_MIX_SESSION,
    audioRoutingMatrix: IDLE_NATIVE_AUDIO_ROUTING_MATRIX,
    captionTrack: IDLE_NATIVE_CAPTION_TRACK,
    brandKit: IDLE_NATIVE_BRAND_KIT,
    mediaPlayback: IDLE_NATIVE_MEDIA_PLAYBACK,
    operatorActions: [],
    eventLog: [],
    diagnostics: {
      generatedAtMs: 0,
      routeCount: 0,
      frameCount: 0,
      programFrameCount: 0,
      outputs: [],
      outputProfile,
      outputHealth: [],
      outputSenderSession,
      sourceSnapshot,
      renderPlan,
      compositor,
      programTransport,
      encoderSession,
      audioMixSession: IDLE_NATIVE_AUDIO_MIX_SESSION,
      audioRoutingMatrix: IDLE_NATIVE_AUDIO_ROUTING_MATRIX,
      captionTrack: IDLE_NATIVE_CAPTION_TRACK,
      brandKit: IDLE_NATIVE_BRAND_KIT,
      mediaPlayback: IDLE_NATIVE_MEDIA_PLAYBACK,
      operatorActions: [],
      eventLog: [],
      warnings: [],
      lastCommandTypes: []
    },
    lastCommandTypes: [],
    warnings: []
  };
}

describe("native host bridge", () => {
  it("adapts any desktop shell preload bridge into the native transport contract", async () => {
    const commands: NativeBridgeCommand[] = [];
    const bridge: NativeHostBridge = {
      host: "test-host",
      platform: "win32",
      async request(command) {
        commands.push(command);
        return {
          id: command.id,
          ok: false,
          error: {
            code: "native-unavailable",
            message: "Native media core is not running."
          }
        };
      }
    };

    const transport = createNativeHostTransport(bridge);
    const response = await transport.request({ id: "1", type: "snapshot" });

    expect(commands).toEqual([{ id: "1", type: "snapshot" }]);
    expect(response).toMatchObject({
      ok: false,
      error: {
        code: "native-unavailable"
      }
    });
  });

  it("routes buildNativeMediaCoreCommands batches through the request RPC", async () => {
    const seen: NativeBridgeCommand[] = [];
    const bridge: NativeHostBridge = {
      host: "native-shell",
      platform: "linux",
      async request(command): Promise<NativeBridgeResponse> {
        seen.push(command);
        return { id: command.id, ok: true, snapshot: fakeSnapshot() };
      }
    };

    const snapshot = await syncMediaCoreThroughBridge(bridge, [{ type: "set-active-speaker", participantId: "p1" }], 1234);

    expect(seen[0]).toMatchObject({ type: "media-core-sync", payload: { elapsedMs: 1234 } });
    expect(snapshot.programTransport.transportId).toBe("in-process-preview");
  });

  it("returns the announced profile from a handshake", async () => {
    const bridge: NativeHostBridge = {
      host: "native-shell",
      platform: "linux",
      async request(command): Promise<NativeBridgeResponse> {
        return { id: command.id, ok: true, profile };
      }
    };

    expect(await handshakeMediaCoreThroughBridge(bridge)).toEqual(profile);
  });

  it("yields undefined when the handshake is declined or throws", async () => {
    const declining: NativeHostBridge = {
      host: "native-shell",
      platform: "linux",
      async request(command): Promise<NativeBridgeResponse> {
        return { id: command.id, ok: false, error: { code: "native-unavailable", message: "no core" } };
      }
    };
    expect(await handshakeMediaCoreThroughBridge(declining)).toBeUndefined();

    const throwing: NativeHostBridge = {
      host: "native-shell",
      platform: "linux",
      async request() {
        throw new Error("ipc down");
      }
    };
    expect(await handshakeMediaCoreThroughBridge(throwing)).toBeUndefined();
  });

  it("attachBridgeMediaCoreSync backs syncMediaCore with the request RPC", async () => {
    const bridge: NativeHostBridge = {
      host: "native-shell",
      platform: "linux",
      async request(command): Promise<NativeBridgeResponse> {
        return { id: command.id, ok: true, snapshot: fakeSnapshot() };
      }
    };

    attachBridgeMediaCoreSync(bridge);
    expect(bridge.syncMediaCore).toBeTypeOf("function");
    const snapshot = await bridge.syncMediaCore!([], 0);
    expect(snapshot.compositor.status).toBe("idle");
  });

  it("attachBridgeMediaCoreSync preserves a host-provided syncMediaCore", () => {
    const provided = async () => fakeSnapshot();
    const bridge: NativeHostBridge = {
      host: "native-shell",
      platform: "linux",
      async request(command): Promise<NativeBridgeResponse> {
        return { id: command.id, ok: false, error: { code: "native-unavailable", message: "x" } };
      },
      syncMediaCore: provided
    };

    attachBridgeMediaCoreSync(bridge);
    expect(bridge.syncMediaCore).toBe(provided);
  });
});
