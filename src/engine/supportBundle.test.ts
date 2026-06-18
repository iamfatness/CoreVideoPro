import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
import { InMemoryMediaCoreSyncEngine } from "./mediaCoreSync";
import { createSupportBundle, prepareSupportBundleForUpload } from "./supportBundle";

describe("createSupportBundle", () => {
  it("anonymizes participant names for telemetry upload", () => {
    const bundle = prepareSupportBundleForUpload(createSupportBundle(initialProduction));
    expect(bundle.participants.every((participant) => participant.name.startsWith("participant-"))).toBe(true);
    expect(bundle.participants.find((participant) => participant.id === "p1")?.name).toBe("participant-p1");
  });

  it("builds human triage lines and machine-readable production diagnostics", () => {
    const bundle = createSupportBundle(initialProduction);

    expect(bundle.app.name).toBe("CoreVideo Pro");
    expect(bundle.app.platform).toBe("mock-desktop");
    expect(bundle.triageLines).toContain("Show: Q2 Product Update (set-and-forget)");
    expect(bundle.summaryText).toContain("Program: speaker-slides; Preview: speaker-slides");
    expect(bundle.participants.find((participant) => participant.name === "Robert Smith")).toMatchObject({
      health: "low-resolution",
      recommendedAction: "Ask participant to improve network or reduce competing bandwidth."
    });
    expect(bundle.actionCounts.lowDeliveredResolution).toBe(1);
    expect(bundle.isoCapacity.selectedParticipantIds).toEqual(["p1", "p2"]);
    expect(bundle.isoCapacity.estimatedPathCount).toBe(3);
    expect(bundle.warnings).toContain("1 participant feed delivered below target resolution.");
  });

  it("includes runtime and crash recovery context when provided", () => {
    const bundle = createSupportBundle(initialProduction, undefined, {
      platform: "win32",
      version: "0.1.0",
      runtime: {
        status: "degraded",
        label: "Media core recovering (1 restart)",
        host: "electron",
        platform: "win32",
        restartCount: 1,
        recovering: true,
        warnings: ["The media core child process crashed and is restarting automatically."]
      },
      crashEvents: [{ at: "2026-06-14T12:00:00.000Z", exitCode: 1, restartCount: 1 }]
    });

    expect(bundle.app.platform).toBe("win32");
    expect(bundle.runtime?.label).toMatch(/recovering/i);
    expect(bundle.crashRecovery?.events).toHaveLength(1);
    expect(bundle.triageLines).toContain("Runtime: Media core recovering (1 restart) (degraded)");
    expect(bundle.triageLines).toContain("Media core restarts: 1");
    expect(bundle.triageLines).toContain("Latest crash: exit 1 at 2026-06-14T12:00:00.000Z");
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
      audio: {
        status: "warning",
        masterLevel: expect.any(Number),
        loudnessLufs: expect.any(Number),
        limiterActive: expect.any(Boolean),
        participantCount: expect.any(Number),
        warningCategories: expect.arrayContaining(["low-level", "noise-suppression"]),
        warnings: expect.arrayContaining(["Noise suppression active on low-level sources."])
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
      eventLog: expect.arrayContaining([
        expect.objectContaining({
          severity: "info",
          area: "sender",
          title: "RTMP sender live",
          detail: "RTMP sender live at 1920x1080 60fps; 8.2 Mbps target.",
          relatedId: "rtmp:program"
        }),
        expect.objectContaining({
          severity: "warning",
          title: "Media core warning",
          detail: "Noise suppression active on low-level sources."
        })
      ])
    });
    expect(JSON.stringify(bundle.mediaCore)).not.toContain("streamKey");
    expect(JSON.stringify(bundle.mediaCore)).not.toContain("endpoint");
  });

  it("surfaces aggregate native audio underrun clipping and sync diagnostics in support data", async () => {
    const mediaCore = await new InMemoryMediaCoreSyncEngine().syncProduction(
      {
        ...initialProduction,
        participants: initialProduction.participants.map((participant) =>
          participant.id === "p2"
            ? {
                ...participant,
                audioLevel: 98,
                gainDb: 12
              }
            : participant
        )
      },
      5600
    );
    const diagnosticWarnings = [
      "Audio packet underrun or timing gap detected in native DSP mix.",
      "Audio clipping detected before native DSP limiting.",
      "A/V sync offset exceeded threshold in native DSP mix."
    ];
    const bundle = createSupportBundle(initialProduction, {
      ...mediaCore,
      audioMixSession: {
        ...mediaCore.audioMixSession,
        status: "warning",
        limiterActive: true,
        warnings: [...mediaCore.audioMixSession.warnings, ...diagnosticWarnings]
      },
      warnings: [...mediaCore.warnings, ...diagnosticWarnings]
    });

    expect(bundle.mediaCore?.audio).toMatchObject({
      status: "warning",
      limiterActive: true,
      participantCount: mediaCore.audioMixSession.participants.length,
      limitedParticipantCount: expect.any(Number),
      warningCount: expect.any(Number),
      warningCategories: expect.arrayContaining(["underrun", "clipping", "av-sync", "limiter"])
    });
    expect(bundle.mediaCore?.audio.warnings).toEqual(expect.arrayContaining(diagnosticWarnings));
    expect(bundle.triageLines).toContain("Audio diagnostics: warning; categories: underrun, clipping, av-sync, limiter, low-level, noise-suppression; warnings: 4");
    expect(JSON.stringify(bundle.mediaCore?.audio)).not.toContain("avSyncOffsetMs");
    expect(JSON.stringify(bundle.mediaCore?.audio)).not.toContain("timingDriftMs");
  });

  it("includes ISO recording readiness in media-core support diagnostics", async () => {
    const mediaCore = await new InMemoryMediaCoreSyncEngine().syncProduction(
      {
        ...initialProduction,
        recording: true,
        participants: initialProduction.participants.map((participant) =>
          participant.id === "p2" ? { ...participant, health: "video-off" as const, isScreenSharing: false } : participant
        ),
        recordingSettings: {
          ...initialProduction.recordingSettings,
          isoParticipantIds: ["p2", "p9"]
        }
      },
      5400
    );
    const bundle = createSupportBundle(initialProduction, mediaCore);

    expect(bundle.mediaCore?.recording).toMatchObject({
      status: "warning",
      writerStatus: "warning",
      streams: expect.arrayContaining([
        expect.objectContaining({
          kind: "iso",
          participantId: "p2",
          readiness: "video-off",
          status: "warning",
          warning: "David Chen ISO video is off."
        }),
        expect.objectContaining({
          kind: "iso",
          participantId: "p9",
          readiness: "missing",
          status: "warning",
          warning: "p9 ISO participant is not present in the Zoom roster."
        })
      ])
    });
    expect(bundle.mediaCore?.recordingManifest).toMatchObject({
      status: "warning",
      active: true,
      trackCount: 3,
      isoTrackCount: 2,
      totals: {
        missingFrames: expect.any(Number)
      },
      warnings: expect.arrayContaining(["David Chen ISO video is off."])
    });
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
                  displayName: "Sophia Martinez",
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
        displayName: "Sophia Martinez",
        health: "low-resolution",
        severity: "warning",
        detail: "Sophia Martinez feed is below target resolution."
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
                  displayName: "Sophia Martinez",
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
