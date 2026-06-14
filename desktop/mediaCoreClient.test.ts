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

  it("answers pings", async () => {
    const supervisor = makeSupervisor();
    await supervisor.start();
    expect(await supervisor.ping()).toBe(true);
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

  it("rejects requests after being stopped", async () => {
    const supervisor = makeSupervisor();
    await supervisor.start();
    supervisor.stop();
    await expect(supervisor.ping()).rejects.toThrow(/not running/);
  });
});
