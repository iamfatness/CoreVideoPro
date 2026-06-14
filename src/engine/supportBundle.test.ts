import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
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
});
