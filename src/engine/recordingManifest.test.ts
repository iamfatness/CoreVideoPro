import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
import { InMemoryMediaCoreSyncEngine } from "./mediaCoreSync";
import { buildRecordingManifest } from "./recordingManifest";

describe("buildRecordingManifest", () => {
  it("describes program and ISO recording tracks with participant metadata", async () => {
    const state = { ...initialProduction, recording: true };
    const mediaCore = await new InMemoryMediaCoreSyncEngine().syncProduction(state, 3000);
    const manifest = buildRecordingManifest(state, mediaCore);

    expect(manifest).toMatchObject({
      manifestId: "manifest-q2-product-update-Q2_Product_Update-p1-p2",
      sessionId: "Q2_Product_Update-p1-p2",
      status: "recording",
      active: true,
      meetingTitle: "Q2 Product Update",
      sceneId: "speaker-slides",
      targetFolder: "Recordings/CoreVideo Pro",
      filenamePrefix: "Q2_Product_Update",
      format: "mp4",
      quality: "high",
      outputProfile: {
        profileId: "1080p60",
        resolution: "1920x1080",
        fps: 60,
        targetBitrateMbps: 8.2
      },
      tracks: expect.arrayContaining([
        expect.objectContaining({ kind: "program", status: "writing", framesWritten: 1 }),
        expect.objectContaining({ kind: "iso", participantId: "p1", participantName: "Sophia Martinez", participantRole: "Host", readiness: "ready" }),
        expect.objectContaining({ kind: "iso", participantId: "p2", participantName: "David Chen", participantRole: "Presenter", readiness: "ready" })
      ]),
      isoPlan: {
        trackCount: 4,
        estimatedTotalMbps: 29,
        outputFolder: "recordings/q2productupdate",
        valid: true
      },
      totals: {
        framesWritten: 3,
        droppedFrames: 0,
        missingFrames: 0,
        bytesWritten: expect.any(Number),
        estimatedDiskRateMBps: 7.49
      },
      warnings: []
    });
  });

  it("captures unavailable ISO readiness and recording warnings", async () => {
    const state = {
      ...initialProduction,
      recording: true,
      participants: initialProduction.participants.map((participant) =>
        participant.id === "p2" ? { ...participant, health: "video-off" as const, isScreenSharing: false } : participant
      ),
      recordingSettings: {
        ...initialProduction.recordingSettings,
        isoParticipantIds: ["p2", "p9"]
      }
    };
    const mediaCore = await new InMemoryMediaCoreSyncEngine().syncProduction(state, 3200);
    const manifest = buildRecordingManifest(state, mediaCore);

    expect(manifest.status).toBe("warning");
    expect(manifest.tracks).toEqual(
      expect.arrayContaining([
        expect.objectContaining({ kind: "iso", participantId: "p2", readiness: "video-off", warning: "David Chen ISO video is off." }),
        expect.objectContaining({ kind: "iso", participantId: "p9", readiness: "missing", warning: "p9 ISO participant is not present in the Zoom roster." })
      ])
    );
    expect(manifest.totals.missingFrames).toBeGreaterThan(0);
    expect(manifest.warnings).toEqual(expect.arrayContaining(["David Chen ISO video is off.", "p9 ISO participant is not present in the Zoom roster."]));
  });

  it("returns an idle manifest before recording starts", () => {
    const manifest = buildRecordingManifest(initialProduction);

    expect(manifest.status).toBe("idle");
    expect(manifest.active).toBe(false);
    expect(manifest.tracks).toEqual([]);
    expect(manifest.targetFolder).toBe(initialProduction.recordingSettings.folder);
    expect(manifest.isoPlan.valid).toBe(true);
  });
});


it("never records command intent as confirmed output in an exported manifest", async () => {
  const state = { ...initialProduction, recording: true };
  expect(buildRecordingManifest(state)).toMatchObject({ active: false, status: "starting", finalized: false });
  const snapshot = await new InMemoryMediaCoreSyncEngine().syncProduction(state, 3000);
  snapshot.recording!.lifecycle = {
    sessionId: "take-1", desiredActive: false, state: "finalizing", health: "healthy", finalized: false
  };
  expect(buildRecordingManifest(state, snapshot)).toMatchObject({
    active: false, status: "finalizing", finalized: false,
    lifecycle: { sessionId: "take-1", state: "finalizing" }
  });
});
