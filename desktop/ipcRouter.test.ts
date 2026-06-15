import { mkdtemp, readFile } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { describe, expect, it } from "vitest";
import { initialProduction } from "../src/domain/production.ts";
import { createSupportBundle } from "../src/engine/supportBundle.ts";
import { createIpcRouter, type MediaCoreBackend } from "./ipcRouter.ts";
import {
  SYNTHETIC_PROFILE,
  createSyntheticZoomCaptureState,
  synthesizeSnapshot,
  synthesizeSpineSnapshot,
  synthesizeZoomJoinSnapshot,
  synthesizeZoomLeaveSnapshot,
  synthesizeZoomSnapshot
} from "./syntheticMediaCore.ts";
import type { CaptureDeviceState } from "../src/domain/production.ts";
import type { NativeMediaCoreProfile } from "../src/engine/nativeMediaCoreProtocol";

function createCaptureBackendState(): CaptureDeviceState[] {
  return [
    {
      id: "decklink-1",
      vendor: "blackmagic",
      name: "DeckLink Mini Recorder 4K",
      inputs: [{ id: "sdi-1", label: "SDI 1", hasEmbeddedAudio: true }],
      selectedInputId: "sdi-1",
      resolution: { width: 1920, height: 1080 },
      frameRate: 60,
      connectionState: "connected",
      signalPresent: true,
      droppedFrames: 0,
      audioSyncOffsetMs: 0
    }
  ];
}

function captureBackendMethods(devices: CaptureDeviceState[]) {
  return {
    listCaptureDevices: async () => devices.map((device) => ({ ...device, inputs: device.inputs.map((input) => ({ ...input })) })),
    selectCaptureInput: async (deviceId: string, inputId: string) => {
      for (const device of devices) {
        if (device.id === deviceId && device.inputs.some((input) => input.id === inputId)) {
          device.selectedInputId = inputId;
        }
      }
      return captureBackendMethods(devices).listCaptureDevices();
    },
    setCaptureAudioSyncOffset: async (deviceId: string, offsetMs: number) => {
      for (const device of devices) {
        if (device.id === deviceId) {
          device.audioSyncOffsetMs = Math.max(-500, Math.min(500, offsetMs));
        }
      }
      return captureBackendMethods(devices).listCaptureDevices();
    },
    connectCaptureDevice: async (deviceId: string) => {
      for (const device of devices) {
        if (device.id === deviceId) {
          device.connectionState = "connected";
          device.signalPresent = true;
        }
      }
      return captureBackendMethods(devices).listCaptureDevices();
    }
  };
}

function fakeBackend(profile: NativeMediaCoreProfile | undefined = SYNTHETIC_PROFILE): MediaCoreBackend {
  const zoomState = createSyntheticZoomCaptureState();
  const captureDevices = createCaptureBackendState();
  return {
    getProfile: () => profile,
    handshake: async () => profile,
    syncMediaCore: async (commands, elapsedMs) => synthesizeSnapshot(commands, elapsedMs, 1),
    syncZoomMediaSpine: async (payload, elapsedMs) => synthesizeSpineSnapshot(payload, elapsedMs),
    joinZoom: async (payload) => synthesizeZoomJoinSnapshot(zoomState, payload),
    leaveZoom: async () => synthesizeZoomLeaveSnapshot(zoomState),
    getZoomSnapshot: async () => synthesizeZoomSnapshot(zoomState),
    getHealth: () => ({ restartCount: 0, recovering: false, stopped: false }),
    ...captureBackendMethods(captureDevices)
  };
}

describe("createIpcRouter", () => {
  it("preserves the command id on every response", async () => {
    const route = createIpcRouter({ mediaCore: fakeBackend() });
    const response = await route({ id: "xyz", type: "get-audio-mix" });
    expect(response.id).toBe("xyz");
  });

  it("dispatches zoom join to a raw capture snapshot", async () => {
    const route = createIpcRouter({ mediaCore: fakeBackend() });
    const response = await route({ id: "1", type: "join", payload: { meetingUrl: "https://zoom.us/j/1", displayName: "Op", webinar: false } });
    expect(response.ok).toBe(true);
    if (response.ok && "snapshot" in response) {
      expect(response.snapshot.meetingState).toBe("in_meeting");
      expect(response.snapshot.participants[0]?.displayName).toBe("Op");
    }
  });

  it("dispatches output commands to the simulated output engine", async () => {
    const route = createIpcRouter({ mediaCore: fakeBackend() });
    const response = await route({ id: "2", type: "get-output-session" });
    expect(response.ok).toBe(true);
    expect("session" in response).toBe(true);
  });

  it("lists capture devices", async () => {
    const route = createIpcRouter({ mediaCore: fakeBackend() });
    const response = await route({ id: "3", type: "list-capture-devices" });
    expect(response.ok).toBe(true);
    if (response.ok && "devices" in response) {
      expect(response.devices.length).toBeGreaterThan(0);
    }
  });

  it("returns the media-core profile on handshake", async () => {
    const route = createIpcRouter({ mediaCore: fakeBackend() });
    const response = await route({ id: "4", type: "media-core-handshake" });
    expect(response.ok).toBe(true);
    if (response.ok && "profile" in response) {
      expect(response.profile.renderer).toBe("vulkan");
    }
  });

  it("reports media-core-unreachable when no profile is available", async () => {
    const noProfile: MediaCoreBackend = {
      getProfile: () => undefined,
      handshake: async () => undefined,
      syncMediaCore: async (commands, elapsedMs) => synthesizeSnapshot(commands, elapsedMs, 1),
      syncZoomMediaSpine: async (payload, elapsedMs) => synthesizeSpineSnapshot(payload, elapsedMs),
      joinZoom: async (payload) => synthesizeZoomJoinSnapshot(createSyntheticZoomCaptureState(), payload),
      leaveZoom: async () => synthesizeZoomLeaveSnapshot(createSyntheticZoomCaptureState()),
      getZoomSnapshot: async () => synthesizeZoomSnapshot(createSyntheticZoomCaptureState()),
      getHealth: () => ({ restartCount: 0, recovering: false, stopped: false }),
      ...captureBackendMethods(createCaptureBackendState())
    };
    const route = createIpcRouter({ mediaCore: noProfile });
    const response = await route({ id: "5", type: "media-core-handshake" });
    expect(response.ok).toBe(false);
    if (!response.ok) {
      expect(response.error.code).toBe("media-core-unreachable");
    }
  });

  it("round-trips a media-core sync batch to a snapshot", async () => {
    const route = createIpcRouter({ mediaCore: fakeBackend() });
    const response = await route({
      id: "6",
      type: "media-core-sync",
      payload: { commands: [{ type: "load-scene-graph", sceneId: "s", routes: [] }], elapsedMs: 10 }
    });
    expect(response.ok).toBe(true);
    if (response.ok && "snapshot" in response) {
      expect(response.snapshot.sceneId).toBe("s");
    }
  });

  it("mutates audio bus gain through the stub", async () => {
    const route = createIpcRouter({ mediaCore: fakeBackend() });
    const response = await route({ id: "7", type: "set-audio-bus-gain", payload: { busId: "program", gainDb: -6 } });
    expect(response.ok).toBe(true);
    if (response.ok && "buses" in response) {
      expect(response.buses.find((bus) => bus.busId === "program")?.gainDb).toBe(-6);
    }
  });

  it("pushes a caption cue through the stub", async () => {
    const route = createIpcRouter({ mediaCore: fakeBackend() });
    const response = await route({ id: "8", type: "push-caption-cue", payload: { text: "Hello", atMs: 5 } });
    expect(response.ok).toBe(true);
    if (response.ok && "track" in response) {
      expect(response.track.cues.at(-1)?.text).toBe("Hello");
    }
  });

  it("exports a support bundle to the diagnostics directory with crash events merged in", async () => {
    const diagnosticsDirectory = await mkdtemp(join(tmpdir(), "corevideo-router-diag-"));
    const route = createIpcRouter({
      mediaCore: fakeBackend(),
      diagnosticsDirectory,
      readCrashEvents: async () => [{ at: "2026-06-14T12:00:00.000Z", exitCode: 1, restartCount: 1 }]
    });
    const bundle = createSupportBundle(initialProduction);
    const response = await route({ id: "export-1", type: "export-support-bundle", payload: { bundle } });

    expect(response.ok).toBe(true);
    if (response.ok && "export" in response) {
      expect(response.export.bytes).toBeGreaterThan(0);
      const raw = await readFile(response.export.path, "utf8");
      const parsed = JSON.parse(raw) as { crashRecovery?: { events: Array<{ restartCount: number }> } };
      expect(parsed.crashRecovery?.events[0]?.restartCount).toBe(1);
    }
  });

  it("surfaces media-core failures as media-core-failed", async () => {
    const failing: MediaCoreBackend = {
      getProfile: () => SYNTHETIC_PROFILE,
      handshake: async () => SYNTHETIC_PROFILE,
      syncMediaCore: async () => {
        throw new Error("boom");
      },
      syncZoomMediaSpine: async (payload, elapsedMs) => synthesizeSpineSnapshot(payload, elapsedMs),
      joinZoom: async (payload) => synthesizeZoomJoinSnapshot(createSyntheticZoomCaptureState(), payload),
      leaveZoom: async () => synthesizeZoomLeaveSnapshot(createSyntheticZoomCaptureState()),
      getZoomSnapshot: async () => synthesizeZoomSnapshot(createSyntheticZoomCaptureState()),
      getHealth: () => ({ restartCount: 0, recovering: false, stopped: false }),
      ...captureBackendMethods(createCaptureBackendState())
    };
    const route = createIpcRouter({ mediaCore: failing });
    const response = await route({ id: "9", type: "media-core-sync", payload: { commands: [], elapsedMs: 0 } });
    expect(response.ok).toBe(false);
    if (!response.ok) {
      expect(response.error.code).toBe("media-core-failed");
    }
  });
});
