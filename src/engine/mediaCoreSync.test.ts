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
      participantTransformCount: 8,
      overlayCount: 1,
      sourceCount: 8,
      resolvedRouteCount: 2,
      outputs: [],
      isoParticipantIds: [],
      renderPlan: {
        sceneId: "speaker-slides",
        sourceCount: 8,
        resolvedRouteCount: 2,
        colorGrade: { lut: "none" },
        layers: [
          { layerId: "route:speaker-slides-1", kind: "screen-share", sourceId: "screen-share:p2", participantId: "p2" },
          { layerId: "route:speaker-slides-2", kind: "participant-video", sourceId: "participant:p2", participantId: "p2" },
          { layerId: "overlay:brand-bug", kind: "overlay", overlayId: "brand-bug" }
        ]
      },
      frames: [
        {
          sourceId: "screen-share:p2",
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
      outputProfile: {
        profileId: "1080p60",
        resolution: "1920x1080",
        width: 1920,
        height: 1080,
        fps: 60,
        targetBitrateMbps: 8.2
      },
      outputHealth: [],
      sourceCount: 0,
      resolvedRouteCount: 0,
      renderPlan: {
        outputProfile: {
          profileId: "1080p60",
          resolution: "1920x1080",
          width: 1920,
          height: 1080,
          fps: 60,
          targetBitrateMbps: 8.2
        },
        colorGrade: { lut: "none" as const, exposure: 0, contrast: 0, saturation: 0, temperature: 0 },
        sourceCount: 0,
        resolvedRouteCount: 0,
        layers: [],
        routes: [],
        warnings: []
      },
      diagnostics: {
        generatedAtMs: 2400,
        routeCount: 1,
        frameCount: 1,
        outputs: ["recording" as const],
        outputProfile: {
          profileId: "1080p60",
          resolution: "1920x1080",
          width: 1920,
          height: 1080,
          fps: 60,
          targetBitrateMbps: 8.2
        },
        outputHealth: [],
        renderPlan: {
          outputProfile: {
            profileId: "1080p60",
            resolution: "1920x1080",
            width: 1920,
            height: 1080,
            fps: 60,
            targetBitrateMbps: 8.2
          },
          colorGrade: { lut: "none" as const, exposure: 0, contrast: 0, saturation: 0, temperature: 0 },
          sourceCount: 0,
          resolvedRouteCount: 0,
          layers: [],
          routes: [],
          warnings: []
        },
        warnings: [],
        lastCommandTypes: ["load-scene-graph"]
      },
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

  it("creates recording session metadata when production is recording", async () => {
    const engine = new InMemoryMediaCoreSyncEngine();
    const snapshot = await engine.syncProduction({ ...initialProduction, recording: true }, 3000);

    expect(snapshot).toMatchObject({
      outputs: ["recording"],
      isoParticipantIds: ["p1", "p2"],
      outputProfile: {
        profileId: "1080p60",
        resolution: "1920x1080",
        width: 1920,
        height: 1080,
        fps: 60,
        targetBitrateMbps: 8.2
      },
      sourceCount: 8,
      resolvedRouteCount: 2,
      renderPlan: {
        routes: [
          { routeId: "speaker-slides-1", status: "resolved", kind: "screen-share", sourceId: "screen-share:p2" },
          { routeId: "speaker-slides-2", status: "resolved", kind: "participant-video", sourceId: "participant:p2" }
        ]
      },
      recording: {
        active: true,
        status: "recording",
        startedAtMs: 3000,
        writerStatus: "writing",
        estimatedDiskRateMBps: 7.49,
        programPath: "Recordings/CoreVideo Pro/AI_Product_Launch_Webinar-program-3000.mp4",
        streams: [
          { kind: "program", status: "writing", framesWritten: 2 },
          { kind: "iso", participantId: "p1", status: "writing", framesWritten: 0 },
          { kind: "iso", participantId: "p2", status: "writing", framesWritten: 1 }
        ],
        totalFramesWritten: 3
      },
      outputHealth: [{ destination: "recording", status: "live", message: "Recording writer active." }],
      diagnostics: {
        generatedAtMs: 3000,
        recording: { sessionId: "AI_Product_Launch_Webinar-p1-p2" }
      }
    });
  });

  it("surfaces render plan warnings when a scene asks for unavailable screen share", async () => {
    const engine = new InMemoryMediaCoreSyncEngine();
    const snapshot = await engine.syncProduction(
      {
        ...initialProduction,
        participants: initialProduction.participants.map((participant) => ({ ...participant, isScreenSharing: false }))
      },
      3200
    );

    expect(snapshot).toMatchObject({
      sceneId: "speaker-slides",
      resolvedRouteCount: 1,
      renderPlan: {
        routes: [
          {
            routeId: "speaker-slides-1",
            status: "missing",
            warning: "Screen share route requested but no active screen share source is available."
          },
          {
            routeId: "speaker-slides-2",
            status: "resolved",
            sourceId: "participant:p2"
          }
        ]
      },
      warnings: ["Screen share route requested but no active screen share source is available."]
    });
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
