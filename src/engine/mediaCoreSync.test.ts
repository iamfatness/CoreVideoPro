import { describe, expect, it, vi } from "vitest";
import { initialProduction } from "../domain/production";
import { InMemoryMediaCoreSyncEngine, NativeHostMediaCoreSyncEngine } from "./mediaCoreSync";
import type { NativeHostBridge } from "./nativeHostBridge";
import type { NativeMediaCoreCommand } from "./nativeMediaCoreProtocol";

describe("media core sync engine", () => {
  class TestMediaCoreSyncEngine extends InMemoryMediaCoreSyncEngine {
    runCommands(commands: NativeMediaCoreCommand[], elapsedMs: number) {
      return this.snapshot(commands, elapsedMs);
    }
  }

  it("syncs production state into a backend-style media snapshot", async () => {
    const engine = new InMemoryMediaCoreSyncEngine();
    const snapshot = await engine.syncProduction(initialProduction, 1200);

    expect(snapshot).toMatchObject({
      sceneId: "speaker-slides",
      routeCount: 2,
      frameCount: 2,
      sourceSnapshot: {
        adapterId: "renderer-test-pattern",
        kind: "test-pattern",
        status: "subscribed",
        subscribedSourceCount: 2,
        liveFrameCount: 2
      },
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
      ],
      programTransport: {
        transportId: "in-process-preview",
        status: "publishing",
        frameNumber: 1,
        latencyMs: 0
      }
    });
  });

  it("creates source actions and events only for routed Zoom feed health issues", () => {
    const engine = new TestMediaCoreSyncEngine();
    const roster = [
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
        health: "low-resolution" as const
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
        isActiveSpeaker: false,
        isScreenSharing: false,
        audioLevel: 82,
        health: "live" as const
      }
    ];

    const unrouted = engine.runCommands(
      [
        { type: "set-zoom-source-roster", sources: roster },
        {
          type: "load-scene-graph",
          sceneId: "single",
          routes: [{ routeId: "guest", mode: "fixed", participantId: "p2", audioRole: "mix" }]
        }
      ],
      1000
    );

    expect(unrouted.sourceSnapshot.issues).toEqual([]);
    expect(unrouted.operatorActions.find((action) => action.actionId.startsWith("source:"))).toBeUndefined();

    const routed = engine.runCommands(
      [
        { type: "set-zoom-source-roster", sources: roster },
        {
          type: "load-scene-graph",
          sceneId: "single",
          routes: [{ routeId: "guest", mode: "fixed", participantId: "p1", audioRole: "mix" }]
        }
      ],
      1500
    );

    expect(routed.sourceSnapshot).toMatchObject({
      status: "degraded",
      issues: [
        {
          sourceId: "participant:p1",
          participantId: "p1",
          displayName: "Maya Chen",
          health: "low-resolution",
          severity: "warning",
          detail: "Maya Chen feed is below target resolution."
        }
      ]
    });
    expect(routed.operatorActions).toEqual(
      expect.arrayContaining([
        expect.objectContaining({
          actionId: "source:p1:check",
          area: "source",
          title: "Check Maya Chen feed",
          detail: "Maya Chen feed is below target resolution."
        })
      ])
    );
    expect(routed.eventLog).toEqual(
      expect.arrayContaining([
        expect.objectContaining({
          area: "source",
          title: "Zoom feed quality warning",
          detail: "Maya Chen feed is below target resolution.",
          relatedId: "p1"
        })
      ])
    );

    const recovered = engine.runCommands(
      [
        {
          type: "set-zoom-source-roster",
          sources: roster.map((source) => (source.participantId === "p1" ? { ...source, health: "live" as const } : source))
        },
        {
          type: "load-scene-graph",
          sceneId: "single",
          routes: [{ routeId: "guest", mode: "fixed", participantId: "p1", audioRole: "mix" }]
        }
      ],
      2000
    );

    expect(recovered.sourceSnapshot.issues).toEqual([]);
    expect(recovered.eventLog).toEqual(
      expect.arrayContaining([
        expect.objectContaining({
          severity: "info",
          area: "source",
          title: "Zoom feed recovered",
          detail: "Maya Chen feed recovered.",
          relatedId: "p1"
        })
      ])
    );
  });

  it("forwards sync commands to a native host bridge when available", async () => {
    const syncMediaCore = vi.fn(async () => ({
      sceneId: "native-scene",
      routeCount: 1,
      frameCount: 1,
      frames: [],
      sourceSnapshot: {
        adapterId: "native-test-source",
        kind: "zoom-sdk" as const,
        status: "idle" as const,
        subscribedSourceCount: 0,
        liveFrameCount: 0,
        staleFrameCount: 0,
        droppedFrameCount: 0,
        lowResolutionFrameCount: 0,
        warnings: []
      },
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
      outputSenderSession: {
        status: "idle" as const,
        activeSenderCount: 0,
        senders: [],
        warnings: []
      },
      operatorActions: [],
      eventLog: [],
      programFrameCount: 0,
      programTransport: {
        transportId: "in-process-preview" as const,
        status: "idle" as const,
        latencyMs: 0
      },
      compositor: {
        status: "idle" as const,
        programFrameCount: 0,
        droppedFrameCount: 0,
        degradedFrameCount: 0
      },
      encoderSession: {
        status: "idle" as const,
        programFrameCount: 0,
        targets: [],
        lifecycle: {
          status: "idle" as const,
          lastTransition: "Encoder session idle."
        },
        warnings: []
      },
      sourceCount: 0,
      resolvedRouteCount: 0,
      renderPlan: {
        renderPlanId: "rp-test",
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
        outputSenderSession: {
          status: "idle" as const,
          activeSenderCount: 0,
          senders: [],
          warnings: []
        },
        sourceSnapshot: {
          adapterId: "native-test-source",
          kind: "zoom-sdk" as const,
          status: "idle" as const,
          subscribedSourceCount: 0,
          liveFrameCount: 0,
          staleFrameCount: 0,
          droppedFrameCount: 0,
          lowResolutionFrameCount: 0,
          warnings: []
        },
        renderPlan: {
          renderPlanId: "rp-test",
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
        programFrameCount: 0,
        compositor: {
          status: "idle" as const,
          programFrameCount: 0,
          droppedFrameCount: 0,
          degradedFrameCount: 0
        },
        programTransport: {
          transportId: "in-process-preview" as const,
          status: "idle" as const,
          latencyMs: 0
        },
        encoderSession: {
          status: "idle" as const,
          programFrameCount: 0,
          targets: [],
          lifecycle: {
            status: "idle" as const,
            lastTransition: "Encoder session idle."
          },
          warnings: []
        },
        operatorActions: [],
        eventLog: [],
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

  it("switches renderer fallback source adapters through media-core commands", () => {
    const engine = new TestMediaCoreSyncEngine();
    const snapshot = engine.runCommands(
      [
        { type: "set-media-source-adapter", kind: "local-camera", adapterId: "camera-a" },
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
              isActiveSpeaker: true,
              isScreenSharing: false,
              audioLevel: 64,
              health: "live"
            }
          ]
        },
        {
          type: "load-scene-graph",
          sceneId: "camera",
          routes: [{ routeId: "host", mode: "fixed", participantId: "p1", audioRole: "isolated" }]
        }
      ],
      1500
    );

    expect(snapshot).toMatchObject({
      sourceSnapshot: {
        adapterId: "camera-a",
        kind: "local-camera",
        status: "subscribed"
      },
      frames: [{ sourceId: "participant:p1", width: 1920, height: 1080, health: "live" }],
      diagnostics: {
        sourceSnapshot: {
          adapterId: "camera-a",
          kind: "local-camera"
        }
      }
    });
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
      frameCount: 3,
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
        warning: undefined,
        estimatedDiskRateMBps: 7.49,
        programPath: "Recordings/CoreVideo Pro/AI_Product_Launch_Webinar-program-3000.mp4",
        streams: [
          { kind: "program", status: "writing", expectedFrames: 1, framesWritten: 1, missingFrames: 0 },
          { kind: "iso", participantId: "p1", status: "writing", expectedFrames: 1, framesWritten: 1, missingFrames: 0, warning: undefined },
          { kind: "iso", participantId: "p2", status: "writing", expectedFrames: 1, framesWritten: 1, missingFrames: 0 }
        ],
        totalFramesWritten: 3
      },
      programFrame: {
        frameNumber: 1,
        renderPlanId: expect.stringMatching(/^rp-/),
        health: "live",
        layerCount: 3
      },
      programFrameCount: 1,
      programTransport: {
        status: "publishing",
        frameNumber: 1
      },
      compositor: {
        status: "live",
        programFrameCount: 1
      },
      encoderSession: {
        status: "encoding",
        lifecycle: {
          status: "encoding",
          lastTransition: "Encoder session started."
        },
        targets: expect.arrayContaining([
          expect.objectContaining({ targetId: "recording:program", streamKind: "program", status: "attached" }),
          expect.objectContaining({ targetId: "recording:iso:p1", streamKind: "iso", status: "attached", warning: undefined }),
          expect.objectContaining({ targetId: "recording:iso:p2", streamKind: "iso", status: "attached" })
        ])
      },
      outputSenderSession: {
        status: "idle",
        activeSenderCount: 0,
        senders: []
      },
      outputHealth: [{ destination: "recording", status: "live", message: "Recording writer active." }],
      operatorActions: [],
      eventLog: [],
      diagnostics: {
        generatedAtMs: 3000,
        outputSenderSession: {
          status: "idle",
          activeSenderCount: 0
        },
        sourceSnapshot: {
          adapterId: "renderer-test-pattern",
          status: "subscribed",
          subscribedSourceCount: 3
        },
        programTransport: {
          status: "publishing"
        },
        recording: { sessionId: "AI_Product_Launch_Webinar-p1-p2" }
      }
    });
  });

  it("surfaces selected ISO participants that are not recordable from the Zoom roster", () => {
    const engine = new TestMediaCoreSyncEngine();
    const snapshot = engine.runCommands(
      [
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
              isActiveSpeaker: true,
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
              isActiveSpeaker: false,
              isScreenSharing: false,
              audioLevel: 82,
              health: "video-off"
            },
            {
              sourceId: "participant:p3",
              participantId: "p3",
              displayName: "Priya Shah",
              role: "Panelist",
              breakoutRoomId: "main",
              breakoutRoomName: "Main room",
              hasVideo: false,
              hasAudio: true,
              isMuted: false,
              isActiveSpeaker: false,
              isScreenSharing: false,
              audioLevel: 40,
              health: "live"
            }
          ]
        },
        {
          type: "load-scene-graph",
          sceneId: "host",
          routes: [{ routeId: "host", mode: "fixed", participantId: "p1", audioRole: "isolated" }]
        },
        { type: "start-program-output", destinations: ["recording"], isoParticipantIds: ["p2", "p3", "p9"] },
        {
          type: "start-recording-session",
          sessionId: "iso-readiness",
          targetFolder: "Recordings/CoreVideo Pro",
          filenamePrefix: "ISO_Readiness",
          format: "mp4",
          quality: "high",
          isoParticipantIds: ["p2", "p3", "p9"]
        }
      ],
      5100
    );

    expect(snapshot).toMatchObject({
      frameCount: 1,
      recording: {
        status: "warning",
        writerStatus: "warning",
        warning: "Andre Wallace ISO video is off.",
        streams: expect.arrayContaining([
          expect.objectContaining({ kind: "iso", participantId: "p2", readiness: "video-off", status: "warning", warning: "Andre Wallace ISO video is off." }),
          expect.objectContaining({
            kind: "iso",
            participantId: "p3",
            readiness: "unsubscribable",
            status: "warning",
            warning: "Priya Shah ISO video cannot be subscribed by the Zoom SDK."
          }),
          expect.objectContaining({
            kind: "iso",
            participantId: "p9",
            readiness: "missing",
            status: "warning",
            warning: "p9 ISO participant is not present in the Zoom roster."
          })
        ])
      },
      encoderSession: {
        status: "warning",
        targets: expect.arrayContaining([
          expect.objectContaining({ targetId: "recording:iso:p2", status: "warning", warning: "Andre Wallace ISO video is off." })
        ])
      },
      outputHealth: [expect.objectContaining({ destination: "recording", status: "warning", message: "Andre Wallace ISO video is off." })],
      operatorActions: expect.arrayContaining([
        expect.objectContaining({
          actionId: "recording:iso:p2:check",
          area: "recording",
          title: "Check p2 ISO recording",
          detail: "Andre Wallace ISO video is off."
        })
      ]),
      eventLog: expect.arrayContaining([
        expect.objectContaining({
          severity: "warning",
          area: "recording",
          title: "ISO recording warning",
          detail: "Andre Wallace ISO video is off."
        })
      ]),
      warnings: expect.arrayContaining(["Andre Wallace ISO video is off."])
    });
  });

  it("tracks network output sender sessions when streaming is active", async () => {
    const engine = new InMemoryMediaCoreSyncEngine();
    const snapshot = await engine.syncProduction({ ...initialProduction, recording: true, streaming: true }, 4100);

    expect(snapshot).toMatchObject({
      outputs: ["recording", "rtmp"],
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
      outputHealth: expect.arrayContaining([
        expect.objectContaining({
          destination: "rtmp",
          status: "live",
          message: "RTMP sender live."
        })
      ]),
      diagnostics: {
        outputSenderSession: {
          activeSenderCount: 1
        }
      }
    });
  });

  it("recovers failed sender and recording states through fallback media-core commands", () => {
    const engine = new TestMediaCoreSyncEngine();
    const baseCommands: NativeMediaCoreCommand[] = [
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
            isActiveSpeaker: true,
            isScreenSharing: false,
            audioLevel: 64,
            health: "live"
          }
        ]
      },
      {
        type: "load-scene-graph",
        sceneId: "host",
        routes: [{ routeId: "host", mode: "fixed", participantId: "p1", audioRole: "isolated" }]
      },
      { type: "start-program-output", destinations: ["recording", "rtmp"], isoParticipantIds: ["p1"] }
    ];

    const failed = engine.runCommands(
      [
        ...baseCommands,
        {
          type: "start-recording-session",
          sessionId: "recover-test",
          targetFolder: "Recordings/CoreVideo Pro",
          filenamePrefix: "Recover_Test",
          format: "mp4",
          quality: "high",
          isoParticipantIds: ["p1"]
        },
        { type: "fail-output-sender", destination: "rtmp", message: "RTMP connection refused." },
        { type: "fail-recording-session", message: "Recording writer crashed." }
      ],
      5000
    );

    expect(failed).toMatchObject({
      outputSenderSession: {
        status: "failed",
        senders: [{ destination: "rtmp", status: "failed", warning: "RTMP connection refused." }]
      },
      recording: {
        active: false,
        status: "failed",
        error: "Recording writer crashed."
      },
      eventLog: expect.arrayContaining([
        expect.objectContaining({
          severity: "critical",
          area: "sender",
          title: "RTMP sender failed",
          detail: "RTMP connection refused."
        }),
        expect.objectContaining({
          severity: "critical",
          area: "recording",
          title: "Recording writer failed",
          detail: "Recording writer crashed."
        })
      ]),
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
      outputHealth: expect.arrayContaining([
        expect.objectContaining({ destination: "rtmp", status: "failed", message: "RTMP connection refused." }),
        expect.objectContaining({ destination: "recording", status: "failed", message: "Recording writer crashed." })
      ])
    });

    const recovered = engine.runCommands(
      [
        ...baseCommands,
        { type: "recover-output-sender", destination: "rtmp", reason: "RTMP sender recovered after reconnect." },
        { type: "recover-recording-session", reason: "Recording writer recovered." }
      ],
      5033
    );

    expect(recovered).toMatchObject({
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
      eventLog: expect.arrayContaining([
        expect.objectContaining({
          severity: "info",
          area: "sender",
          title: "RTMP sender recovery requested"
        }),
        expect.objectContaining({
          severity: "info",
          area: "sender",
          title: "RTMP sender recovered",
          detail: "RTMP sender recovered at 1920x1080 60fps."
        }),
        expect.objectContaining({
          severity: "info",
          area: "recording",
          title: "Recording writer recovery requested"
        })
      ]),
      outputHealth: expect.arrayContaining([
        expect.objectContaining({ destination: "rtmp", status: "live" }),
        expect.objectContaining({ destination: "recording", status: "live" })
      ])
    });
  });

  it("records sender warning and stopped transitions for output routing", () => {
    const engine = new TestMediaCoreSyncEngine();
    const warning = engine.runCommands(
      [
        { type: "set-zoom-source-roster", sources: [] },
        {
          type: "load-scene-graph",
          sceneId: "missing-guest",
          routes: [{ routeId: "guest", mode: "fixed", participantId: "p9", audioRole: "mix" }]
        },
        { type: "start-program-output", destinations: ["srt"], isoParticipantIds: [] }
      ],
      2100
    );

    expect(warning).toMatchObject({
      outputSenderSession: {
        status: "warning",
        senders: [{ destination: "srt", status: "warning", warning: "SRT sender is publishing degraded program frames." }]
      },
      eventLog: expect.arrayContaining([
        expect.objectContaining({
          severity: "warning",
          area: "sender",
          title: "SRT sender warning",
          detail: "SRT sender is publishing degraded program frames.",
          relatedId: "srt:program"
        })
      ])
    });

    const stopped = engine.runCommands(
      [
        { type: "set-zoom-source-roster", sources: [] },
        {
          type: "load-scene-graph",
          sceneId: "missing-guest",
          routes: [{ routeId: "guest", mode: "fixed", participantId: "p9", audioRole: "mix" }]
        }
      ],
      2200
    );

    expect(stopped).toMatchObject({
      outputSenderSession: {
        senders: [{ destination: "srt", status: "stopped", stoppedAtMs: 2200 }]
      },
      eventLog: expect.arrayContaining([
        expect.objectContaining({
          severity: "info",
          area: "sender",
          title: "SRT sender stopped",
          detail: "SRT sender stopped after 1 frame.",
          relatedId: "srt:program"
        })
      ])
    });
  });

  it("executes recoverable operator actions with production context", async () => {
    const engine = new TestMediaCoreSyncEngine();
    const baseCommands: NativeMediaCoreCommand[] = [
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
            isActiveSpeaker: true,
            isScreenSharing: false,
            audioLevel: 64,
            health: "live"
          }
        ]
      },
      {
        type: "load-scene-graph",
        sceneId: "host",
        routes: [{ routeId: "host", mode: "fixed", participantId: "p1", audioRole: "isolated" }]
      },
      { type: "start-program-output", destinations: ["recording", "rtmp"], isoParticipantIds: ["p1"] }
    ];
    const failed = engine.runCommands(
      [...baseCommands, { type: "fail-output-sender", destination: "rtmp", message: "RTMP connection refused." }],
      7000
    );

    const recovered = await engine.executeOperatorAction(
      {
        ...initialProduction,
        recording: true,
        streaming: true
      },
      failed.operatorActions.find((action) => action.actionId === "sender:rtmp:recover")!,
      7033
    );

    expect(recovered).toMatchObject({
      outputSenderSession: {
        status: "live",
        senders: [{ destination: "rtmp", status: "live", warning: undefined }]
      },
      operatorActions: [],
      eventLog: expect.arrayContaining([
        expect.objectContaining({
          severity: "info",
          area: "sender",
          title: "RTMP sender recovery requested",
          commandType: "recover-output-sender"
        })
      ]),
      lastCommandTypes: expect.arrayContaining(["recover-output-sender"])
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
      operatorActions: expect.arrayContaining([
        expect.objectContaining({
          actionId: "routing:speaker-slides-1:resolve",
          severity: "warning",
          title: "Resolve missing route speaker-slides-1"
        })
      ]),
      eventLog: expect.arrayContaining([
        expect.objectContaining({
          severity: "warning",
          title: "Media core warning",
          detail: "Screen share route requested but no active screen share source is available."
        })
      ]),
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
