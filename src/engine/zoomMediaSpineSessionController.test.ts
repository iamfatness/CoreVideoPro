import { describe, expect, it } from "vitest";
import { initialProduction, type ProductionState } from "../domain/production";
import type { ZoomMediaSpineNativeSnapshot } from "./zoomMediaSpineNativeSync";
import type { ZoomMediaSpineServiceResponse, ZoomMediaSpineServiceTransport } from "./zoomMediaSpineServiceExecutor";
import type { ZoomMediaSpineServiceRequest } from "./zoomMediaSpineServicePlan";
import { ZoomMediaSpineSessionController } from "./zoomMediaSpineSessionController";
import type { ZoomMediaSpineSyncPayload } from "./zoomMediaSpineSync";
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

function snapshot(request: ZoomMediaSpineServiceRequest): ZoomMediaSpineNativeSnapshot {
  return {
    meetingState: request.type === "zoom-leave" ? "idle" : "in-meeting",
    sdkVersion: "7.0.5.39292",
    participantCount: 8,
    activeSpeakerId: "p2",
    screenShareParticipantId: "p2",
    participants: [],
    subscriptions: [],
    recording: {
      evidence: {
        programFramesWritten: request.type === "zoom-tick" ? 2 : 0,
        isoFramesWritten: request.type === "zoom-tick" ? 2 : 0,
        audioPacketsObserved: request.type === "zoom-tick" ? 4 : 0,
        subscribedVideoFeeds: 1
      }
    },
    warnings: [],
    events: [`handled ${request.type}`]
  };
}

class MemoryZoomServiceTransport implements ZoomMediaSpineServiceTransport {
  readonly requests: ZoomMediaSpineServiceRequest[] = [];
  readonly payloadUpdates: Array<{ payload: ZoomMediaSpineSyncPayload; elapsedMs: number }> = [];
  readonly order: string[] = [];
  failType?: ZoomMediaSpineServiceRequest["type"];

  updatePayload(payload: ZoomMediaSpineSyncPayload, elapsedMs: number): void {
    this.payloadUpdates.push({ payload, elapsedMs });
    this.order.push("payload");
  }

  async request(request: ZoomMediaSpineServiceRequest): Promise<ZoomMediaSpineServiceResponse> {
    this.requests.push(request);
    this.order.push(request.type);
    if (request.type === this.failType) {
      return {
        id: request.id,
        ok: false,
        error: {
          code: "native-failed",
          message: `${request.type} failed`
        }
      };
    }

    return {
      id: request.id,
      ok: true,
      zoom: snapshot(request)
    };
  }
}

describe("ZoomMediaSpineSessionController", () => {
  it("joins production by building payload, plan, and executing helper requests", async () => {
    const transport = new MemoryZoomServiceTransport();
    const controller = new ZoomMediaSpineSessionController(transport);
    const result = await controller.joinProduction({ ...initialProduction, recording: true }, readinessInput, {
      join: {
        meetingNumber: "123456789",
        displayName: "CoreVideo Producer",
        passcodePresent: true
      },
      elapsedMs: 66,
      requestIdPrefix: "join",
      includeSnapshot: true
    });

    expect(result.ok).toBe(true);
    expect(controller.isRecordingActive()).toBe(true);
    expect(transport.requests.map((request) => request.type)).toEqual([
      "zoom-configure",
      "zoom-join",
      "zoom-subscriptions",
      "zoom-start-recording",
      "zoom-tick",
      "zoom-snapshot"
    ]);
    expect(transport.requests[1]).toMatchObject({
      id: "join:02:join",
      type: "zoom-join",
      join: {
        meetingNumber: "123456789",
        displayName: "CoreVideo Producer",
        passcodePresent: true
      }
    });
    expect(transport.order[0]).toBe("payload");
    expect(transport.payloadUpdates).toHaveLength(1);
    expect(transport.payloadUpdates[0].elapsedMs).toBe(66);
    expect(transport.payloadUpdates[0].payload.participants.length).toBe(initialProduction.participants.length);
    expect(transport.payloadUpdates[0].payload.subscriptions.length).toBeGreaterThan(0);
  });

  it("emits stop-recording on sync after recording was previously active", async () => {
    const transport = new MemoryZoomServiceTransport();
    const controller = new ZoomMediaSpineSessionController(transport);
    await controller.joinProduction({ ...initialProduction, recording: true }, readinessInput, {
      join: {
        meetingNumber: "123456789",
        displayName: "CoreVideo Producer"
      },
      elapsedMs: 33
    });

    transport.requests.splice(0);
    const result = await controller.syncProduction(initialProduction, readinessInput, {
      elapsedMs: 66,
      requestIdPrefix: "sync"
    });

    expect(result.ok).toBe(true);
    expect(controller.isRecordingActive()).toBe(false);
    expect(transport.requests.map((request) => request.type)).toEqual([
      "zoom-configure",
      "zoom-roster",
      "zoom-subscriptions",
      "zoom-stop-recording",
      "zoom-tick"
    ]);
  });

  it("does not mark recording active when join execution fails", async () => {
    const transport = new MemoryZoomServiceTransport();
    transport.failType = "zoom-start-recording";
    const controller = new ZoomMediaSpineSessionController(transport);
    const result = await controller.joinProduction({ ...initialProduction, recording: true }, readinessInput, {
      join: {
        meetingNumber: "123456789",
        displayName: "CoreVideo Producer"
      },
      elapsedMs: 33
    });

    expect(result.ok).toBe(false);
    expect(controller.isRecordingActive()).toBe(false);
    expect(result.summary).toBe("zoom-start-recording failed: zoom-start-recording failed");
  });

  it("clears recording state on successful leave", async () => {
    const transport = new MemoryZoomServiceTransport();
    const controller = new ZoomMediaSpineSessionController(transport);
    await controller.joinProduction({ ...initialProduction, recording: true }, readinessInput, {
      join: {
        meetingNumber: "123456789",
        displayName: "CoreVideo Producer"
      },
      elapsedMs: 33
    });

    transport.requests.splice(0);
    const result = await controller.leave({
      elapsedMs: 100,
      includeSnapshot: true,
      requestIdPrefix: "leave"
    });

    expect(result.ok).toBe(true);
    expect(controller.isRecordingActive()).toBe(false);
    expect(transport.requests).toEqual([
      { id: "leave:01:leave", type: "zoom-leave" },
      { id: "leave:02:snapshot", type: "zoom-snapshot" }
    ]);
  });

  it("propagates blocked readiness while still executing diagnostic sync requests", async () => {
    const transport = new MemoryZoomServiceTransport();
    const controller = new ZoomMediaSpineSessionController(transport);
    const blockedInput = {
      ...readinessInput,
      sdkRuntimePresent: false
    };
    const result = await controller.syncProduction(initialProduction, blockedInput, {
      elapsedMs: 0
    });

    expect(result.ok).toBe(false);
    expect(result.blocked).toBe(true);
    expect(result.summary).toBe("Zoom media spine service plan blocked; 1 warning requires attention.");
    expect(transport.requests.map((request) => request.type)).toEqual(["zoom-configure", "zoom-roster", "zoom-subscriptions", "zoom-tick"]);
  });

  it("passes sync options through to breakout-scoped payload building", async () => {
    const transport = new MemoryZoomServiceTransport();
    const controller = new ZoomMediaSpineSessionController(transport);
    const state = {
      ...initialProduction,
      activeSceneId: "panel",
      recording: true,
      recordingSettings: {
        ...initialProduction.recordingSettings,
        isoParticipantIds: ["p1", "p3", "p4"]
      }
    } satisfies ProductionState;

    await controller.syncProduction(state, readinessInput, {
      elapsedMs: 33,
      syncOptions: {
        selectedBreakoutRoomId: "customer-panel"
      }
    });

    expect(transport.requests).toContainEqual(
      expect.objectContaining({
        type: "zoom-start-recording",
        targets: expect.objectContaining({
          isoParticipantIds: ["p3", "p4"]
        })
      })
    );
  });
});
