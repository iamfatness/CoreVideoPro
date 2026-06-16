import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
import { mapCaptureSnapshot } from "./captureSnapshotMapper";
import { buildMagicScene, recommendAutoProduction } from "./mockEngines";

describe("buildMagicScene", () => {
  it("selects speaker plus slides when screen share is active", () => {
    const result = buildMagicScene({
      participants: initialProduction.participants,
      currentScenes: initialProduction.scenes,
      screenShareActive: true
    });

    expect(result.scenes.find((scene) => scene.selected)?.id).toBe("speaker-slides");
    expect(result.scenes.find((scene) => scene.id === "speaker-slides")?.layout).toBe("speaker-slides");
    expect(result.summary).toContain("with screen share");
    expect(result.scenes.find((scene) => scene.id === "speaker-slides")?.slots).toContain("screen-share");
  });

  it("includes low-quality participant warnings", () => {
    const result = buildMagicScene({
      participants: initialProduction.participants,
      currentScenes: initialProduction.scenes,
      screenShareActive: true
    });

    expect(result.warnings).toContain("Robert Smith is low resolution.");
  });

  it("falls back to interview for two participants without screen share", () => {
    const participants = initialProduction.participants.slice(0, 2).map((participant) => ({
      ...participant,
      isScreenSharing: false
    }));

    const result = buildMagicScene({
      participants,
      currentScenes: initialProduction.scenes,
      screenShareActive: false
    });

    expect(result.scenes.find((scene) => scene.selected)?.id).toBe("interview");
    expect(result.scenes.find((scene) => scene.selected)?.layout).toBe("two-up");
  });

  it("falls back to panel for larger meetings without screen share", () => {
    const participants = initialProduction.participants.map((participant) => ({
      ...participant,
      isScreenSharing: false
    }));

    const result = buildMagicScene({
      participants,
      currentScenes: initialProduction.scenes,
      screenShareActive: false
    });

    expect(result.scenes.find((scene) => scene.selected)?.id).toBe("panel");
    expect(result.scenes.find((scene) => scene.selected)?.layout).toBe("smart-grid");
  });
});

describe("recommendAutoProduction", () => {
  it("holds speaker plus slides when screen share is already live", () => {
    const snapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p2",
      participants: initialProduction.participants.map((participant) => ({
        userId: participant.id,
        displayName: participant.name,
        role: participant.role,
        talking: participant.id === "p2",
        sharingScreen: participant.id === "p2",
        networkQuality: "good"
      }))
    });

    const recommendation = recommendAutoProduction(initialProduction, snapshot);

    expect(recommendation.recommendedSceneId).toBe("speaker-slides");
    expect(recommendation.action).toBe("hold");
    expect(recommendation.reason).toContain("Screen share is active");
  });

  it("takes panel automatically in set-and-forget when a larger meeting has no screen share", () => {
    const snapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: initialProduction.participants.map((participant) => ({
        userId: participant.id,
        displayName: participant.name,
        role: participant.role,
        talking: participant.id === "p1",
        sharingScreen: false,
        networkQuality: "good"
      }))
    });

    const stabilized = recommendAutoProduction(
      {
        ...initialProduction,
        autoProduction: {
          ...initialProduction.autoProduction,
          pendingSceneId: "panel",
          holdStartedAtMs: 0,
          holdElapsedMs: 5000
        }
      },
      snapshot
    );

    expect(stabilized.recommendedSceneId).toBe("panel");
    expect(stabilized.action).toBe("take");
    expect(stabilized.confidence).toBeGreaterThanOrEqual(90);
    expect(stabilized.ruleId).toBe("panel-discussion");
    expect(stabilized.signals).toContain("rule panel-discussion");
  });

  it("queues recommendations in manual mode", () => {
    const snapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: initialProduction.participants.map((participant) => ({
        userId: participant.id,
        displayName: participant.name,
        role: participant.role,
        talking: participant.id === "p1",
        sharingScreen: false,
        networkQuality: "good"
      }))
    });

    const recommendation = recommendAutoProduction({ ...initialProduction, mode: "manual" }, snapshot);

    expect(recommendation.recommendedSceneId).toBe("panel");
    expect(recommendation.action).toBe("queue");
  });

  it("holds set-and-forget when the producer has a different preview override", () => {
    const snapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: initialProduction.participants.map((participant) => ({
        userId: participant.id,
        displayName: participant.name,
        role: participant.role,
        talking: participant.id === "p1",
        sharingScreen: false,
        networkQuality: "good"
      }))
    });

    const recommendation = recommendAutoProduction(
      {
        ...initialProduction,
        activeSceneId: "speaker-slides",
        previewSceneId: "interview"
      },
      snapshot
    );

    expect(recommendation.recommendedSceneId).toBe("panel");
    expect(recommendation.action).toBe("hold");
    expect(recommendation.ruleId).toBe("producer-preview-override");
    expect(recommendation.overrideReason).toBe("Producer preview override is holding Interview.");
  });

  it("uses a host-focused scene when only one live speaker remains", () => {
    const snapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: [
        {
          userId: "p1",
          displayName: "Sophia Martinez",
          role: "Host",
          talking: true,
          sharingScreen: false,
          networkQuality: "good"
        }
      ]
    });

    const recommendation = recommendAutoProduction(
      {
        ...initialProduction,
        activeSceneId: "panel",
        previewSceneId: "panel",
        autoProduction: {
          ...initialProduction.autoProduction,
          pendingSceneId: "intro",
          holdStartedAtMs: 0,
          holdElapsedMs: 5000
        }
      },
      snapshot
    );

    expect(recommendation.recommendedSceneId).toBe("intro");
    expect(recommendation.action).toBe("take");
    expect(recommendation.ruleId).toBe("single-speaker");
    expect(recommendation.reason).toContain("One live speaker");
  });
});
