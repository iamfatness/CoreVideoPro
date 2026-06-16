import { describe, expect, it } from "vitest";
import { initialParticipants } from "../domain/production";
import { buildMediaFrames, getFrameForParticipant } from "./mediaFrames";

describe("media frame modeling", () => {
  it("builds one participant video frame per participant plus active screen share", () => {
    const frames = buildMediaFrames(initialParticipants, 0);
    const expectedVideoCount = initialParticipants.filter((participant) => participant.health !== "video-off").length;

    expect(frames.filter((frame) => frame.kind === "participant-video")).toHaveLength(expectedVideoCount);
    const shareFrame = frames.find((frame) => frame.kind === "screen-share");
    expect(shareFrame?.label).toBe("David Chen screen share");
    expect(shareFrame?.participantId).toBe("p2");
    expect(shareFrame?.sourceId).toBeTruthy();
    expect(shareFrame?.width).toBeGreaterThan(0);
    expect(shareFrame?.fps).toBeGreaterThan(0);
  });

  it("models low-resolution participant feeds distinctly", () => {
    const frames = buildMediaFrames(initialParticipants, 0);
    const priyaFrame = getFrameForParticipant(frames, "p6");

    expect(priyaFrame?.width).toBe(1280);
    expect(priyaFrame?.height).toBe(720);
    expect(priyaFrame?.fps).toBe(24);
    expect(priyaFrame?.health).toBe("low-resolution");
  });

  it("uses tighter smart crop for active speakers", () => {
    const frames = buildMediaFrames(initialParticipants, 0);
    const activeFrame = getFrameForParticipant(frames, "p2");
    const inactiveFrame = getFrameForParticipant(frames, "p1");

    expect(activeFrame?.crop.width).toBeLessThan(inactiveFrame?.crop.width ?? 0);
  });

  it("does not emit invalid 0x0 frames for video-off participants", () => {
    const participants = initialParticipants.map((participant) =>
      participant.id === "p1" ? { ...participant, health: "video-off" as const } : participant
    );

    const frames = buildMediaFrames(participants, 0);

    expect(getFrameForParticipant(frames, "p1")).toBeUndefined();
    expect(frames.every((frame) => frame.width > 0 && frame.height > 0)).toBe(true);
  });
});
