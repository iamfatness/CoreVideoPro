import { afterEach, describe, expect, it } from "vitest";
import { resolve } from "node:path";
import { MediaCoreServiceClient } from "./client.js";
import type { MediaCoreCommand } from "./protocol.js";

const clients: MediaCoreServiceClient[] = [];
const nativeCoreRoot = resolve(__dirname, "..");
const workspaceRoot = resolve(nativeCoreRoot, "..");

describe("MediaCoreServiceClient", () => {
  afterEach(() => {
    clients.splice(0).forEach((client) => client.close());
  });

  it("syncs commands through the native-core service process", async () => {
    const client = new MediaCoreServiceClient({
      command: process.execPath,
      args: [resolve(workspaceRoot, "node_modules/tsx/dist/cli.mjs"), "src/service.ts"],
      cwd: nativeCoreRoot,
      requestTimeoutMs: 10000
    });
    clients.push(client);

    const commands: MediaCoreCommand[] = [
      {
        type: "set-zoom-source-roster",
        sources: [
          {
            sourceId: "participant:p1",
            participantId: "p1",
            displayName: "Maya Chen",
            role: "Host",
            breakoutRoomId: "main",
            breakoutRoomName: "Main room",
            hasVideo: true,
            hasAudio: true,
            isMuted: false,
            isActiveSpeaker: true,
            isScreenSharing: true,
            audioLevel: 64,
            health: "live"
          }
        ]
      },
      { type: "set-active-speaker", participantId: "p1" },
      { type: "set-screen-share-source", participantId: "p1" },
      {
        type: "load-scene-graph",
        sceneId: "panel",
        routes: [
          { routeId: "active", mode: "active-speaker", audioRole: "mix" },
          { routeId: "screen", mode: "screen-share", audioRole: "audience" }
        ]
      },
      {
        type: "set-output-profile",
        profileId: "1080p30",
        resolution: "1920x1080",
        width: 1920,
        height: 1080,
        fps: 30,
        targetBitrateMbps: 6
      },
      {
        type: "start-program-output",
        destinations: ["recording", "srt"],
        isoParticipantIds: ["p1"]
      },
      {
        type: "set-recording-targets",
        targetFolder: "Recordings/CoreVideo Pro/native-core",
        filenamePrefix: "program",
        format: "mp4",
        quality: "high",
        isoParticipantIds: ["p1"]
      },
      {
        type: "start-recording-session",
        sessionId: "client-session",
        isoParticipantIds: ["p1"]
      }
    ];

    const sync = await client.sync(commands);
    expect(sync).toMatchObject({
      ok: true,
      appliedCommandCount: 8,
      state: {
        sceneId: "panel",
        routeCount: 2,
        outputProfile: {
          profileId: "1080p30",
          fps: 30,
          targetBitrateMbps: 6
        },
        outputs: ["recording", "srt"],
        isoParticipantIds: ["p1"],
        recording: {
          sessionId: "client-session",
          active: true,
          programPath: "Recordings/CoreVideo Pro/native-core/program-program-0.mp4",
          streams: [
            { kind: "program", framesWritten: 1 },
            { kind: "iso", participantId: "p1", framesWritten: 1 }
          ]
        }
      }
    });

    const snapshot = await client.snapshot();
    expect(snapshot).toMatchObject({
      ok: true,
      state: {
        sceneId: "panel",
        routeCount: 2,
        outputs: ["recording", "srt"]
      }
    });

    const tick = await client.tick(33);
    expect(tick).toMatchObject({
      ok: true,
      appliedCommandCount: 0,
      state: {
        frameCount: 2,
        frames: [
          {
            sourceId: "participant:p1",
            frameNumber: 2,
            timestampMs: 33
          },
          {
            sourceId: "screen-share:p1",
            frameNumber: 2,
            timestampMs: 33
          }
        ],
        recording: {
          elapsedMs: 33,
          streams: [
            { kind: "program", framesWritten: 2 },
            { kind: "iso", participantId: "p1", framesWritten: 2 }
          ],
          totalFramesWritten: 4
        }
      }
    });
  });

  it("drives Zoom media spine requests through the native-core service process", async () => {
    const client = new MediaCoreServiceClient({
      command: process.execPath,
      args: [resolve(workspaceRoot, "node_modules/tsx/dist/cli.mjs"), "src/service.ts"],
      cwd: nativeCoreRoot,
      requestTimeoutMs: 10000
    });
    clients.push(client);

    await expect(
      client.configureZoom({
        platform: "windows",
        sdkRuntimePresent: true,
        sdkVersion: "6.2.0",
        appKeyPresent: true,
        oauthConfigured: true,
        jwtBrokerConfigured: true,
        rawVideoEnabled: true,
        rawAudioEnabled: true,
        rawShareEnabled: true
      })
    ).resolves.toMatchObject({
      ok: true,
      zoom: {
        meetingState: "idle",
        sdkVersion: "6.2.0"
      }
    });

    await expect(
      client.joinZoom(
        { meetingNumber: "123456789", displayName: "Producer" },
        [
          {
            sdkUserId: "sdk-presenter",
            displayName: "Andre Wallace",
            role: "panelist",
            videoOn: true,
            muted: false,
            talking: true,
            sharingScreen: true,
            audioLevel: 82,
            networkQuality: "good"
          }
        ]
      )
    ).resolves.toMatchObject({
      ok: true,
      zoom: {
        meetingState: "in-meeting",
        participantCount: 1,
        activeSpeakerId: "sdk-presenter"
      }
    });

    await client.syncZoomSubscriptions([
      { participantId: "sdk-presenter", kind: "participant-video", purpose: "program", priority: 1 },
      { participantId: "sdk-presenter", kind: "participant-audio", purpose: "mix", priority: 2 }
    ]);
    await client.startZoomRecording({ filenamePrefix: "client-zoom-proof", isoParticipantIds: ["sdk-presenter"] });
    const tick = await client.tickZoom(33);

    expect(tick).toMatchObject({
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

    const leave = await client.leaveZoom();
    expect(leave).toMatchObject({
      ok: true,
      zoom: {
        meetingState: "idle",
        participantCount: 0,
        subscriptions: []
      }
    });
  });
});
