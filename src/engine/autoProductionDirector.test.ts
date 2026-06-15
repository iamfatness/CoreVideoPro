import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
import { mapCaptureSnapshot } from "./captureSnapshotMapper";
import {
  planAutoProduction,
  requiredDirectorHoldMs,
  SCREEN_SHARE_ENTER_HOLD_MS,
  SCREEN_SHARE_EXIT_HOLD_MS
} from "./autoProductionDirector";
import { recommendAutoProduction } from "./mockEngines";

function meetingSnapshot(screenShare = false, participantCount = initialProduction.participants.length, tick = 1) {
  return mapCaptureSnapshot({
    meetingState: "in_meeting",
    tick,
    activeSpeakerId: "p1",
    participants: initialProduction.participants.slice(0, participantCount).map((participant, index) => ({
      userId: participant.id,
      displayName: participant.name,
      role: participant.role,
      talking: index === 0,
      sharingScreen: screenShare && index === 1,
      networkQuality: "good"
    }))
  });
}

describe("requiredDirectorHoldMs", () => {
  it("requires a longer hold before entering speaker-slides during screen share", () => {
    expect(
      requiredDirectorHoldMs({
        currentSceneId: "panel",
        targetSceneId: "speaker-slides",
        ruleId: "screen-share-priority",
        screenShareActive: true
      })
    ).toBe(SCREEN_SHARE_ENTER_HOLD_MS);
  });

  it("requires a longer hold before leaving speaker-slides after screen share stops", () => {
    expect(
      requiredDirectorHoldMs({
        currentSceneId: "speaker-slides",
        targetSceneId: "panel",
        ruleId: "panel-discussion",
        screenShareActive: false
      })
    ).toBe(SCREEN_SHARE_EXIT_HOLD_MS);
  });
});

describe("planAutoProduction", () => {
  it("holds scene changes until the stabilization window elapses", () => {
    const snapshot = meetingSnapshot(false);
    const first = planAutoProduction(
      { ...initialProduction, activeSceneId: "speaker-slides", previewSceneId: "speaker-slides" },
      snapshot,
      1000
    );

    expect(first.action).toBe("hold");
    expect(first.ruleId).toBe("director-hold");
    expect(first.pendingSceneId).toBe("panel");
    expect(first.reason).toContain("Holding");

    const second = planAutoProduction(
      {
        ...initialProduction,
        activeSceneId: "speaker-slides",
        previewSceneId: "speaker-slides",
        autoProduction: first
      },
      snapshot,
      6000
    );

    expect(second.action).toBe("take");
    expect(second.recommendedSceneId).toBe("panel");
  });

  it("takes speaker-slides once screen share has been stable long enough", () => {
    const snapshot = meetingSnapshot(true);
    const first = planAutoProduction(
      { ...initialProduction, activeSceneId: "panel", previewSceneId: "panel" },
      snapshot,
      1000
    );

    expect(first.action).toBe("hold");
    expect(first.pendingSceneId).toBe("speaker-slides");
    expect(first.holdReason).toBe("Screen share stabilization");

    const second = planAutoProduction(
      {
        ...initialProduction,
        activeSceneId: "panel",
        previewSceneId: "panel",
        autoProduction: first
      },
      snapshot,
      4000
    );

    expect(second.action).toBe("take");
    expect(second.recommendedSceneId).toBe("speaker-slides");
  });
});

describe("recommendAutoProduction integration", () => {
  it("still queues recommendations in manual mode without director holds", () => {
    const recommendation = recommendAutoProduction({ ...initialProduction, mode: "manual" }, meetingSnapshot(false));

    expect(recommendation.recommendedSceneId).toBe("panel");
    expect(recommendation.action).toBe("queue");
  });
});