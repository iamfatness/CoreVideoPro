import { describe, expect, it } from "vitest";
import { handleLine } from "./service.js";

describe("native-core service", () => {
  it("handles sync requests from a JSON line", () => {
    const response = handleLine(
      JSON.stringify({
        id: "line-1",
        type: "sync",
        commands: [
          {
            type: "load-scene-graph",
            sceneId: "interview",
            routes: [{ routeId: "guest", mode: "active-speaker", audioRole: "mix" }]
          }
        ]
      })
    );

    expect(response).toMatchObject({
      id: "line-1",
      ok: true,
      appliedCommandCount: 1,
      state: {
        sceneId: "interview",
        routeCount: 1
      }
    });
  });

  it("rejects malformed JSON lines", () => {
    expect(handleLine("{")).toMatchObject({
      id: "unknown",
      ok: false,
      error: {
        code: "invalid-request"
      }
    });
  });

  it("handles tick requests from a JSON line", () => {
    handleLine(
      JSON.stringify({
        id: "line-2",
        type: "sync",
        commands: [
          {
            type: "set-zoom-source-roster",
            sources: [
              {
                sourceId: "participant:p1",
                participantId: "p1",
                displayName: "Sophia Martinez",
                role: "Host",
                breakoutRoomId: "main",
                breakoutRoomName: "Main room",
                hasVideo: true,
                hasAudio: true,
                isMuted: false,
                isActiveSpeaker: true,
                isScreenSharing: false,
                audioLevel: 64,
                health: "live"
              }
            ]
          },
          { type: "set-active-speaker", participantId: "p1" },
          {
            type: "load-scene-graph",
            sceneId: "panel",
            routes: [{ routeId: "active", mode: "active-speaker", audioRole: "mix" }]
          }
        ]
      })
    );
    const response = handleLine(JSON.stringify({ id: "tick-1", type: "tick", elapsedMs: 100 }));

    expect(response).toMatchObject({
      id: "tick-1",
      ok: true,
      appliedCommandCount: 0,
      state: {
        frameCount: 1
      }
    });
  });

  it("handles Zoom media spine requests from JSON lines", () => {
    const configure = handleLine(
      JSON.stringify({
        id: "zoom-config-1",
        type: "zoom-configure",
        config: {
          platform: "windows",
          sdkRuntimePresent: true,
          sdkVersion: "6.2.0",
          appKeyPresent: true,
          oauthConfigured: true,
          jwtBrokerConfigured: true,
          rawVideoEnabled: true,
          rawAudioEnabled: true,
          rawShareEnabled: true
        }
      })
    );
    expect(configure).toMatchObject({
      id: "zoom-config-1",
      ok: true,
      zoom: {
        meetingState: "idle",
        sdkVersion: "6.2.0"
      }
    });

    const join = handleLine(
      JSON.stringify({
        id: "zoom-join-1",
        type: "zoom-join",
        join: { meetingNumber: "123456789", displayName: "Producer" },
        participants: [
          {
            sdkUserId: "sdk-presenter",
            displayName: "David Chen",
            role: "panelist",
            videoOn: true,
            muted: false,
            talking: true,
            sharingScreen: true,
            audioLevel: 82,
            networkQuality: "good"
          }
        ]
      })
    );
    expect(join).toMatchObject({
      id: "zoom-join-1",
      ok: true,
      zoom: {
        meetingState: "in-meeting",
        activeSpeakerId: "sdk-presenter",
        screenShareParticipantId: "sdk-presenter"
      }
    });

    const subscriptions = handleLine(
      JSON.stringify({
        id: "zoom-subscribe-1",
        type: "zoom-subscriptions",
        subscriptions: [
          { participantId: "sdk-presenter", kind: "participant-video", purpose: "program", priority: 1 },
          { participantId: "sdk-presenter", kind: "participant-audio", purpose: "mix", priority: 2 }
        ]
      })
    );
    expect(subscriptions).toMatchObject({
      id: "zoom-subscribe-1",
      ok: true,
      zoom: {
        subscriptions: [
          { participantId: "sdk-presenter", kind: "participant-video", status: "subscribed" },
          { participantId: "sdk-presenter", kind: "participant-audio", status: "subscribed" }
        ]
      }
    });

    handleLine(JSON.stringify({ id: "zoom-record-1", type: "zoom-start-recording", targets: { isoParticipantIds: ["sdk-presenter"] } }));
    const tick = handleLine(JSON.stringify({ id: "zoom-tick-1", type: "zoom-tick", elapsedMs: 33 }));
    expect(tick).toMatchObject({
      id: "zoom-tick-1",
      ok: true,
      zoom: {
        recording: {
          evidence: {
            programFramesWritten: 1,
            isoFramesWritten: 1,
            audioPacketsObserved: 1,
            subscribedVideoFeeds: 1
          }
        }
      }
    });
  });

  it("handles the single-shot zoom-media-spine-sync supervisor request", () => {
    const response = handleLine(
      JSON.stringify({
        id: "zoom-spine-sync-1",
        type: "zoom-media-spine-sync",
        elapsedMs: 66,
        payload: {
          readiness: {
            status: "ready",
            platform: "windows",
            sdkVersion: "7.0.5",
            checks: [],
            blockers: [],
            warnings: [],
            summary: "Zoom SDK media path ready on windows."
          },
          participants: [
            {
              sdkUserId: "sdk-presenter",
              displayName: "David Chen",
              role: "panelist",
              videoOn: true,
              muted: false,
              talking: true,
              sharingScreen: false,
              audioLevel: 82,
              networkQuality: "good"
            }
          ],
          subscriptions: [
            { participantId: "sdk-presenter", kind: "participant-video", purpose: "program", priority: 1 },
            { participantId: "sdk-presenter", kind: "participant-audio", purpose: "mix", priority: 2 }
          ],
          recording: {
            targetFolder: "Recordings",
            filenamePrefix: "zoom-spine",
            format: "mp4",
            quality: "high",
            isoParticipantIds: ["sdk-presenter"]
          },
          blocked: false,
          warnings: [],
          summary: "1 Zoom participant, 1 video and 1 audio raw Zoom subscriptions planned."
        }
      })
    );

    expect(response).toMatchObject({
      id: "zoom-spine-sync-1",
      ok: true,
      type: "zoom-media-spine-sync",
      zoom: {
        meetingState: "in-meeting",
        activeSpeakerId: "sdk-presenter",
        recording: {
          evidence: {
            subscribedVideoFeeds: 1,
            programFramesWritten: 1,
            isoFramesWritten: 1,
            audioPacketsObserved: 1
          }
        }
      }
    });
  });

  it("rejects unsupported request types", () => {
    const response = handleLine(JSON.stringify({ id: "bad-type", type: "unsupported" }));

    expect(response).toMatchObject({
      id: "bad-type",
      ok: false,
      error: {
        code: "invalid-request",
        message: "Request type is not supported."
      }
    });
  });
});
