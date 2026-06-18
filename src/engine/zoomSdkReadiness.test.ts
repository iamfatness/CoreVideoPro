import { describe, expect, it } from "vitest";
import { initialProduction, type Participant, type ProductionState } from "../domain/production";
import { assessZoomSdkReadiness, buildZoomRawMediaSubscriptionPlan, shouldBlockZoomJoin } from "./zoomSdkReadiness";
import type { RuntimeEnvironment } from "./runtimeEnvironment";

describe("assessZoomSdkReadiness", () => {
  it("marks the Zoom SDK media path ready when runtime, auth, and raw media are configured", () => {
    const report = assessZoomSdkReadiness({
      platform: "windows",
      sdkRuntimePresent: true,
      sdkVersion: "6.2.0",
      appKeyPresent: true,
      oauthConfigured: true,
      jwtBrokerConfigured: true,
      rawVideoEnabled: true,
      rawAudioEnabled: true,
      rawShareEnabled: true,
      packagingPath: "CoreVideoPro/resources/zoom-runtime"
    });

    expect(report.status).toBe("ready");
    expect(report.blockers).toHaveLength(0);
    expect(report.summary).toBe("Zoom SDK media path ready on windows.");
  });

  it("blocks when required runtime or auth pieces are missing", () => {
    const report = assessZoomSdkReadiness({
      platform: "macos",
      sdkRuntimePresent: false,
      appKeyPresent: false,
      oauthConfigured: true,
      jwtBrokerConfigured: false,
      rawVideoEnabled: true,
      rawAudioEnabled: false,
      rawShareEnabled: true
    });

    expect(report.status).toBe("blocked");
    expect(report.blockers).toEqual([
      "Zoom Meeting SDK runtime is missing from the packaged helper process.",
      "Meeting SDK app key is missing.",
      "SDK JWT broker is not configured.",
      "Raw mixed and isolated audio callbacks are disabled; enable raw audio in the Zoom SDK helper before live audio validation."
    ]);
  });

  it("warns when the SDK version is not reported", () => {
    const report = assessZoomSdkReadiness({
      platform: "windows",
      sdkRuntimePresent: true,
      appKeyPresent: true,
      oauthConfigured: true,
      jwtBrokerConfigured: true,
      rawVideoEnabled: true,
      rawAudioEnabled: true,
      rawShareEnabled: true
    });

    expect(report.status).toBe("warning");
    expect(report.warnings).toEqual(["SDK version is unknown; include it in support bundles before beta."]);
  });
});

describe("shouldBlockZoomJoin", () => {
  const blockedReadiness = assessZoomSdkReadiness({
    platform: "windows",
    sdkRuntimePresent: false,
    appKeyPresent: false,
    oauthConfigured: false,
    jwtBrokerConfigured: false,
    rawVideoEnabled: false,
    rawAudioEnabled: false,
    rawShareEnabled: false
  });

  const mockRuntime: RuntimeEnvironment = {
    status: "mock",
    label: "Mock studio",
    host: "browser-preview",
    platform: "web",
    warnings: [],
    capabilities: []
  };

  const nativeRuntime: RuntimeEnvironment = {
    status: "ready",
    label: "Native media ready",
    host: "electron",
    platform: "win32",
    warnings: [],
    capabilities: ["zoom-raw-video"]
  };

  it("does not block simulated joins in mock runtime", () => {
    expect(shouldBlockZoomJoin(mockRuntime, blockedReadiness)).toBe(false);
    expect(shouldBlockZoomJoin(undefined, blockedReadiness)).toBe(false);
  });

  it("blocks native joins when SDK readiness is blocked", () => {
    expect(shouldBlockZoomJoin(nativeRuntime, blockedReadiness)).toBe(true);
  });
});

describe("buildZoomRawMediaSubscriptionPlan", () => {
  it("plans speaker, screen-share, ISO, and audio subscriptions for speaker-slides", () => {
    const plan = buildZoomRawMediaSubscriptionPlan(initialProduction);

    expect(plan.summary).toBe("2 video, 1 screen-share, 7 audio raw Zoom subscriptions planned.");
    expect(plan.subscriptions).toEqual(
      expect.arrayContaining([
        expect.objectContaining({ participantId: "p2", kind: "screen-share", purpose: "screen-share" }),
        expect.objectContaining({ participantId: "p2", kind: "participant-video", purpose: "active-speaker" }),
        expect.objectContaining({ participantId: "p1", kind: "participant-video", purpose: "iso" }),
        expect.objectContaining({ participantId: "p1", kind: "participant-audio", purpose: "mix" })
      ])
    );
    expect(plan.blocked).toBe(false);
  });

  it("filters raw subscriptions to the selected breakout room", () => {
    const state = {
      ...initialProduction,
      activeSceneId: "panel",
      selectedBreakoutRoomId: "customer-panel",
      recordingSettings: {
        ...initialProduction.recordingSettings,
        isoParticipantIds: ["p6", "p7"]
      }
    } satisfies ProductionState;

    const plan = buildZoomRawMediaSubscriptionPlan(state);

    expect(plan.audioSubscriptionCount).toBe(2);
    expect(plan.subscriptions.map((subscription) => subscription.participantId).sort()).toEqual(["p6", "p6", "p7", "p7"]);
  });

  it("warns and skips participant video subscriptions when the camera is off", () => {
    const state = {
      ...initialProduction,
      participants: initialProduction.participants.map((participant): Participant =>
        participant.id === "p1" ? { ...participant, health: "video-off" } : participant
      ),
      recordingSettings: {
        ...initialProduction.recordingSettings,
        isoParticipantIds: ["p1"]
      }
    } satisfies ProductionState;

    const plan = buildZoomRawMediaSubscriptionPlan(state);

    expect(plan.subscriptions.some((subscription) => subscription.participantId === "p1" && subscription.kind === "participant-video")).toBe(false);
    expect(plan.warnings).toEqual(["Sophia Martinez cannot provide iso video because camera is off."]);
  });

  it("caps lower-priority video subscriptions when the SDK subscription budget is tight", () => {
    const state = {
      ...initialProduction,
      activeSceneId: "panel",
      recordingSettings: {
        ...initialProduction.recordingSettings,
        isoParticipantIds: ["p1", "p2", "p4", "p5", "p6"]
      }
    } satisfies ProductionState;

    const plan = buildZoomRawMediaSubscriptionPlan(state, { maxVideoSubscriptions: 3 });

    expect(plan.videoSubscriptionCount).toBe(3);
    expect(plan.warnings).toContain("Michael Thompson program video not subscribed; max video subscription limit is 3.");
    expect(plan.warnings).toContain("Ava Patel program video not subscribed; max video subscription limit is 3.");
    expect(plan.warnings).toContain("Robert Smith program video not subscribed; max video subscription limit is 3.");
  });
});
