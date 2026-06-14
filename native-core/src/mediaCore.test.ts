import { describe, expect, it } from "vitest";
import { MediaCoreRuntime } from "./mediaCore.js";
import type { MediaCoreCommand, MediaCoreFrame, MediaCoreFrameSourceSnapshot } from "./protocol.js";
import type { MediaCoreFrameSourceRequest, MediaCoreFrameSourceResult, MediaFrameSource } from "./mediaSource.js";

const commands: MediaCoreCommand[] = [
  {
    type: "set-zoom-source-roster",
    sources: [
      {
        sourceId: "participant:p1",
        participantId: "p1",
        displayName: "Maya Chen",
        role: "Host",
        breakoutRoomId: "main",
        breakoutRoomName: "Main room",
        hasVideo: true,
        hasAudio: true,
        isMuted: false,
        isActiveSpeaker: false,
        isScreenSharing: false,
        audioLevel: 64,
        health: "live"
      },
      {
        sourceId: "participant:p2",
        participantId: "p2",
        displayName: "Andre Wallace",
        role: "Presenter",
        breakoutRoomId: "main",
        breakoutRoomName: "Main room",
        hasVideo: true,
        hasAudio: true,
        isMuted: false,
        isActiveSpeaker: true,
        isScreenSharing: true,
        audioLevel: 82,
        health: "live"
      }
    ]
  },
  {
    type: "set-active-speaker",
    participantId: "p2"
  },
  {
    type: "set-screen-share-source",
    participantId: "p2"
  },
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
    type: "set-color-grade",
    lut: "warm-film",
    exposure: 2,
    contrast: 6,
    saturation: 8,
    temperature: 5
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
      appliedCommandCount: 11,
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
            sourceId: "screen-share:p2",
            kind: "screen-share",
            frameNumber: 1,
            width: 1920,
            height: 1080
          }
        ],
        sourceSnapshot: {
          adapterId: "test-pattern",
          kind: "test-pattern",
          status: "subscribed",
          subscribedSourceCount: 2,
          liveFrameCount: 2
        },
        participantTransformCount: 1,
        overlayCount: 1,
        sourceCount: 2,
        resolvedRouteCount: 2,
        renderPlan: {
          sceneId: "speaker-slides",
          colorGrade: { lut: "warm-film" },
          layers: [
            { layerId: "route:speaker", sourceId: "participant:p2", participantId: "p2", kind: "participant-video" },
            { layerId: "route:slides", sourceId: "screen-share:p2", participantId: "p2", kind: "screen-share" },
            { layerId: "overlay:brand-bug", kind: "overlay", overlayId: "brand-bug" }
          ]
        },
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
            { kind: "program", status: "writing", framesWritten: 1 },
            { kind: "iso", participantId: "p1", status: "writing", framesWritten: 0 },
            { kind: "iso", participantId: "p2", status: "writing", framesWritten: 1 }
          ],
          totalFramesWritten: 2
        },
        programFrame: {
          frameNumber: 1,
          renderPlanId: expect.stringMatching(/^rp-/),
          health: "live",
          layerCount: 3
        },
        programFrameCount: 1,
        programTransport: {
          transportId: "in-process-preview",
          status: "publishing",
          frameNumber: 1,
          latencyMs: 0
        },
        compositor: {
          status: "live",
          programFrameCount: 1,
          lastReconfigureReason: expect.stringContaining("Initial render plan")
        },
        encoderSession: {
          status: "encoding",
          lifecycle: {
            status: "encoding",
            lastTransition: "Program output encoder session started."
          },
          targets: expect.arrayContaining([
            expect.objectContaining({ targetId: "recording:program", streamKind: "program", status: "attached" }),
            expect.objectContaining({ targetId: "rtmp:program", streamKind: "program", status: "attached" }),
            expect.objectContaining({ targetId: "recording:iso:p1", streamKind: "iso", status: "attached" }),
            expect.objectContaining({ targetId: "recording:iso:p2", streamKind: "iso", status: "attached" })
          ])
        },
        outputSenderSession: {
          status: "live",
          activeSenderCount: 1,
          senders: [
            {
              senderId: "rtmp:program",
              destination: "rtmp",
              status: "live",
              framesSent: 1,
              latencyMs: 2100,
              bitrateMbps: 8.2
            }
          ]
        },
        outputHealth: expect.arrayContaining([{ destination: "recording", status: "live", message: "Recording writer active.", droppedFrames: 0 }]),
        diagnostics: {
          outputSenderSession: {
            status: "live",
            activeSenderCount: 1
          },
          sourceSnapshot: {
            adapterId: "test-pattern",
            status: "subscribed",
            subscribedSourceCount: 2
          },
          programTransport: {
            status: "publishing",
            frameNumber: 1
          },
          encoderSession: {
            lifecycle: {
              status: "encoding"
            },
            targets: expect.arrayContaining([expect.objectContaining({ targetId: "rtmp:program" })])
          },
          recording: {
            active: true,
            totalFramesWritten: 2
          }
        },
        lastCommandTypes: [
          "set-zoom-source-roster",
          "set-active-speaker",
          "set-screen-share-source",
          "load-scene-graph",
          "set-participant-transform",
          "set-overlay-asset",
          "set-color-grade",
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
            sourceId: "screen-share:p2",
            frameNumber: 2,
            timestampMs: 33
          }
        ],
        recording: {
          elapsedMs: 33,
          streams: [
            { kind: "program", framesWritten: 2 },
            { kind: "iso", participantId: "p1", framesWritten: 0 },
            { kind: "iso", participantId: "p2", framesWritten: 2 }
          ],
          totalFramesWritten: 4
        }
      }
    });
  });

  it("accepts injectable media source adapters for future Zoom SDK or local camera frames", () => {
    class LocalCameraSource implements MediaFrameSource {
      readonly adapterId = "local-camera:a";
      readonly kind = "local-camera" as const;
      private frameNumber = 0;
      private lastSnapshot: MediaCoreFrameSourceSnapshot = {
        adapterId: this.adapterId,
        kind: this.kind,
        status: "idle",
        subscribedSourceCount: 0,
        liveFrameCount: 0,
        staleFrameCount: 0,
        droppedFrameCount: 0,
        lowResolutionFrameCount: 0,
        warnings: []
      };

      render(sources: MediaCoreFrameSourceRequest[], elapsedMs: number): MediaCoreFrameSourceResult {
        this.frameNumber += 1;
        const frames: MediaCoreFrame[] = sources.map((source) => ({
          sourceId: source.sourceId,
          participantId: source.participantId,
          kind: source.kind,
          frameNumber: this.frameNumber,
          timestampMs: elapsedMs,
          width: 3840,
          height: 2160,
          fps: 60,
          health: "live"
        }));
        this.lastSnapshot = {
          adapterId: this.adapterId,
          kind: this.kind,
          status: frames.length ? "subscribed" : "idle",
          subscribedSourceCount: frames.length,
          liveFrameCount: frames.length,
          staleFrameCount: 0,
          droppedFrameCount: 0,
          lowResolutionFrameCount: 0,
          lastFrameTimestampMs: frames.length ? elapsedMs : undefined,
          warnings: []
        };
        return { frames, snapshot: this.lastSnapshot };
      }

      snapshot(): MediaCoreFrameSourceSnapshot {
        return this.lastSnapshot;
      }
    }

    const runtime = new MediaCoreRuntime(new LocalCameraSource());
    const response = runtime.handle({
      id: "local-camera",
      type: "sync",
      commands: [
        commands[0],
        {
          type: "load-scene-graph",
          sceneId: "camera-scene",
          routes: [{ routeId: "host", mode: "fixed", participantId: "p1", audioRole: "isolated" }]
        }
      ]
    });

    expect(response).toMatchObject({
      ok: true,
      state: {
        frames: [{ sourceId: "participant:p1", width: 3840, height: 2160, fps: 60 }],
        sourceSnapshot: {
          adapterId: "local-camera:a",
          kind: "local-camera",
          status: "subscribed",
          subscribedSourceCount: 1
        },
        programTransport: {
          status: "publishing",
          frameNumber: 1
        }
      }
    });
  });

  it("switches media source adapters through the runtime command protocol", () => {
    const runtime = new MediaCoreRuntime();
    const response = runtime.handle({
      id: "source-adapter",
      type: "sync",
      commands: [
        { type: "set-media-source-adapter", kind: "local-camera", adapterId: "camera-a" },
        commands[0],
        {
          type: "load-scene-graph",
          sceneId: "camera-scene",
          routes: [{ routeId: "host", mode: "fixed", participantId: "p1", audioRole: "isolated" }]
        }
      ]
    });

    expect(response).toMatchObject({
      ok: true,
      appliedCommandCount: 3,
      state: {
        frames: [{ sourceId: "participant:p1", width: 1920, height: 1080, health: "live" }],
        sourceSnapshot: {
          adapterId: "camera-a",
          kind: "local-camera",
          status: "subscribed",
          subscribedSourceCount: 1
        },
        diagnostics: {
          sourceSnapshot: {
            adapterId: "camera-a",
            kind: "local-camera"
          }
        }
      }
    });
  });

  it("exposes encoder lifecycle stops as output health warnings", () => {
    const runtime = new MediaCoreRuntime();
    const response = runtime.handle({
      id: "encoder-stop",
      type: "sync",
      commands: [
        ...commands,
        {
          type: "stop-encoder-session",
          reason: "Operator stopped encoder during rehearsal."
        }
      ]
    });

    expect(response).toMatchObject({
      ok: true,
      state: {
        encoderSession: {
          status: "idle",
          lifecycle: {
            status: "stopped",
            lastTransition: "Operator stopped encoder during rehearsal."
          }
        },
      }
    });
    if (response.ok) {
      expect(response.state.outputHealth).toEqual(
        expect.arrayContaining([
          expect.objectContaining({
            destination: "rtmp",
            status: "warning",
            message: "Operator stopped encoder during rehearsal."
          }),
          expect.objectContaining({
            destination: "recording",
            status: "warning",
            message: "Operator stopped encoder during rehearsal."
          })
        ])
      );
    }
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

  it("recovers failed recording writers and failed output senders through commands", () => {
    const runtime = new MediaCoreRuntime();
    runtime.handle({ id: "sync-1", type: "sync", commands });

    const failed = runtime.handle({
      id: "fail-runtime",
      type: "sync",
      commands: [
        { type: "start-program-output", destinations: ["recording", "rtmp"], isoParticipantIds: ["p1", "p2"] },
        { type: "fail-output-sender", destination: "rtmp", message: "RTMP connection refused." },
        { type: "fail-recording-session", message: "Recording writer crashed." }
      ]
    });

    expect(failed).toMatchObject({
      ok: true,
      state: {
        outputSenderSession: {
          status: "failed",
          senders: [{ destination: "rtmp", status: "failed", warning: "RTMP connection refused." }]
        },
        recording: {
          active: false,
          status: "failed",
          error: "Recording writer crashed."
        },
        operatorActions: [
          {
            actionId: "recording:recover",
            severity: "critical",
            title: "Recover recording writer",
            command: "recover-recording-session"
          },
          {
            actionId: "sender:rtmp:recover",
            severity: "critical",
            title: "Recover RTMP sender",
            command: "recover-output-sender:rtmp"
          }
        ],
        diagnostics: {
          operatorActions: expect.arrayContaining([
            expect.objectContaining({ actionId: "recording:recover" }),
            expect.objectContaining({ actionId: "sender:rtmp:recover" })
          ])
        },
        outputHealth: expect.arrayContaining([
          expect.objectContaining({ destination: "rtmp", status: "failed", message: "RTMP connection refused." }),
          expect.objectContaining({ destination: "recording", status: "failed", message: "Recording writer crashed." })
        ]),
        warnings: expect.arrayContaining(["RTMP connection refused.", "Recording writer crashed."])
      }
    });

    const recovered = runtime.handle({
      id: "recover-runtime",
      type: "sync",
      commands: [
        { type: "start-program-output", destinations: ["recording", "rtmp"], isoParticipantIds: ["p1", "p2"] },
        { type: "recover-output-sender", destination: "rtmp", reason: "RTMP sender recovered after reconnect." },
        { type: "recover-recording-session", reason: "Recording writer recovered." }
      ]
    });

    expect(recovered).toMatchObject({
      ok: true,
      state: {
        outputSenderSession: {
          status: "live",
          senders: [{ destination: "rtmp", status: "live", warning: undefined }]
        },
        recording: {
          active: true,
          status: "recording",
          writerStatus: "writing",
          error: undefined
        },
        operatorActions: [],
        outputHealth: expect.arrayContaining([
          expect.objectContaining({ destination: "rtmp", status: "live" }),
          expect.objectContaining({ destination: "recording", status: "live" })
        ])
      }
    });
  });

  it("resolves active speaker and screen share routes from the Zoom source roster", () => {
    const runtime = new MediaCoreRuntime();
    const response = runtime.handle({
      id: "sync-routes",
      type: "sync",
      commands: [
        commands[0],
        { type: "set-active-speaker", participantId: "p2" },
        { type: "set-screen-share-source", participantId: "p2" },
        {
          type: "load-scene-graph",
          sceneId: "panel",
          routes: [
            { routeId: "active", mode: "active-speaker", audioRole: "mix" },
            { routeId: "screen", mode: "screen-share", audioRole: "audience" }
          ]
        }
      ]
    });

    expect(response).toMatchObject({
      ok: true,
      state: {
        sourceCount: 2,
        resolvedRouteCount: 2,
        renderPlan: {
          routes: [
            { routeId: "active", mode: "active-speaker", status: "resolved", sourceId: "participant:p2" },
            { routeId: "screen", mode: "screen-share", status: "resolved", sourceId: "screen-share:p2" }
          ],
          layers: [
            { layerId: "route:active", kind: "participant-video", sourceId: "participant:p2" },
            { layerId: "route:screen", kind: "screen-share", sourceId: "screen-share:p2" }
          ]
        }
      }
    });
  });

  it("warns for unavailable participants, missing screen share, muted isolated audio, and duplicate video assignments", () => {
    const runtime = new MediaCoreRuntime();
    const response = runtime.handle({
      id: "sync-warnings",
      type: "sync",
      commands: [
        {
          type: "set-zoom-source-roster",
          sources: [
            {
              sourceId: "participant:p1",
              participantId: "p1",
              displayName: "Maya Chen",
              role: "Host",
              breakoutRoomId: "main",
              breakoutRoomName: "Main room",
              hasVideo: true,
              hasAudio: true,
              isMuted: true,
              isActiveSpeaker: false,
              isScreenSharing: false,
              audioLevel: 0,
              health: "live"
            },
            {
              sourceId: "participant:p2",
              participantId: "p2",
              displayName: "Andre Wallace",
              role: "Presenter",
              breakoutRoomId: "main",
              breakoutRoomName: "Main room",
              hasVideo: false,
              hasAudio: true,
              isMuted: false,
              isActiveSpeaker: false,
              isScreenSharing: false,
              audioLevel: 40,
              health: "video-off"
            }
          ]
        },
        {
          type: "load-scene-graph",
          sceneId: "problem-scene",
          routes: [
            { routeId: "muted", mode: "fixed", participantId: "p1", audioRole: "isolated" },
            { routeId: "dupe", mode: "fixed", participantId: "p1", audioRole: "mix" },
            { routeId: "video-off", mode: "fixed", participantId: "p2", audioRole: "mix" },
            { routeId: "missing", mode: "fixed", participantId: "p9", audioRole: "mix" },
            { routeId: "screen", mode: "screen-share", audioRole: "audience" }
          ]
        }
      ]
    });

    expect(response).toMatchObject({
      ok: true,
      state: {
        resolvedRouteCount: 2,
        renderPlan: {
          routes: [
            { routeId: "muted", status: "resolved", warning: "Maya Chen is muted but assigned isolated audio." },
            { routeId: "dupe", status: "resolved", warning: "p1 is assigned to multiple program routes." },
            { routeId: "video-off", status: "missing", warning: "Andre Wallace has no clean video feed." },
            { routeId: "missing", status: "missing", warning: "p9 is not present in the Zoom source roster." },
            { routeId: "screen", status: "missing", warning: "Screen share route requested but no active screen share source is available." }
          ]
        },
        warnings: expect.arrayContaining([
          "Maya Chen is muted but assigned isolated audio.",
          "p1 is assigned to multiple program routes.",
          "Andre Wallace has no clean video feed.",
          "p9 is not present in the Zoom source roster.",
          "Screen share route requested but no active screen share source is available."
        ])
      }
    });
  });
});
