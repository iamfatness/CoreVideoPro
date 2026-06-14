import { describe, expect, it } from "vitest";
import { MediaCoreRuntime } from "./mediaCore.js";
import type { MediaCoreCommand } from "./protocol.js";

const commands: MediaCoreCommand[] = [
  {
    type: "load-scene-graph",
    sceneId: "speaker-slides",
    routes: [
      { routeId: "speaker", mode: "fixed", participantId: "p2", audioRole: "isolated" },
      { routeId: "slides", mode: "screen-share", audioRole: "audience" }
    ]
  },
  {
    type: "set-participant-transform",
    participantId: "p2",
    crop: { x: 0.1, y: 0.1, width: 0.8, height: 0.8 },
    scale: 1.25,
    chromaKey: { enabled: true, color: "green", spillSuppression: 44 }
  },
  {
    type: "set-overlay-asset",
    overlayId: "brand-bug",
    text: "CoreVideo Pro",
    position: "top-right"
  },
  {
    type: "start-program-output",
    destinations: ["recording", "rtmp"],
    isoParticipantIds: ["p1", "p2"]
  }
];

describe("MediaCoreRuntime", () => {
  it("applies scene, transform, overlay, and output commands into backend state", () => {
    const runtime = new MediaCoreRuntime();
    const response = runtime.handle({ id: "sync-1", type: "sync", commands });

    expect(response).toMatchObject({
      id: "sync-1",
      ok: true,
      appliedCommandCount: 4,
      state: {
        sceneId: "speaker-slides",
        routeCount: 2,
        frameCount: 2,
        frames: [
          {
            sourceId: "participant:p2",
            participantId: "p2",
            kind: "participant-video",
            frameNumber: 1,
            width: 1280,
            height: 720
          },
          {
            sourceId: "screen-share:slides",
            kind: "screen-share",
            frameNumber: 1,
            width: 1920,
            height: 1080
          }
        ],
        participantTransformCount: 1,
        overlayCount: 1,
        outputs: ["recording", "rtmp"],
        isoParticipantIds: ["p1", "p2"],
        lastCommandTypes: [
          "load-scene-graph",
          "set-participant-transform",
          "set-overlay-asset",
          "start-program-output"
        ]
      }
    });
  });

  it("returns snapshots without requiring a renderer sync", () => {
    const runtime = new MediaCoreRuntime();

    expect(runtime.handle({ id: "snapshot-1", type: "snapshot" })).toMatchObject({
      id: "snapshot-1",
      ok: true,
      state: {
        routeCount: 0,
        frameCount: 0,
        frames: [],
        participantTransformCount: 0,
        overlayCount: 0,
        outputs: []
      }
    });
  });

  it("warns when output starts without destinations", () => {
    const runtime = new MediaCoreRuntime();
    const response = runtime.handle({
      id: "sync-2",
      type: "sync",
      commands: [{ type: "start-program-output", destinations: [], isoParticipantIds: [] }]
    });

    expect(response.ok).toBe(true);
    if (response.ok) {
      expect(response.state.warnings).toContain("Program output started without destinations.");
    }
  });

  it("advances fake backend frames with tick requests", () => {
    const runtime = new MediaCoreRuntime();
    runtime.handle({ id: "sync-1", type: "sync", commands });
    const response = runtime.handle({ id: "tick-1", type: "tick", elapsedMs: 33 });

    expect(response).toMatchObject({
      id: "tick-1",
      ok: true,
      appliedCommandCount: 0,
      state: {
        frameCount: 2,
        frames: [
          {
            sourceId: "participant:p2",
            frameNumber: 2,
            timestampMs: 33
          },
          {
            sourceId: "screen-share:slides",
            frameNumber: 2,
            timestampMs: 33
          }
        ]
      }
    });
  });
});
