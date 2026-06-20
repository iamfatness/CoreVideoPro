import { describe, expect, it } from "vitest";
import { initialProduction, type ProductionState } from "../domain/production";
import { mapCaptureSnapshot, type RawParticipantEvent } from "./captureSnapshotMapper";
import {
  planAutoProduction,
  requiredDirectorHoldMs,
  SCENE_CHANGE_HOLD_MS,
  SCREEN_SHARE_ENTER_HOLD_MS,
  SCREEN_SHARE_EXIT_HOLD_MS
} from "./autoProductionDirector";
import { StaticDirectorStrategy } from "./directorStrategy";
import { recommendAutoProduction } from "./mockEngines";

function rawParticipants(events: Partial<RawParticipantEvent>[]): RawParticipantEvent[] {
  return events.map((event, index) => ({
    userId: event.userId ?? `p${index + 1}`,
    displayName: event.displayName ?? `Participant ${index + 1}`,
    role: event.role ?? "Guest",
    talking: event.talking ?? false,
    sharingScreen: event.sharingScreen ?? false,
    muted: event.muted ?? false,
    videoOn: event.videoOn ?? true,
    networkQuality: event.networkQuality ?? "good"
  }));
}

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

describe("planAutoProduction robustness", () => {
  it("holds the program when the meeting is in-progress but no one has live video", () => {
    const snapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 2,
      activeSpeakerId: "p1",
      participants: rawParticipants([
        { userId: "p1", role: "Host", videoOn: false },
        { userId: "p2", role: "Presenter", videoOn: false, muted: true }
      ])
    });

    const recommendation = planAutoProduction(
      { ...initialProduction, activeSceneId: "panel", previewSceneId: "panel" },
      snapshot,
      8000
    );

    expect(recommendation.action).toBe("hold");
    expect(recommendation.ruleId).toBe("hold-empty");
    expect(recommendation.recommendedSceneId).toBe("panel");
    expect(recommendation.reason).toContain("live video");
    expect(recommendation.signals).toContain("no live video");
  });

  it("keeps a solo presenter on the host-focus shot", () => {
    const snapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: rawParticipants([{ userId: "p1", role: "Host", talking: true }])
    });

    const recommendation = planAutoProduction(
      {
        ...initialProduction,
        activeSceneId: "intro",
        previewSceneId: "intro"
      },
      snapshot,
      9000
    );

    expect(recommendation.recommendedSceneId).toBe("intro");
    expect(recommendation.ruleId).toBe("single-speaker");
  });

  it("keeps a panel stable across a brief single-feed dropout (4-up loses one)", () => {
    const four = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: rawParticipants([
        { userId: "p1", role: "Host", talking: true },
        { userId: "p2", role: "Panelist" },
        { userId: "p3", role: "Panelist" },
        { userId: "p4", role: "Panelist" }
      ])
    });
    const droppedOne = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 2,
      activeSpeakerId: "p1",
      participants: rawParticipants([
        { userId: "p1", role: "Host", talking: true },
        { userId: "p2", role: "Panelist" },
        { userId: "p3", role: "Panelist", videoOn: false },
        { userId: "p4", role: "Panelist" }
      ])
    });

    const stateOnPanel: ProductionState = {
      ...initialProduction,
      activeSceneId: "panel",
      previewSceneId: "panel"
    };

    expect(planAutoProduction(stateOnPanel, four, 8000).recommendedSceneId).toBe("panel");
    // A transient drop from 4 -> 3 live still resolves to a panel: no scene churn.
    expect(planAutoProduction(stateOnPanel, droppedOne, 16000).recommendedSceneId).toBe("panel");
  });

  it("requires the enter hold for 4+ participants when one starts a screen share", () => {
    const snapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p2",
      participants: rawParticipants([
        { userId: "p1", role: "Host", talking: false },
        { userId: "p2", role: "Presenter", talking: true, sharingScreen: true },
        { userId: "p3", role: "Panelist" },
        { userId: "p4", role: "Panelist" }
      ])
    });

    const first = planAutoProduction(
      { ...initialProduction, activeSceneId: "panel", previewSceneId: "panel" },
      snapshot,
      1000
    );

    expect(first.action).toBe("hold");
    expect(first.pendingSceneId).toBe("speaker-slides");
    expect(first.holdReason).toBe("Screen share stabilization");

    const settled = planAutoProduction(
      { ...initialProduction, activeSceneId: "panel", previewSceneId: "panel", autoProduction: first },
      snapshot,
      1000 + SCREEN_SHARE_ENTER_HOLD_MS
    );
    expect(settled.action).toBe("take");
    expect(settled.recommendedSceneId).toBe("speaker-slides");
  });

  it("requires the exit hold before returning to a panel after a screen share stops", () => {
    const snapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 3,
      activeSpeakerId: "p1",
      participants: rawParticipants([
        { userId: "p1", role: "Host", talking: true },
        { userId: "p2", role: "Presenter" },
        { userId: "p3", role: "Panelist" },
        { userId: "p4", role: "Panelist" }
      ])
    });

    const onSlides: ProductionState = {
      ...initialProduction,
      activeSceneId: "speaker-slides",
      previewSceneId: "speaker-slides"
    };

    const first = planAutoProduction(onSlides, snapshot, 1000);
    expect(first.action).toBe("hold");
    expect(first.pendingSceneId).toBe("panel");
    expect(first.holdReason).toBe("Screen share release stabilization");

    const settled = planAutoProduction(
      { ...onSlides, autoProduction: first },
      snapshot,
      1000 + SCREEN_SHARE_EXIT_HOLD_MS
    );
    expect(settled.action).toBe("take");
    expect(settled.recommendedSceneId).toBe("panel");
  });

  it("lowers confidence during rapid active-speaker flips (cross-talk) than a calm room", () => {
    const calm = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: rawParticipants([
        { userId: "p1", role: "Host", talking: true },
        { userId: "p2", role: "Panelist" },
        { userId: "p3", role: "Panelist" },
        { userId: "p4", role: "Panelist" }
      ])
    });
    const crossTalk = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: rawParticipants([
        { userId: "p1", role: "Host", talking: true },
        { userId: "p2", role: "Panelist", talking: true },
        { userId: "p3", role: "Panelist", talking: true },
        { userId: "p4", role: "Panelist" }
      ])
    });

    const stateOnPanel: ProductionState = { ...initialProduction, activeSceneId: "panel", previewSceneId: "panel" };
    const calmRec = planAutoProduction(stateOnPanel, calm, 8000);
    const crossTalkRec = planAutoProduction(stateOnPanel, crossTalk, 8000);

    expect(calmRec.recommendedSceneId).toBe("panel");
    expect(crossTalkRec.recommendedSceneId).toBe("panel");
    expect(crossTalkRec.confidence).toBeLessThan(calmRec.confidence);
  });
});

describe("planAutoProduction strategy seam + fallback", () => {
  const panelSnapshot = meetingSnapshot(false);

  it("falls back to the heuristic when the strategy throws", () => {
    const throwing = new StaticDirectorStrategy(() => {
      throw new Error("strategy boom");
    }, "throwing");

    const heuristic = planAutoProduction(initialProduction, panelSnapshot, 8000);
    const withThrowing = planAutoProduction(initialProduction, panelSnapshot, 8000, throwing);

    expect(withThrowing.recommendedSceneId).toBe(heuristic.recommendedSceneId);
    expect(withThrowing.ruleId).toBe(heuristic.ruleId);
    expect(withThrowing.confidence).toBe(heuristic.confidence);
  });

  it("falls back to the heuristic when the strategy returns an invalid proposal", () => {
    const invalid = new StaticDirectorStrategy(
      { ruleId: "totally-invalid", recommendedSceneId: "panel", confidence: 99 },
      "invalid"
    );

    const heuristic = planAutoProduction(initialProduction, panelSnapshot, 8000);
    const withInvalid = planAutoProduction(initialProduction, panelSnapshot, 8000, invalid);

    expect(withInvalid.recommendedSceneId).toBe(heuristic.recommendedSceneId);
    expect(withInvalid.ruleId).toBe(heuristic.ruleId);
  });

  it("still gates a valid-but-unsafe proposal behind the anti-thrash hold", () => {
    // A confident strategy proposes jumping straight to speaker-slides, but the
    // screen-share enter hold must still apply: no instant take.
    const eager = new StaticDirectorStrategy(
      { ruleId: "screen-share-priority", recommendedSceneId: "speaker-slides", confidence: 100 },
      "eager"
    );
    const screenShare = meetingSnapshot(true);

    const first = planAutoProduction(
      { ...initialProduction, activeSceneId: "panel", previewSceneId: "panel" },
      screenShare,
      1000,
      eager
    );

    expect(first.action).toBe("hold");
    expect(first.pendingSceneId).toBe("speaker-slides");
    expect(first.holdReason).toBe("Screen share stabilization");

    const tooSoon = planAutoProduction(
      { ...initialProduction, activeSceneId: "panel", previewSceneId: "panel", autoProduction: first },
      screenShare,
      1000 + SCREEN_SHARE_ENTER_HOLD_MS - 100,
      eager
    );
    expect(tooSoon.action).toBe("hold");

    const settled = planAutoProduction(
      { ...initialProduction, activeSceneId: "panel", previewSceneId: "panel", autoProduction: first },
      screenShare,
      1000 + SCREEN_SHARE_ENTER_HOLD_MS,
      eager
    );
    expect(settled.action).toBe("take");
    expect(settled.recommendedSceneId).toBe("speaker-slides");
  });

  it("still gates an unsafe scene-change proposal behind the scene-change hold", () => {
    // Strategy insists on intro; with no screen share involved this must wait
    // out the generic scene-change hold rather than cut immediately.
    const eager = new StaticDirectorStrategy(
      { ruleId: "single-speaker", recommendedSceneId: "intro", confidence: 100 },
      "eager-intro"
    );

    const first = planAutoProduction(
      { ...initialProduction, activeSceneId: "panel", previewSceneId: "panel" },
      panelSnapshot,
      1000,
      eager
    );
    expect(first.action).toBe("hold");
    expect(first.pendingSceneId).toBe("intro");

    const tooSoon = planAutoProduction(
      { ...initialProduction, activeSceneId: "panel", previewSceneId: "panel", autoProduction: first },
      panelSnapshot,
      1000 + SCENE_CHANGE_HOLD_MS - 200,
      eager
    );
    expect(tooSoon.action).toBe("hold");

    const settled = planAutoProduction(
      { ...initialProduction, activeSceneId: "panel", previewSceneId: "panel", autoProduction: first },
      panelSnapshot,
      1000 + SCENE_CHANGE_HOLD_MS,
      eager
    );
    expect(settled.action).toBe("take");
    expect(settled.recommendedSceneId).toBe("intro");
  });

  it("honors a valid strategy proposal once it has passed the holds", () => {
    const valid = new StaticDirectorStrategy(
      { ruleId: "single-speaker", recommendedSceneId: "intro", confidence: 95 },
      "valid"
    );

    const recommendation = planAutoProduction(
      {
        ...initialProduction,
        activeSceneId: "panel",
        previewSceneId: "panel",
        autoProduction: {
          ...initialProduction.autoProduction,
          pendingSceneId: "intro",
          holdStartedAtMs: 0,
          holdElapsedMs: SCENE_CHANGE_HOLD_MS + 1000
        }
      },
      panelSnapshot,
      SCENE_CHANGE_HOLD_MS + 2000,
      valid
    );

    expect(recommendation.recommendedSceneId).toBe("intro");
    expect(recommendation.action).toBe("take");
    expect(recommendation.ruleId).toBe("single-speaker");
  });
});