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
      appliedCommandCount: 5,
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
            { kind: "program", framesWritten: 2 },
            { kind: "iso", participantId: "p1", framesWritten: 0 }
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
            sourceId: "active-speaker:active",
            frameNumber: 2,
            timestampMs: 33
          },
          {
            sourceId: "screen-share:screen",
            frameNumber: 2,
            timestampMs: 33
          }
        ],
        recording: {
          elapsedMs: 33,
          streams: [
            { kind: "program", framesWritten: 4 },
            { kind: "iso", participantId: "p1", framesWritten: 0 }
          ],
          totalFramesWritten: 4
        }
      }
    });
  });
});
