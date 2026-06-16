import { describe, expect, it } from "vitest";
import {
  ZoomMediaSpineRuntime,
  assessZoomMediaSpineReadiness,
  type ZoomMediaSpineParticipant,
  type ZoomMediaSpineRuntimeConfig
} from "./zoomMediaSpine.js";

const readyConfig: ZoomMediaSpineRuntimeConfig = {
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

const participants: ZoomMediaSpineParticipant[] = [
  {
    sdkUserId: "sdk-host-1",
    displayName: "Sophia Martinez",
    role: "host",
    videoOn: true,
    muted: false,
    talking: false,
    sharingScreen: false,
    audioLevel: 48,
    networkQuality: "good"
  },
  {
    sdkUserId: "sdk-presenter-2",
    displayName: "David Chen",
    role: "panelist",
    videoOn: true,
    muted: false,
    talking: true,
    sharingScreen: true,
    audioLevel: 82,
    networkQuality: "good"
  }
];

describe("assessZoomMediaSpineReadiness", () => {
  it("reports a ready native Zoom media path when SDK, auth, and raw media are configured", () => {
    expect(assessZoomMediaSpineReadiness(readyConfig)).toEqual({
      status: "ready",
      platform: "windows",
      sdkVersion: "6.2.0",
      blockers: []
    });
  });

  it("reports all blockers needed before the helper can join a real meeting", () => {
    const report = assessZoomMediaSpineReadiness({
      ...readyConfig,
      sdkRuntimePresent: false,
      appKeyPresent: false,
      jwtBrokerConfigured: false,
      rawAudioEnabled: false
    });

    expect(report.status).toBe("blocked");
    expect(report.blockers).toEqual([
      "Zoom Meeting SDK runtime is missing.",
      "Meeting SDK app key is missing.",
      "SDK JWT broker is not configured.",
      "Raw audio is disabled."
    ]);
  });
});

describe("ZoomMediaSpineRuntime", () => {
  it("joins with stable SDK participant IDs and derives active speaker and screen share", () => {
    const runtime = new ZoomMediaSpineRuntime(readyConfig);
    const snapshot = runtime.join({ meetingNumber: "123456789", displayName: "Producer" }, participants);

    expect(snapshot.meetingState).toBe("in-meeting");
    expect(snapshot.participantCount).toBe(2);
    expect(snapshot.activeSpeakerId).toBe("sdk-presenter-2");
    expect(snapshot.screenShareParticipantId).toBe("sdk-presenter-2");
    expect(snapshot.events).toContain("Joined Zoom meeting with 2 participants.");
  });

  it("blocks join when the native SDK runtime is not usable", () => {
    const runtime = new ZoomMediaSpineRuntime({ ...readyConfig, sdkRuntimePresent: false });
    const snapshot = runtime.join({ meetingNumber: "123456789", displayName: "Producer" }, participants);

    expect(snapshot.meetingState).toBe("error");
    expect(snapshot.participantCount).toBe(0);
    expect(snapshot.warnings).toEqual(["Zoom Meeting SDK runtime is missing."]);
  });

  it("subscribes raw participant, audio, and screen-share streams with SDK result evidence", () => {
    const runtime = new ZoomMediaSpineRuntime(readyConfig);
    runtime.join({ meetingNumber: "123456789", displayName: "Producer" }, participants);

    const snapshot = runtime.syncSubscriptions([
      { participantId: "sdk-presenter-2", kind: "participant-video", purpose: "active-speaker", priority: 1 },
      { participantId: "sdk-presenter-2", kind: "screen-share", purpose: "screen-share", priority: 2 },
      { participantId: "sdk-host-1", kind: "participant-audio", purpose: "mix", priority: 3 }
    ]);

    expect(snapshot.subscriptions).toEqual(
      expect.arrayContaining([
        expect.objectContaining({ participantId: "sdk-presenter-2", kind: "participant-video", status: "subscribed", lastResultCode: "ok" }),
        expect.objectContaining({ participantId: "sdk-presenter-2", kind: "screen-share", status: "subscribed", deliveredWidth: 1920 }),
        expect.objectContaining({ participantId: "sdk-host-1", kind: "participant-audio", status: "subscribed" })
      ])
    );
  });

  it("surfaces camera-off and low-resolution subscription problems", () => {
    const runtime = new ZoomMediaSpineRuntime(readyConfig);
    runtime.join({ meetingNumber: "123456789", displayName: "Producer" }, [
      { ...participants[0], videoOn: false },
      { ...participants[1], networkQuality: "low" }
    ]);

    const snapshot = runtime.syncSubscriptions([
      { participantId: "sdk-host-1", kind: "participant-video", purpose: "iso", priority: 1 },
      { participantId: "sdk-presenter-2", kind: "participant-video", purpose: "active-speaker", priority: 2 }
    ]);

    expect(snapshot.subscriptions).toEqual(
      expect.arrayContaining([
        expect.objectContaining({ participantId: "sdk-host-1", status: "failed", lastResultCode: "video-off" }),
        expect.objectContaining({ participantId: "sdk-presenter-2", status: "degraded", lastResultCode: "low-resolution", deliveredHeight: 360 })
      ])
    );
    expect(snapshot.warnings).toEqual([
      "Sophia Martinez video is off.",
      "David Chen raw media subscribed below target resolution."
    ]);
  });

  it("proves first media path by ticking subscribed feeds into a recording session", () => {
    const runtime = new ZoomMediaSpineRuntime(readyConfig);
    runtime.join({ meetingNumber: "123456789", displayName: "Producer" }, participants);
    runtime.syncSubscriptions([
      { participantId: "sdk-presenter-2", kind: "participant-video", purpose: "program", priority: 1 },
      { participantId: "sdk-presenter-2", kind: "participant-audio", purpose: "mix", priority: 2 }
    ]);
    runtime.startRecording({
      filenamePrefix: "zoom-proof",
      isoParticipantIds: ["sdk-presenter-2"]
    });

    runtime.tick(33);
    const snapshot = runtime.tick(66);

    expect(snapshot.recording.session?.status).toBe("recording");
    expect(snapshot.recording.evidence).toMatchObject({
      programFramesWritten: 2,
      isoFramesWritten: 2,
      audioPacketsObserved: 2,
      subscribedVideoFeeds: 1
    });
  });

  it("clears stale roster and subscriptions on leave before rejoin", () => {
    const runtime = new ZoomMediaSpineRuntime(readyConfig);
    runtime.join({ meetingNumber: "123456789", displayName: "Producer" }, participants);
    runtime.syncSubscriptions([{ participantId: "sdk-presenter-2", kind: "participant-video", purpose: "program", priority: 1 }]);

    const left = runtime.leave();
    expect(left.meetingState).toBe("idle");
    expect(left.participantCount).toBe(0);
    expect(left.subscriptions).toHaveLength(0);

    const rejoined = runtime.join({ meetingNumber: "123456789", displayName: "Producer" }, [participants[0]]);
    expect(rejoined.participantCount).toBe(1);
    expect(rejoined.subscriptions).toHaveLength(0);
    expect(rejoined.events).toContain("Left Zoom meeting and cleared raw media subscriptions.");
  });

  it("consumes a ZoomMediaSpineSyncPayload in one native supervisor round trip", () => {
    const runtime = new ZoomMediaSpineRuntime(readyConfig);
    const snapshot = runtime.syncPayload(
      {
        readiness: {
          status: "ready",
          platform: "windows",
          sdkVersion: "7.0.5",
          checks: [],
          blockers: [],
          warnings: [],
          summary: "Zoom SDK media path ready on windows."
        },
        participants,
        subscriptions: [
          { participantId: "sdk-presenter-2", kind: "participant-video", purpose: "program", priority: 1 },
          { participantId: "sdk-host-1", kind: "participant-audio", purpose: "mix", priority: 2 }
        ],
        recording: {
          targetFolder: "Recordings",
          filenamePrefix: "zoom-spine",
          format: "mp4",
          quality: "high",
          isoParticipantIds: ["sdk-presenter-2"]
        },
        blocked: false,
        warnings: [],
        summary: "2 Zoom participants, raw Zoom subscriptions planned."
      },
      66
    );

    expect(snapshot.meetingState).toBe("in-meeting");
    expect(snapshot.activeSpeakerId).toBe("sdk-presenter-2");
    expect(snapshot.recording.evidence).toMatchObject({
      subscribedVideoFeeds: 1,
      programFramesWritten: 1,
      isoFramesWritten: 1,
      audioPacketsObserved: 1
    });
    expect(snapshot.events).toContain("Zoom media spine payload accepted by native-core stub.");
  });
});
