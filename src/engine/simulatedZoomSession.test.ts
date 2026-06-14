import { describe, expect, it } from "vitest";
import { SimulatedZoomSession } from "./simulatedZoomSession";

describe("SimulatedZoomSession", () => {
  it("starts with an in-meeting snapshot", async () => {
    const session = new SimulatedZoomSession();
    const snapshot = await session.getSnapshot();

    expect(snapshot.meetingState).toBe("in_meeting");
    expect(snapshot.participants).toHaveLength(8);
    expect(snapshot.screenShareActive).toBe(true);
  });

  it("clears participants on leave and restores them on join", async () => {
    const session = new SimulatedZoomSession();

    await session.leave();
    expect((await session.getSnapshot()).participants).toHaveLength(0);

    await session.join({
      meetingUrl: "https://zoom.us/j/123",
      displayName: "Producer",
      webinar: true
    });

    const snapshot = await session.getSnapshot();
    expect(snapshot.meetingState).toBe("in_meeting");
    expect(snapshot.participants).toHaveLength(8);
  });

  it("advances active speaker and captions deterministically", async () => {
    const session = new SimulatedZoomSession();
    const first = await session.getSnapshot();
    const second = await session.advance();

    expect(second.elapsedSeconds).toBe(8);
    expect(second.caption).not.toBe(first.caption);
    expect(second.activeSpeakerName).not.toBe(first.activeSpeakerName);
  });

  it("returns immutable participant snapshots", async () => {
    const session = new SimulatedZoomSession();
    const snapshot = await session.getSnapshot();
    expect(() => snapshot.participants.pop()).toThrow();

    expect((await session.getSnapshot()).participants).toHaveLength(8);
  });
});
