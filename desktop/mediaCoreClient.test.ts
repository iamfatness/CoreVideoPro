import { afterEach, describe, expect, it } from "vitest";
import { MediaCoreSupervisor } from "./mediaCoreClient.ts";

const supervisors: MediaCoreSupervisor[] = [];

function makeSupervisor(options = {}) {
  const supervisor = new MediaCoreSupervisor(options);
  supervisors.push(supervisor);
  return supervisor;
}

afterEach(() => {
  while (supervisors.length > 0) {
    supervisors.pop()?.stop();
  }
});

describe("MediaCoreSupervisor", () => {
  it("spawns the stub and completes the capability handshake", async () => {
    const supervisor = makeSupervisor();
    const profile = await supervisor.start();
    expect(profile?.renderer).toBe("vulkan");
    expect(supervisor.getProfile()).toBe(profile);
    expect(supervisor.running).toBe(true);
  });

  it("round-trips media-core commands and returns synthetic health", async () => {
    const supervisor = makeSupervisor();
    await supervisor.start();
    const snapshot = await supervisor.syncMediaCore(
      [{ type: "load-scene-graph", sceneId: "s", routes: [{ routeId: "r", mode: "active-speaker", audioRole: "mix" }] }],
      1000
    );
    expect(snapshot.sceneId).toBe("s");
    expect(snapshot.programFrame?.health).toBe("live");
  });

  it("round-trips Zoom join, snapshot, and leave through the supervised core", async () => {
    const supervisor = makeSupervisor();
    await supervisor.start();

    const joined = await supervisor.joinZoom({ meetingUrl: "https://zoom.us/j/123", displayName: "Operator", webinar: false });
    expect(joined.meetingState).toBe("in_meeting");
    expect(joined.participants[0]?.displayName).toBe("Operator");

    const snapshot = await supervisor.getZoomSnapshot();
    expect(snapshot.meetingState).toBe("in_meeting");

    const left = await supervisor.leaveZoom();
    expect(left.meetingState).toBe("idle");
    expect(left.participants).toHaveLength(0);
  });

  it("answers pings", async () => {
    const supervisor = makeSupervisor();
    await supervisor.start();
    expect(await supervisor.ping()).toBe(true);
  });

  it("dispatches unsolicited Zoom video frame events", async () => {
    const frames: unknown[] = [];
    const script = `
      const readline = require("node:readline");
      const rl = readline.createInterface({ input: process.stdin });
      rl.on("line", (line) => {
        const request = JSON.parse(line);
        if (request.type === "handshake") {
          console.log(JSON.stringify({
            id: request.id,
            ok: true,
            type: "handshake",
            profile: {
              name: "test",
              renderer: "software",
              maxProgramResolution: "1920x1080",
              maxProgramFps: 30,
              maxParticipantFeeds: 6,
              maxIsoRecordings: 2,
              capabilities: []
            }
          }));
          console.log(JSON.stringify({
            type: "zoom-video-frame",
            frame: {
              participantId: "42",
              width: 2,
              height: 1,
              frameId: 9,
              rgba: [255, 255, 255, 255, 0, 0, 0, 255]
            }
          }));
        }
      });
    `;
    const supervisor = makeSupervisor({
      command: process.execPath,
      args: ["-e", script],
      onZoomVideoFrame: (frame: unknown) => frames.push(frame)
    });

    await supervisor.start();
    await new Promise((resolve) => setTimeout(resolve, 50));

    expect(frames).toHaveLength(1);
    expect(frames[0]).toMatchObject({ participantId: "42", frameId: 9 });
  });

  it("isolates a crash, restarts, and keeps serving requests", async () => {
    const crashes: number[] = [];
    const supervisor = makeSupervisor({ onCrash: (info: { restartCount: number }) => crashes.push(info.restartCount) });
    await supervisor.start();

    await supervisor.forceCrash();
    // Give the supervisor a moment to respawn + re-handshake.
    await new Promise((resolve) => setTimeout(resolve, 300));

    expect(crashes).toContain(1);
    expect(supervisor.restartCount).toBe(1);
    expect(await supervisor.ping()).toBe(true);
  });

  it("round-trips capture-device commands through the supervised stub core", async () => {
    const supervisor = makeSupervisor();
    await supervisor.start();

    const listed = await supervisor.listCaptureDevices();
    expect(listed.length).toBeGreaterThanOrEqual(2);
    expect(listed.some((device) => device.vendor === "blackmagic")).toBe(true);

    const connected = await supervisor.connectCaptureDevice("aja-io-1");
    expect(connected.find((device) => device.id === "aja-io-1")?.connectionState).toBe("connected");
  });

  it("rejects requests after being stopped", async () => {
    const supervisor = makeSupervisor();
    await supervisor.start();
    supervisor.stop();
    await expect(supervisor.ping()).rejects.toThrow(/not running/);
  });
});
