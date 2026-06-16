import { describe, expect, it } from "vitest";
import { executeZoomMediaSpineServicePlan, type ZoomMediaSpineServiceResponse, type ZoomMediaSpineServiceTransport } from "./zoomMediaSpineServiceExecutor";
import type { ZoomMediaSpineNativeSnapshot } from "./zoomMediaSpineNativeSync";
import type { ZoomMediaSpineServicePlan, ZoomMediaSpineServiceRequest } from "./zoomMediaSpineServicePlan";

const snapshot: ZoomMediaSpineNativeSnapshot = {
  meetingState: "in-meeting",
  sdkVersion: "7.0.5.39292",
  participantCount: 1,
  activeSpeakerId: "p1",
  screenShareParticipantId: "p1",
  participants: [
    {
      sdkUserId: "p1",
      displayName: "Sophia Martinez",
      role: "host",
      videoOn: true,
      muted: false,
      talking: true,
      sharingScreen: true,
      audioLevel: 76,
      networkQuality: "good"
    }
  ],
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
  events: ["Zoom media spine helper returned snapshot."]
};

function plan(overrides: Partial<ZoomMediaSpineServicePlan> = {}): ZoomMediaSpineServicePlan {
  return {
    blocked: false,
    requests: [
      {
        id: "zoom:01:configure",
        type: "zoom-configure",
        config: {
          platform: "windows",
          sdkRuntimePresent: true,
          sdkVersion: "7.0.5.39292",
          appKeyPresent: true,
          oauthConfigured: true,
          jwtBrokerConfigured: true,
          rawVideoEnabled: true,
          rawAudioEnabled: true,
          rawShareEnabled: true
        }
      },
      {
        id: "zoom:02:tick",
        type: "zoom-tick",
        elapsedMs: 66
      }
    ],
    warnings: [],
    summary: "2 Zoom media spine service requests planned.",
    ...overrides
  };
}

describe("executeZoomMediaSpineServicePlan", () => {
  it("executes every request in order and returns the final Zoom snapshot", async () => {
    const requests: ZoomMediaSpineServiceRequest[] = [];
    const transport: ZoomMediaSpineServiceTransport = {
      async request(request) {
        requests.push(request);
        return {
          id: request.id,
          ok: true,
          zoom: {
            ...snapshot,
            events: [`handled ${request.type}`]
          }
        };
      }
    };

    const result = await executeZoomMediaSpineServicePlan(plan(), transport);

    expect(result).toMatchObject({
      ok: true,
      blocked: false,
      summary: "2 Zoom media spine requests executed.",
      finalSnapshot: {
        events: ["handled zoom-tick"]
      },
      steps: [
        { requestId: "zoom:01:configure", requestType: "zoom-configure", ok: true },
        { requestId: "zoom:02:tick", requestType: "zoom-tick", ok: true }
      ],
      warnings: []
    });
    expect(requests.map((request) => request.type)).toEqual(["zoom-configure", "zoom-tick"]);
  });

  it("stops on a failed service response and preserves the last successful snapshot", async () => {
    const responses: ZoomMediaSpineServiceResponse[] = [
      { id: "zoom:01:configure", ok: true, zoom: snapshot },
      {
        id: "zoom:02:tick",
        ok: false,
        error: {
          code: "zoom-runtime-unconfigured",
          message: "Zoom media spine runtime is not configured."
        }
      }
    ];
    const transport: ZoomMediaSpineServiceTransport = {
      async request() {
        return responses.shift() as ZoomMediaSpineServiceResponse;
      }
    };

    const result = await executeZoomMediaSpineServicePlan(plan(), transport);

    expect(result).toMatchObject({
      ok: false,
      blocked: true,
      finalSnapshot: snapshot,
      summary: "zoom-tick failed: Zoom media spine runtime is not configured.",
      steps: [
        { requestId: "zoom:01:configure", requestType: "zoom-configure", ok: true },
        {
          requestId: "zoom:02:tick",
          requestType: "zoom-tick",
          ok: false,
          errorCode: "zoom-runtime-unconfigured",
          message: "Zoom media spine runtime is not configured."
        }
      ],
      warnings: ["Zoom media spine runtime is not configured."]
    });
  });

  it("converts thrown transport errors into failed execution steps", async () => {
    const transport: ZoomMediaSpineServiceTransport = {
      async request(request) {
        if (request.type === "zoom-tick") {
          throw new Error("native-core pipe closed");
        }

        return { id: request.id, ok: true, zoom: snapshot };
      }
    };

    const result = await executeZoomMediaSpineServicePlan(plan(), transport);

    expect(result).toMatchObject({
      ok: false,
      blocked: true,
      summary: "zoom-tick transport error: native-core pipe closed",
      steps: [
        { requestType: "zoom-configure", ok: true },
        { requestType: "zoom-tick", ok: false, errorCode: "transport-error", message: "native-core pipe closed" }
      ],
      warnings: ["native-core pipe closed"]
    });
  });

  it("returns blocked when the plan is blocked even if diagnostic requests execute", async () => {
    const transport: ZoomMediaSpineServiceTransport = {
      async request(request) {
        return { id: request.id, ok: true, zoom: { ...snapshot, warnings: ["Raw audio callbacks are disabled."] } };
      }
    };

    const result = await executeZoomMediaSpineServicePlan(
      plan({
        blocked: true,
        warnings: ["Raw audio callbacks are disabled."],
        summary: "Zoom media spine service plan blocked; 1 warning requires attention."
      }),
      transport
    );

    expect(result).toMatchObject({
      ok: false,
      blocked: true,
      summary: "Zoom media spine service plan blocked; 1 warning requires attention.",
      warnings: ["Raw audio callbacks are disabled.", "Raw audio callbacks are disabled."]
    });
  });

  it("fails if the service plan produces no snapshot response", async () => {
    const result = await executeZoomMediaSpineServicePlan(
      plan({ requests: [] }),
      {
        async request() {
          throw new Error("should not be called");
        }
      }
    );

    expect(result).toMatchObject({
      ok: false,
      blocked: false,
      summary: "Zoom media spine service plan completed without a snapshot.",
      steps: [],
      warnings: []
    });
  });
});
