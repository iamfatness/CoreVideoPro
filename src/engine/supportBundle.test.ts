import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
import { InMemoryMediaCoreSyncEngine } from "./mediaCoreSync";
import { createSupportBundle } from "./supportBundle";

describe("createSupportBundle", () => {
  it("builds human triage lines and machine-readable production diagnostics", () => {
    const bundle = createSupportBundle(initialProduction);

    expect(bundle.app.name).toBe("CoreVideo Pro");
    expect(bundle.triageLines).toContain("Show: AI Product Launch Webinar (set-and-forget)");
    expect(bundle.summaryText).toContain("Program: speaker-slides; Preview: speaker-slides");
    expect(bundle.participants.find((participant) => participant.name === "Priya Shah")).toMatchObject({
      health: "low-resolution",
      recommendedAction: "Ask participant to improve network or reduce competing bandwidth."
    });
    expect(bundle.actionCounts.lowDeliveredResolution).toBe(1);
    expect(bundle.isoCapacity.selectedParticipantIds).toEqual(["p1", "p2"]);
    expect(bundle.isoCapacity.estimatedPathCount).toBe(3);
    expect(bundle.warnings).toContain("1 participant feed delivered below target resolution.");
  });

  it("redacts stream secrets and credential-bearing endpoint parameters", () => {
    const bundle = createSupportBundle({
      ...initialProduction,
      outputDestinations: initialProduction.outputDestinations.map((destination) =>
        destination.id === "srt-backup"
          ? {
              ...destination,
              enabled: true,
              endpoint: "srt://backup.example.com:9000?mode=caller&passphrase=super-secret",
              streamKey: "backup-secret"
            }
          : destination
      )
    });

    const srt = bundle.output.destinations.find((destination) => destination.id === "srt-backup");

    expect(srt?.endpoint).toContain("passphrase=redacted");
    expect(srt?.endpoint).not.toContain("super-secret");
    expect(srt?.streamKey).toBe("present-redacted");
    expect(JSON.stringify(bundle)).not.toContain("backup-secret");
  });

  it("reports duplicate scene assignments and missing screen share as operator warnings", () => {
    const bundle = createSupportBundle({
      ...initialProduction,
      participants: initialProduction.participants.map((participant) => ({ ...participant, isScreenSharing: false })),
      scenes: initialProduction.scenes.map((scene) =>
        scene.id === "speaker-slides"
          ? {
              ...scene,
              routes: [
                { id: "r1", mode: "fixed", participantId: "p2", audioRole: "isolated" },
                { id: "r2", mode: "fixed", participantId: "p2", audioRole: "isolated" },
                { id: "r3", mode: "screen-share", audioRole: "audience" }
              ]
            }
          : scene
      )
    });

    expect(bundle.actionCounts.duplicateAssignments).toBe(1);
    expect(bundle.actionCounts.unavailableScreenShare).toBe(1);
    expect(bundle.warnings).toContain("1 duplicate scene assignment detected.");
    expect(bundle.warnings).toContain("Program scene expects screen share, but no participant is sharing.");
  });

  it("includes sanitized media-core runtime diagnostics when provided", async () => {
    const mediaCore = await new InMemoryMediaCoreSyncEngine().syncProduction(
      {
        ...initialProduction,
        recording: true,
        streaming: true
      },
      5000
    );
    const bundle = createSupportBundle(initialProduction, mediaCore);

    expect(bundle.mediaCore).toMatchObject({
      sceneId: "speaker-slides",
      renderPlanId: expect.stringMatching(/^rp-/),
      source: {
        adapterId: "renderer-test-pattern",
        kind: "test-pattern",
        status: "subscribed",
        subscribedSourceCount: 3
      },
      compositor: {
        status: "live",
        programFrameCount: 1
      },
      transport: {
        status: "publishing",
        frameNumber: 1,
        latencyMs: 0
      },
      encoder: {
        status: "encoding",
        lifecycle: "encoding",
        targetCount: 4
      },
      senders: {
        status: "live",
        activeSenderCount: 1,
        destinations: [
          {
            destination: "rtmp",
            status: "live",
            startedAtMs: 5000,
            lastFrameNumber: 1,
            framesSent: 1,
            retryCount: 0,
            latencyMs: 2100,
            bitrateMbps: 8.2,
            warning: undefined
          }
        ]
      },
      recording: {
        status: "recording",
        writerStatus: "writing",
        totalFramesWritten: 3,
        estimatedDiskRateMBps: 7.49,
        streams: expect.arrayContaining([
          expect.objectContaining({
            kind: "iso",
            participantId: "p1",
            status: "writing",
            expectedFrames: 1,
            framesWritten: 1,
            missingFrames: 0,
            warning: undefined
          })
        ])
      },
      operatorActions: [],
      eventLog: [
        expect.objectContaining({
          severity: "info",
          area: "sender",
          title: "RTMP sender live",
          detail: "RTMP sender live at 1920x1080 60fps; 8.2 Mbps target.",
          relatedId: "rtmp:program"
        })
      ]
    });
    expect(JSON.stringify(bundle.mediaCore)).not.toContain("streamKey");
    expect(JSON.stringify(bundle.mediaCore)).not.toContain("endpoint");
  });

  it("includes routed Zoom source health issues in media-core diagnostics", () => {
    class TestMediaCoreSyncEngine extends InMemoryMediaCoreSyncEngine {
      runRoutedLowResolutionFeed() {
        return this.snapshot(
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
                  isActiveSpeaker: false,
                  isScreenSharing: false,
                  audioLevel: 64,
                  health: "low-resolution"
                }
              ]
            },
            {
              type: "load-scene-graph",
              sceneId: "single",
              routes: [{ routeId: "guest", mode: "fixed", participantId: "p1", audioRole: "mix" }]
            }
          ],
          2000
        );
      }
    }

    const mediaCore = new TestMediaCoreSyncEngine().runRoutedLowResolutionFeed();
    const bundle = createSupportBundle(initialProduction, mediaCore);

    expect(bundle.mediaCore?.source.issues).toEqual([
      {
        sourceId: "participant:p1",
        participantId: "p1",
        displayName: "Maya Chen",
        health: "low-resolution",
        severity: "warning",
        detail: "Maya Chen feed is below target resolution."
      }
    ]);
  });

  it("includes media-core operator actions for support triage", async () => {
    class TestMediaCoreSyncEngine extends InMemoryMediaCoreSyncEngine {
      runFailure() {
        return this.snapshot(
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
                }
              ]
            },
            {
              type: "load-scene-graph",
              sceneId: "host",
              routes: [{ routeId: "host", mode: "fixed", participantId: "p1", audioRole: "isolated" }]
            },
            { type: "start-program-output", destinations: ["recording", "rtmp"], isoParticipantIds: ["p1"] },
            {
              type: "start-recording-session",
              sessionId: "support-recover-test",
              targetFolder: "Recordings/CoreVideo Pro",
              filenamePrefix: "Support_Recover_Test",
              format: "mp4",
              quality: "high",
              isoParticipantIds: ["p1"]
            },
            { type: "fail-output-sender", destination: "rtmp", message: "RTMP connection refused." },
            { type: "fail-recording-session", message: "Recording writer crashed." }
          ],
          6200
        );
      }
    }

    const mediaCore = new TestMediaCoreSyncEngine().runFailure();
    const bundle = createSupportBundle(initialProduction, mediaCore);

    expect(bundle.mediaCore?.operatorActions).toEqual([
      expect.objectContaining({
        actionId: "recording:recover",
        severity: "critical",
        command: "recover-recording-session"
      }),
      expect.objectContaining({
        actionId: "sender:rtmp:recover",
        severity: "critical",
        command: "recover-output-sender:rtmp"
      })
    ]);
    expect(bundle.mediaCore?.eventLog).toEqual(
      expect.arrayContaining([
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
      ])
    );
  });
});
