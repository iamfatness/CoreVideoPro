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
    type: "set-output-profile",
    profileId: "1080p60",
    resolution: "1920x1080",
    width: 1920,
    height: 1080,
    fps: 60,
    targetBitrateMbps: 8.2
  },
  {
    type: "start-program-output",
    destinations: ["recording", "rtmp"],
    isoParticipantIds: ["p1", "p2"]
  },
  {
    type: "set-recording-targets",
    targetFolder: "Recordings/CoreVideo Pro/native-core",
    filenamePrefix: "program",
    format: "mp4",
    quality: "high",
    isoParticipantIds: ["p1", "p2"]
  },
  {
    type: "start-recording-session",
    sessionId: "test-session",
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
      appliedCommandCount: 7,
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
        outputProfile: {
          profileId: "1080p60",
          resolution: "1920x1080",
          width: 1920,
          height: 1080,
          fps: 60,
          targetBitrateMbps: 8.2
        },
        isoParticipantIds: ["p1", "p2"],
        recording: {
          sessionId: "test-session",
          active: true,
          status: "recording",
          writerStatus: "writing",
          programPath: "Recordings/CoreVideo Pro/native-core/program-program-0.mp4",
          streams: [
            { kind: "program", status: "writing", framesWritten: 2 },
            { kind: "iso", participantId: "p1", status: "writing", framesWritten: 0 },
            { kind: "iso", participantId: "p2", status: "writing", framesWritten: 1 }
          ],
          totalFramesWritten: 3
        },
        outputHealth: expect.arrayContaining([{ destination: "recording", status: "live", message: "Recording writer active.", droppedFrames: 0 }]),
        lastCommandTypes: [
          "load-scene-graph",
          "set-participant-transform",
          "set-overlay-asset",
          "set-output-profile",
          "start-program-output",
          "set-recording-targets",
          "start-recording-session"
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
        recording: undefined,
        outputHealth: [],
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
        ],
        recording: {
          elapsedMs: 33,
          streams: [
            { kind: "program", framesWritten: 4 },
            { kind: "iso", participantId: "p1", framesWritten: 0 },
            { kind: "iso", participantId: "p2", framesWritten: 2 }
          ],
          totalFramesWritten: 6
        }
      }
    });
  });

  it("clears recording when a sync no longer includes recording output", () => {
    const runtime = new MediaCoreRuntime();
    runtime.handle({ id: "sync-1", type: "sync", commands });
    const response = runtime.handle({
      id: "sync-2",
      type: "sync",
      commands: [
          {
            type: "load-scene-graph",
            sceneId: "interview",
            routes: [{ routeId: "active", mode: "active-speaker", audioRole: "mix" }]
        },
        { type: "stop-recording-session", reason: "Recording disabled in production state." }
      ]
    });

    expect(response).toMatchObject({
      ok: true,
      state: {
        outputs: [],
        isoParticipantIds: [],
        recording: {
          active: false,
          status: "stopped",
          writerStatus: "stopped"
        },
        outputHealth: [{ destination: "recording", status: "idle" }]
      }
    });
  });

  it("captures recording writer failures in output health and diagnostics", () => {
    const runtime = new MediaCoreRuntime();
    const response = runtime.handle({
      id: "sync-fail",
      type: "sync",
      commands: [
        {
          type: "start-recording-session",
          targetFolder: "Recordings/CoreVideo Pro/native-core",
          filenamePrefix: "failure-test",
          format: "mp4",
          quality: "high",
          isoParticipantIds: ["p1"]
        },
        { type: "fail-recording-session", message: "Encoder process exited." }
      ]
    });

    expect(response).toMatchObject({
      ok: true,
      state: {
        recording: {
          active: false,
          status: "failed",
          error: "Encoder process exited."
        },
        outputHealth: [{ destination: "recording", status: "failed", message: "Encoder process exited." }],
        diagnostics: {
          warnings: ["Encoder process exited."]
        }
      }
    });
  });
});
