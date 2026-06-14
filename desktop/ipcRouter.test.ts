import { describe, expect, it } from "vitest";
import { createIpcRouter, type MediaCoreBackend } from "./ipcRouter.ts";
import { SYNTHETIC_PROFILE, synthesizeSnapshot, synthesizeSpineSnapshot } from "./syntheticMediaCore.ts";
import type { NativeMediaCoreProfile } from "../src/engine/nativeMediaCoreProtocol";

function fakeBackend(profile: NativeMediaCoreProfile | undefined = SYNTHETIC_PROFILE): MediaCoreBackend {
  return {
    getProfile: () => profile,
    handshake: async () => profile,
    syncMediaCore: async (commands, elapsedMs) => synthesizeSnapshot(commands, elapsedMs, 1),
    syncZoomMediaSpine: async (payload, elapsedMs) => synthesizeSpineSnapshot(payload, elapsedMs)
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
    const response = await route({ id: "1", type: "join", payload: { meetingNumber: "1", displayName: "Op" } as never });
    expect(response.ok).toBe(true);
    if (response.ok && "snapshot" in response) {
      expect(response.snapshot.meetingState).toBe("in_meeting");
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
      syncMediaCore: async (commands, elapsedMs) => synthesizeSnapshot(commands, elapsedMs, 1)
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

  it("surfaces media-core failures as media-core-failed", async () => {
    const failing: MediaCoreBackend = {
      getProfile: () => SYNTHETIC_PROFILE,
      handshake: async () => SYNTHETIC_PROFILE,
      syncMediaCore: async () => {
        throw new Error("boom");
      }
    };
    const route = createIpcRouter({ mediaCore: failing });
    const response = await route({ id: "9", type: "media-core-sync", payload: { commands: [], elapsedMs: 0 } });
    expect(response.ok).toBe(false);
    if (!response.ok) {
      expect(response.error.code).toBe("media-core-failed");
    }
  });
});
