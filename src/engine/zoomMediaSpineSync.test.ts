import { describe, expect, it } from "vitest";
import { initialProduction, type Participant, type ProductionState } from "../domain/production";
import { buildZoomMediaSpineSyncPayload } from "./zoomMediaSpineSync";
import type { ZoomSdkReadinessInput } from "./zoomSdkReadiness";

const readyInput: ZoomSdkReadinessInput = {
  platform: "windows",
  sdkRuntimePresent: true,
  sdkVersion: "6.2.0",
  appKeyPresent: true,
  oauthConfigured: true,
  jwtBrokerConfigured: true,
  rawVideoEnabled: true,
  rawAudioEnabled: true,
  rawShareEnabled: true
};

describe("buildZoomMediaSpineSyncPayload", () => {
  it("builds the native helper payload from production participants and raw subscription planning", () => {
    const state = {
      ...initialProduction,
      recording: true
    } satisfies ProductionState;

    const payload = buildZoomMediaSpineSyncPayload(state, readyInput);

    expect(payload.blocked).toBe(false);
    expect(payload.summary).toBe("7 Zoom participants, 2 video, 1 screen-share, 7 audio raw Zoom subscriptions planned.");
    expect(payload.participants).toEqual(
      expect.arrayContaining([
        expect.objectContaining({
          sdkUserId: "p1",
          displayName: "Sophia Martinez",
          role: "host",
          videoOn: true,
          muted: false,
          talking: false,
          networkQuality: "good"
        }),
        expect.objectContaining({
          sdkUserId: "p6",
          role: "guest",
          muted: true,
          networkQuality: "low"
        })
      ])
    );
    expect(payload.subscriptions).toEqual(
      expect.arrayContaining([
        { participantId: "p2", kind: "screen-share", purpose: "screen-share", priority: 8 },
        { participantId: "p2", kind: "participant-video", purpose: "active-speaker", priority: 10 },
        { participantId: "p1", kind: "participant-video", purpose: "iso", priority: 30 },
        { participantId: "p1", kind: "participant-audio", purpose: "mix", priority: 40 }
      ])
    );
    expect(payload.recording).toEqual({
      targetFolder: "Recordings/CoreVideo Pro",
      filenamePrefix: "Q2_Product_Update",
      format: "mp4",
      quality: "high",
      isoParticipantIds: ["p1", "p2"]
    });
  });

  it("filters participants, subscriptions, and recording ISOs to the selected breakout room", () => {
    const state = {
      ...initialProduction,
      activeSceneId: "panel",
      selectedBreakoutRoomId: "customer-panel",
      recording: true,
      recordingSettings: {
        ...initialProduction.recordingSettings,
        isoParticipantIds: ["p1", "p6", "p7"]
      }
    } satisfies ProductionState;

    const payload = buildZoomMediaSpineSyncPayload(state, readyInput);

    expect(payload.participants.map((participant) => participant.sdkUserId)).toEqual(["p6", "p7"]);
    expect(payload.subscriptions.every((subscription) => ["p6", "p7"].includes(subscription.participantId))).toBe(true);
    expect(payload.recording?.isoParticipantIds).toEqual(["p6", "p7"]);
    expect(payload.warnings).toContain("Participant p1 is not available for iso video.");
    expect(payload.warnings).toContain("Some requested ISO participants are outside the selected Zoom room or unavailable.");
  });

  it("passes readiness blockers and suppresses recording targets when not recording", () => {
    const payload = buildZoomMediaSpineSyncPayload(initialProduction, {
      ...readyInput,
      sdkRuntimePresent: false,
      rawAudioEnabled: false
    });

    expect(payload.blocked).toBe(true);
    expect(payload.recording).toBeUndefined();
    expect(payload.warnings).toEqual([
      "Zoom Meeting SDK runtime is missing from the packaged helper process.",
      "Raw mixed and isolated audio callbacks are disabled; enable raw audio in the Zoom SDK helper before live audio validation."
    ]);
  });

  it("maps camera-off participants as unavailable video while preserving audio mix subscriptions", () => {
    const state = {
      ...initialProduction,
      participants: initialProduction.participants.map((participant): Participant =>
        participant.id === "p2" ? { ...participant, health: "video-off", isScreenSharing: false } : participant
      )
    } satisfies ProductionState;

    const payload = buildZoomMediaSpineSyncPayload(state, readyInput);

    expect(payload.participants.find((participant) => participant.sdkUserId === "p2")).toMatchObject({
      videoOn: false,
      networkQuality: "good"
    });
    expect(payload.subscriptions.some((subscription) => subscription.participantId === "p2" && subscription.kind === "participant-video")).toBe(false);
    expect(payload.subscriptions).toContainEqual({ participantId: "p2", kind: "participant-audio", purpose: "mix", priority: 41 });
    expect(payload.warnings).toContain("David Chen cannot provide active-speaker video because camera is off.");
  });
});
