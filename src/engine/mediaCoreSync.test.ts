import { describe, expect, it, vi } from "vitest";
import { initialProduction } from "../domain/production";
import { InMemoryMediaCoreSyncEngine, NativeHostMediaCoreSyncEngine } from "./mediaCoreSync";
import type { NativeHostBridge } from "./nativeHostBridge";

describe("media core sync engine", () => {
  it("syncs production state into a backend-style media snapshot", async () => {
    const engine = new InMemoryMediaCoreSyncEngine();
    const snapshot = await engine.syncProduction(initialProduction, 1200);

    expect(snapshot).toMatchObject({
      sceneId: "speaker-slides",
      routeCount: 2,
      frameCount: 2,
      participantTransformCount: 4,
      overlayCount: 1,
      outputs: [],
      isoParticipantIds: [],
      frames: [
        {
          sourceId: "screen-share:speaker-slides-1",
          kind: "screen-share",
          timestampMs: 1200,
          width: 1920,
          height: 1080
        },
        {
          sourceId: "participant:p2",
          participantId: "p2",
          kind: "participant-video",
          timestampMs: 1200
        }
      ]
    });
  });

  it("forwards sync commands to a native host bridge when available", async () => {
    const syncMediaCore = vi.fn(async () => ({
      sceneId: "native-scene",
      routeCount: 1,
      frameCount: 1,
      frames: [],
      participantTransformCount: 0,
      overlayCount: 0,
      outputs: ["recording" as const],
      isoParticipantIds: ["p1"],
      lastCommandTypes: ["load-scene-graph"],
      warnings: []
    }));
    const bridge: NativeHostBridge = {
      host: "test-host",
      platform: "win32",
      async request(command) {
        return {
          id: command.id,
          ok: false,
          error: {
            code: "native-unavailable",
            message: "unused"
          }
        };
      },
      syncMediaCore
    };
    const engine = new NativeHostMediaCoreSyncEngine(bridge);

    await expect(engine.syncProduction(initialProduction, 2400)).resolves.toMatchObject({
      sceneId: "native-scene",
      outputs: ["recording"]
    });
    expect(syncMediaCore).toHaveBeenCalledWith(expect.arrayContaining([expect.objectContaining({ type: "load-scene-graph" })]), 2400);
  });

  it("falls back with a warning when a native host has no media-core bridge", async () => {
    const bridge: NativeHostBridge = {
      host: "test-host",
      platform: "darwin",
      async request(command) {
        return {
          id: command.id,
          ok: false,
          error: {
            code: "native-unavailable",
            message: "unused"
          }
        };
      }
    };
    const engine = new NativeHostMediaCoreSyncEngine(bridge);

    await expect(engine.syncProduction(initialProduction, 10)).resolves.toMatchObject({
      sceneId: "speaker-slides",
      warnings: ["Native host has no media-core sync bridge; using renderer-side simulation."]
    });
  });
});
