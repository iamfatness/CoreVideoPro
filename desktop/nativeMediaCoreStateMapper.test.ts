import { describe, expect, it } from "vitest";
import type { NativeMediaCoreCommand } from "../src/engine/nativeMediaCoreProtocol";
import { mapNativeWireStateToSnapshot } from "./nativeMediaCoreStateMapper.ts";

describe("mapNativeWireStateToSnapshot", () => {
  const commands: NativeMediaCoreCommand[] = [
    {
      type: "load-scene-graph",
      sceneId: "interview",
      routes: [{ routeId: "a", mode: "active-speaker", audioRole: "mix" }]
    },
    {
      type: "start-program-output",
      destinations: ["recording", "rtmp"],
      isoParticipantIds: ["p1"]
    },
    {
      type: "start-recording-session",
      sessionId: "show-1",
      targetFolder: "Recordings",
      filenamePrefix: "program",
      format: "mp4",
      quality: "high",
      isoParticipantIds: ["p1"]
    }
  ];

  it("merges native encoder, recording, and sender state into a renderer snapshot", () => {
    const snapshot = mapNativeWireStateToSnapshot(commands, 3000, 12, {
      sceneId: "interview",
      routeCount: 1,
      transformCount: 0,
      overlayCount: 0,
      outputs: ["recording", "rtmp"],
      isoParticipantIds: ["p1"],
      programFrameCount: 12,
      renderPlanId: "interview:1:0",
      compositorRenderer: "d3d11",
      encoderSession: {
        status: "encoding",
        renderPlanId: "interview:1:0",
        programFrameCount: 12,
        targets: [
          {
            targetId: "recording:program",
            destination: "recording",
            streamKind: "program",
            status: "attached",
            attachedFrameCount: 12
          }
        ],
        lifecycle: { status: "encoding", lastTransition: "Program output encoder session started." },
        warnings: []
      },
      outputSenderSession: {
        status: "live",
        activeSenderCount: 1,
        senders: [
          {
            senderId: "rtmp:program",
            destination: "rtmp",
            status: "live",
            framesSent: 12,
            retryCount: 0,
            latencyMs: 2100,
            bitrateMbps: 6
          }
        ],
        warnings: []
      },
      recording: {
        sessionId: "show-1",
        active: true,
        status: "recording",
        writerStatus: "writing",
        startedAtMs: 1000,
        elapsedMs: 2000,
        targetFolder: "Recordings",
        filenamePrefix: "program",
        format: "mp4",
        quality: "high",
        encoder: { codec: "h264", hardwareAccelerated: true, targetBitrateMbps: 18 },
        estimatedDiskRateMBps: 4.99,
        programPath: "Recordings/program-program-0.mp4",
        streams: [],
        totalFramesWritten: 12,
        totalDroppedFrames: 0,
        totalBytesWritten: 4096
      },
      health: {
        status: "live",
        renderer: "d3d11",
        encoder: "media-foundation",
        codec: "h264",
        hardwareEncoder: true,
        recordingArtifactPath: "C:/Temp/corevideo-mf-recording-123.mp4",
        recordingBytesWritten: 4096,
        encodedFrameCount: 12,
        frameCount: 12
      },
      profile: {
        name: "CoreVideo Pro Native Media Core",
        renderer: "d3d11",
        encoder: "media-foundation",
        maxProgramResolution: "1920x1080",
        maxProgramFps: 30,
        maxParticipantFeeds: 6,
        maxIsoRecordings: 2,
        capabilities: ["gpu-compositor", "program-recording", "rtmp-output"]
      }
    });

    expect(snapshot.sceneId).toBe("interview");
    expect(snapshot.outputs).toEqual(["recording", "rtmp"]);
    expect(snapshot.programFrameCount).toBe(12);
    expect(snapshot.compositor.status).toBe("live");
    expect(snapshot.encoderSession.status).toBe("encoding");
    expect(snapshot.outputSenderSession.status).toBe("live");
    expect(snapshot.recording?.status).toBe("recording");
    expect(snapshot.warnings[0]).toContain("corevideo-mf-recording-123.mp4");
    expect(snapshot.outputHealth.some((item) => item.destination === "rtmp" && item.status === "live")).toBe(true);
  });

  it("merges native audio mix and caption track state from the wire payload", () => {
    const snapshot = mapNativeWireStateToSnapshot(commands, 3000, 12, {
      sceneId: "interview",
      routeCount: 1,
      audioMixSession: {
        status: "live",
        masterLevel: 72,
        loudnessLufs: -16,
        limiterActive: false,
        mixedFrameCount: 12,
        participants: [
          {
            participantId: "p1",
            inputLevel: 64,
            outputLevel: 68,
            gainDb: 0,
            noiseSuppression: false,
            limiterActive: false,
            muted: false,
            status: "balanced"
          }
        ],
        summary: "Program mix balanced",
        warnings: []
      },
      captionTrack: {
        enabled: true,
        status: "live",
        currentCue: {
          text: "Welcome to the webinar.",
          speaker: "Maya Chen",
          atMs: 2800,
          confidence: 95
        },
        latencyMs: 180,
        warnings: []
      }
    });

    expect(snapshot.audioMixSession.summary).toBe("Program mix balanced");
    expect(snapshot.audioMixSession.masterLevel).toBe(72);
    expect(snapshot.captionTrack.currentCue?.text).toBe("Welcome to the webinar.");
    expect(snapshot.diagnostics.audioMixSession.masterLevel).toBe(72);
    expect(snapshot.diagnostics.captionTrack.currentCue?.speaker).toBe("Maya Chen");
  });
});