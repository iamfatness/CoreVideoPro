import { describe, expect, it } from "vitest";
import { initialProduction, type ProductionState } from "../domain/production";
import type { NativeHostBridge } from "./nativeHostBridge";
import {
  buildFallbackZoomMediaSpineSnapshot,
  NativeHostZoomMediaSpineSyncEngine,
  type ZoomMediaSpineNativeSnapshot
} from "./zoomMediaSpineNativeSync";
import { buildZoomMediaSpineSyncPayload, type ZoomMediaSpineSyncPayload } from "./zoomMediaSpineSync";
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

describe("NativeHostZoomMediaSpineSyncEngine", () => {
  it("hands production sync payloads to the native host Zoom media spine bridge", async () => {
    const calls: Array<{ payload: ZoomMediaSpineSyncPayload; elapsedMs: number }> = [];
    const nativeSnapshot: ZoomMediaSpineNativeSnapshot = {
      meetingState: "in-meeting",
      sdkVersion: "6.2.0",
      participantCount: 7,
      activeSpeakerId: "p2",
      screenShareParticipantId: "p2",
      participants: [],
      subscriptions: [],
      recording: {
        evidence: {
          programFramesWritten: 2,
          isoFramesWritten: 2,
          audioPacketsObserved: 4,
          subscribedVideoFeeds: 1
        }
      },
      warnings: [],
      events: ["native bridge accepted payload"]
    };
    const bridge: NativeHostBridge = {
      platform: "win32",
      host: "test-host",
      async request(command) {
        return {
          id: command.id,
          ok: false,
          error: {
            code: "native-unavailable",
            message: "Not used by this test."
          }
        };
      },
      async syncZoomMediaSpine(payload, elapsedMs) {
        calls.push({ payload, elapsedMs });
        return nativeSnapshot;
      }
    };
    const engine = new NativeHostZoomMediaSpineSyncEngine(bridge);
    const snapshot = await engine.syncProduction({ ...initialProduction, recording: true }, readyInput, 66);

    expect(snapshot).toBe(nativeSnapshot);
    expect(calls).toHaveLength(1);
    expect(calls[0].elapsedMs).toBe(66);
    expect(calls[0].payload).toMatchObject({
      blocked: false,
      participants: expect.arrayContaining([expect.objectContaining({ sdkUserId: "p2", talking: true, sharingScreen: true })]),
      subscriptions: expect.arrayContaining([
        { participantId: "p2", kind: "screen-share", purpose: "screen-share", priority: 8 },
        { participantId: "p2", kind: "participant-video", purpose: "active-speaker", priority: 10 }
      ]),
      recording: {
        isoParticipantIds: ["p1", "p2"]
      }
    });
  });

  it("returns a renderer-side proof snapshot when the native bridge hook is missing", async () => {
    const bridge: NativeHostBridge = {
      platform: "win32",
      host: "test-host",
      async request(command) {
        return {
          id: command.id,
          ok: false,
          error: {
            code: "native-unavailable",
            message: "Not used by this test."
          }
        };
      }
    };
    const engine = new NativeHostZoomMediaSpineSyncEngine(bridge);
    const snapshot = await engine.syncProduction({ ...initialProduction, recording: true }, readyInput, 99);

    expect(snapshot).toMatchObject({
      meetingState: "in-meeting",
      sdkVersion: "6.2.0",
      participantCount: 7,
      activeSpeakerId: "p2",
      screenShareParticipantId: "p2",
      recording: {
        session: {
          active: true,
          filenamePrefix: "Q2_Product_Update"
        },
        evidence: {
          programFramesWritten: 3,
          isoFramesWritten: 6,
          subscribedVideoFeeds: 2
        }
      },
      warnings: ["Native host has no Zoom media spine bridge; using renderer-side sync proof."]
    });
  });
});

describe("buildFallbackZoomMediaSpineSnapshot", () => {
  it("surfaces raw media readiness blockers as failed native subscriptions", () => {
    const payload = buildZoomMediaSpineSyncPayload(initialProduction, {
      ...readyInput,
      rawAudioEnabled: false
    });
    const snapshot = buildFallbackZoomMediaSpineSnapshot(payload, 33);

    expect(snapshot.meetingState).toBe("error");
    expect(snapshot.subscriptions).toContainEqual(
      expect.objectContaining({
        participantId: "p1",
        kind: "participant-audio",
        status: "failed",
        lastResultCode: "raw-audio-unavailable",
        warning: "Raw mixed and isolated audio callbacks are disabled; enable raw audio in the Zoom SDK helper before live audio validation."
      })
    );
    expect(snapshot.recording.evidence.rawAudioByParticipant).toContainEqual(
      expect.objectContaining({
        participantId: "p1",
        status: "failed",
        lastResultCode: "raw-audio-unavailable",
        audioPacketsReceived: 0
      })
    );
  });

  it("records raw audio packet evidence per participant", () => {
    const payload = buildZoomMediaSpineSyncPayload(initialProduction, readyInput);
    const snapshot = buildFallbackZoomMediaSpineSnapshot(payload, 100);

    expect(snapshot.recording.evidence.audioPacketsObserved).toBeGreaterThan(0);
    expect(snapshot.recording.evidence.rawAudioByParticipant).toEqual(
      expect.arrayContaining([
        expect.objectContaining({
          participantId: "p1",
          displayName: "Sophia Martinez",
          status: "subscribed",
          lastResultCode: "ok",
          audioPacketsReceived: 5
        }),
        expect.objectContaining({
          participantId: "p2",
          displayName: "David Chen",
          status: "subscribed",
          lastResultCode: "ok",
          audioPacketsReceived: 5
        })
      ])
    );
  });

  it("keeps low-resolution Zoom subscriptions degraded instead of silently passing them", () => {
    const state = {
      ...initialProduction,
      activeSceneId: "panel",
      recording: true,
      recordingSettings: {
        ...initialProduction.recordingSettings,
        isoParticipantIds: ["p6"]
      }
    } satisfies ProductionState;
    const payload = buildZoomMediaSpineSyncPayload(state, readyInput);
    const snapshot = buildFallbackZoomMediaSpineSnapshot(payload, 33);

    expect(snapshot.subscriptions).toContainEqual(
      expect.objectContaining({
        participantId: "p6",
        kind: "participant-video",
        status: "degraded",
        lastResultCode: "low-resolution",
        deliveredHeight: 360
      })
    );
  });
});
