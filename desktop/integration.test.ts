/**
 * The milestone integration gate, runnable in a plain Linux container with the
 * Node stub core (no GPU, no Zoom SDK, no Electron):
 *
 *   supervisor spawns the stub core  →  handshake announces a profile  →
 *   describeRuntimeEnvironment reports "ready" (not "Mock studio")  →
 *   a scripted Magic Scene take round-trips NativeMediaCoreCommands through the
 *   bridge to the core, which acks with synthetic health.
 *
 * The bridge here is wired exactly as the Electron preload wires it: every
 * renderer `request` goes through the IPC router, which delegates media-core
 * traffic to the supervised child process.
 */
import { afterEach, describe, expect, it } from "vitest";
import { MediaCoreSupervisor } from "./mediaCoreClient.ts";
import { createIpcRouter } from "./ipcRouter.ts";
import { buildNativeMediaCoreCommands } from "../src/engine/nativeMediaCoreCommands";
import { describeRuntimeEnvironment } from "../src/engine/runtimeEnvironment";
import {
  attachBridgeMediaCoreHealth,
  attachBridgeMediaCoreSync,
  attachBridgeZoomMediaSpineSync,
  handshakeMediaCoreThroughBridge,
  syncMediaCoreThroughBridge,
  syncZoomMediaSpineThroughBridge,
  type NativeHostBridge
} from "../src/engine/nativeHostBridge";
import { initialProduction } from "../src/domain/production";
import { buildZoomMediaSpineSyncPayload } from "../src/engine/zoomMediaSpineSync";
import type { ZoomSdkReadinessInput } from "../src/engine/zoomSdkReadiness";

let active: MediaCoreSupervisor | undefined;

afterEach(() => {
  active?.stop();
  active = undefined;
});

async function bootBridge(): Promise<NativeHostBridge> {
  const supervisor = new MediaCoreSupervisor();
  active = supervisor;
  await supervisor.start();
  const route = createIpcRouter({ mediaCore: supervisor });

  // This mirrors the Electron preload: a NativeHostBridge whose `request` is the
  // main-process IPC router, with the handshake profile cached synchronously.
  const bridge: NativeHostBridge = {
    host: "electron",
    platform: process.platform,
    mediaCoreProfile: supervisor.getProfile(),
    request: (command) => route(command)
  };
  attachBridgeMediaCoreSync(bridge);
  attachBridgeZoomMediaSpineSync(bridge);
  attachBridgeMediaCoreHealth(bridge);
  return bridge;
}

describe("desktop integration gate", () => {
  it("renderer detects the native bridge — runtime is not the Mock studio", async () => {
    const bridge = await bootBridge();
    const runtime = describeRuntimeEnvironment(bridge, bridge.mediaCoreProfile);
    expect(runtime.status).not.toBe("mock");
    expect(runtime.label).not.toBe("Mock studio");
  });

  it("handshake yields a profile so the runtime reports ready", async () => {
    const bridge = await bootBridge();
    const profile = await handshakeMediaCoreThroughBridge(bridge);
    expect(profile).toBeDefined();
    const runtime = describeRuntimeEnvironment(bridge, profile);
    expect(runtime.status).toBe("ready");
  });

  it("round-trips a scripted Magic Scene take to the core, which acks with live health", async () => {
    const bridge = await bootBridge();
    const commands = buildNativeMediaCoreCommands(initialProduction);
    expect(commands.length).toBeGreaterThan(0);

    const snapshot = await syncMediaCoreThroughBridge(bridge, commands, 2000);

    // The whole pipe is real: the renderer's command batch reached the child
    // process and came back as synthetic media-core health.
    expect(snapshot.lastCommandTypes).toEqual(commands.map((command) => command.type));
    expect(snapshot.programFrame?.health).toBe("live");
    expect(snapshot.sceneId).toBe(initialProduction.activeSceneId);
  });

  it("syncMediaCore attached on the bridge drives the same pipe", async () => {
    const bridge = await bootBridge();
    const commands = buildNativeMediaCoreCommands(initialProduction);
    const snapshot = await bridge.syncMediaCore!(commands, 3000);
    expect(snapshot.compositor.status).toBe("live");
  });

  it("A1 gate — Zoom media spine round-trips through Electron→main→core, returning stub-sourced snapshot", async () => {
    const bridge = await bootBridge();

    const readiness: ZoomSdkReadinessInput = {
      platform: "windows",
      sdkRuntimePresent: true,
      sdkVersion: "6.1.0",
      appKeyPresent: true,
      oauthConfigured: true,
      jwtBrokerConfigured: false,
      rawVideoEnabled: true,
      rawAudioEnabled: true,
      rawShareEnabled: false
    };
    const payload = buildZoomMediaSpineSyncPayload(initialProduction, readiness);
    const spineSnapshot = await syncZoomMediaSpineThroughBridge(bridge, payload, 4000);

    // Response originated from the Node stub (not the renderer fallback).
    expect(spineSnapshot.events).toContain("zoom-media-spine-sync accepted by Node stub core.");
    // The stub reflects the participant list from the payload.
    expect(spineSnapshot.participantCount).toBe(payload.participants.length);
    // meetingState driven by payload.blocked (subscription plan); either is from the real pipe.
    expect(["in-meeting", "error"]).toContain(spineSnapshot.meetingState);
  });

  it("attachBridgeZoomMediaSpineSync wires syncZoomMediaSpine on the bridge", async () => {
    const bridge = await bootBridge();
    expect(bridge.syncZoomMediaSpine).toBeDefined();
    const readiness: ZoomSdkReadinessInput = {
      platform: "windows",
      sdkRuntimePresent: true,
      sdkVersion: "6.1.0",
      appKeyPresent: true,
      oauthConfigured: true,
      jwtBrokerConfigured: false,
      rawVideoEnabled: true,
      rawAudioEnabled: true,
      rawShareEnabled: false
    };
    const payload = buildZoomMediaSpineSyncPayload(initialProduction, readiness);
    const snapshot = await bridge.syncZoomMediaSpine!(payload, 5000);
    expect(snapshot.events).toContain("zoom-media-spine-sync accepted by Node stub core.");
  });

  it("A3 gate — force-crash triggers recovering health visible through the bridge", async () => {
    const bridge = await bootBridge();
    expect(bridge.getMediaCoreHealth).toBeDefined();

    // Baseline: no crash yet.
    const baseHealth = await bridge.getMediaCoreHealth!();
    expect(baseHealth.recovering).toBe(false);
    expect(baseHealth.restartCount).toBe(0);

    // Crash the core; supervisor restarts it.
    await active!.forceCrash();
    await new Promise((resolve) => setTimeout(resolve, 100));

    const crashHealth = await bridge.getMediaCoreHealth!();
    expect(crashHealth.restartCount).toBeGreaterThan(0);

    // describeRuntimeEnvironment surfaces the recovering state as degraded.
    const degraded = describeRuntimeEnvironment(bridge, bridge.mediaCoreProfile, crashHealth);
    expect(degraded.status).toBe("degraded");
    expect(degraded.label).toMatch(/recovering/i);
  });
});
