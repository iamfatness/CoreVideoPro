import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
import { mapCaptureSnapshot } from "./captureSnapshotMapper";
import { buildCaptionOverlay } from "./captionOverlay";

describe("buildCaptionOverlay", () => {
  it("attributes captions to the active speaker and lifts lower-thirds over slides", () => {
    const snapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p2",
      caption: "This is a clean attributed caption.",
      participants: initialProduction.participants.map((participant) => ({
        userId: participant.id,
        displayName: participant.name,
        role: participant.role,
        talking: participant.id === "p2",
        sharingScreen: participant.id === "p2",
        networkQuality: "good"
      }))
    });

    const overlay = buildCaptionOverlay(snapshot, initialProduction.scenes[2]);

    expect(overlay.speakerName).toBe("David Chen");
    expect(overlay.captionPosition).toBe("bottom");
    expect(overlay.lowerThirdPosition).toBe("upper-left");
    expect(overlay.warnings).toContain("Lower-third lifted to protect slide captions.");
  });

  it("moves captions away from focus title graphics", () => {
    const snapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      caption: "",
      participants: initialProduction.participants.slice(0, 1).map((participant) => ({
        userId: participant.id,
        displayName: participant.name,
        role: participant.role,
        talking: true,
        sharingScreen: false,
        networkQuality: "good"
      }))
    });

    const overlay = buildCaptionOverlay(snapshot, initialProduction.scenes[0]);

    expect(overlay.text).toBe("Captions are standing by for the next speaker.");
    expect(overlay.captionPosition).toBe("top");
    expect(overlay.lowerThirdPosition).toBe("lower-left");
  });
});
