import { describe, expect, it } from "vitest";
import { initialProduction, type ProductionState } from "../domain/production";
import { buildZoomMediaSpineServicePlan } from "./zoomMediaSpineServicePlan";
import { buildZoomMediaSpineSyncPayload } from "./zoomMediaSpineSync";
import type { ZoomSdkReadinessInput } from "./zoomSdkReadiness";

const readinessInput: ZoomSdkReadinessInput = {
  platform: "windows",
  sdkRuntimePresent: true,
  sdkVersion: "7.0.5.39292",
  appKeyPresent: true,
  oauthConfigured: true,
  jwtBrokerConfigured: true,
  rawVideoEnabled: true,
  rawAudioEnabled: true,
  rawShareEnabled: true
};

describe("buildZoomMediaSpineServicePlan", () => {
  it("plans configure, join, subscriptions, recording, tick, and snapshot in deterministic order", () => {
    const payload = buildZoomMediaSpineSyncPayload({ ...initialProduction, recording: true }, readinessInput);
    const plan = buildZoomMediaSpineServicePlan(payload, readinessInput, {
      mode: "join",
      join: {
        meetingNumber: "123456789",
        displayName: "CoreVideo Producer",
        passcodePresent: true
      },
      elapsedMs: 66,
      requestIdPrefix: "test",
      includeSnapshot: true
    });

    expect(plan.blocked).toBe(false);
    expect(plan.summary).toBe("6 Zoom media spine service requests planned.");
    expect(plan.requests.map((request) => request.type)).toEqual([
      "zoom-configure",
      "zoom-join",
      "zoom-subscriptions",
      "zoom-start-recording",
      "zoom-tick",
      "zoom-snapshot"
    ]);
    expect(plan.requests.map((request) => request.id)).toEqual([
      "test:01:configure",
      "test:02:join",
      "test:03:subscriptions",
      "test:04:start-recording",
      "test:05:tick",
      "test:06:snapshot"
    ]);
    expect(plan.requests[1]).toMatchObject({
      type: "zoom-join",
      join: {
        meetingNumber: "123456789",
        displayName: "CoreVideo Producer",
        passcodePresent: true
      },
      participants: expect.arrayContaining([expect.objectContaining({ sdkUserId: "p2", talking: true, sharingScreen: true })])
    });
    expect(plan.requests[2]).toMatchObject({
      type: "zoom-subscriptions",
      subscriptions: expect.arrayContaining([
        { participantId: "p2", kind: "screen-share", purpose: "screen-share", priority: 8 },
        { participantId: "p2", kind: "participant-video", purpose: "active-speaker", priority: 10 },
        { participantId: "p1", kind: "participant-video", purpose: "iso", priority: 30 }
      ])
    });
    expect(plan.requests[3]).toMatchObject({
      type: "zoom-start-recording",
      targets: {
        filenamePrefix: "AI_Product_Launch_Webinar",
        isoParticipantIds: ["p1", "p2"]
      }
    });
  });

  it("plans roster sync and stop-recording when recording was previously active", () => {
    const payload = buildZoomMediaSpineSyncPayload(initialProduction, readinessInput);
    const plan = buildZoomMediaSpineServicePlan(payload, readinessInput, {
      mode: "sync",
      elapsedMs: 100,
      previousRecordingActive: true
    });

    expect(plan.requests.map((request) => request.type)).toEqual([
      "zoom-configure",
      "zoom-roster",
      "zoom-subscriptions",
      "zoom-stop-recording",
      "zoom-tick"
    ]);
    expect(plan.requests[1]).toMatchObject({
      type: "zoom-roster",
      participants: expect.arrayContaining([expect.objectContaining({ sdkUserId: "p1" })])
    });
    expect(plan.requests[3]).toEqual({
      id: "zoom-spine:04:stop-recording",
      type: "zoom-stop-recording"
    });
  });

  it("preserves blockers but still emits configure and sync requests for diagnostics", () => {
    const blockedInput = {
      ...readinessInput,
      sdkRuntimePresent: false,
      rawAudioEnabled: false
    };
    const payload = buildZoomMediaSpineSyncPayload(initialProduction, blockedInput);
    const plan = buildZoomMediaSpineServicePlan(payload, blockedInput, {
      mode: "sync",
      elapsedMs: 0
    });

    expect(plan.blocked).toBe(true);
    expect(plan.summary).toBe("Zoom media spine service plan blocked; 2 warnings require attention.");
    expect(plan.warnings).toEqual([
      "Zoom Meeting SDK runtime is missing from the packaged helper process.",
      "Raw audio callbacks are disabled."
    ]);
    expect(plan.requests.map((request) => request.type)).toEqual(["zoom-configure", "zoom-roster", "zoom-subscriptions", "zoom-tick"]);
  });

  it("blocks join mode when join parameters are missing", () => {
    const payload = buildZoomMediaSpineSyncPayload(initialProduction, readinessInput);
    const plan = buildZoomMediaSpineServicePlan(payload, readinessInput, {
      mode: "join",
      elapsedMs: 0
    });

    expect(plan.blocked).toBe(true);
    expect(plan.requests).toEqual([
      {
        id: "zoom-spine:01:configure",
        type: "zoom-configure",
        config: readinessInput
      }
    ]);
    expect(plan.warnings).toContain("Zoom join request is missing.");
  });

  it("plans leave as an isolated cleanup sequence", () => {
    const payload = buildZoomMediaSpineSyncPayload(initialProduction, readinessInput);
    const plan = buildZoomMediaSpineServicePlan(payload, readinessInput, {
      mode: "leave",
      elapsedMs: 0,
      includeSnapshot: true
    });

    expect(plan).toEqual({
      blocked: false,
      requests: [
        { id: "zoom-spine:01:leave", type: "zoom-leave" },
        { id: "zoom-spine:02:snapshot", type: "zoom-snapshot" }
      ],
      warnings: [],
      summary: "Zoom media spine leave request planned."
    });
  });

  it("filters service plan recording targets through breakout-room availability", () => {
    const state = {
      ...initialProduction,
      activeSceneId: "panel",
      selectedBreakoutRoomId: "customer-panel",
      recording: true,
      recordingSettings: {
        ...initialProduction.recordingSettings,
        isoParticipantIds: ["p1", "p3", "p4"]
      }
    } satisfies ProductionState;
    const payload = buildZoomMediaSpineSyncPayload(state, readinessInput);
    const plan = buildZoomMediaSpineServicePlan(payload, readinessInput, {
      mode: "sync",
      elapsedMs: 33
    });

    expect(plan.requests).toContainEqual({
      id: "zoom-spine:04:start-recording",
      type: "zoom-start-recording",
      targets: {
        targetFolder: "Recordings/CoreVideo Pro",
        filenamePrefix: "AI_Product_Launch_Webinar",
        format: "mp4",
        quality: "high",
        isoParticipantIds: ["p3", "p4"]
      }
    });
    expect(plan.warnings).toContain("Some requested ISO participants are outside the selected Zoom room or unavailable.");
  });
});
