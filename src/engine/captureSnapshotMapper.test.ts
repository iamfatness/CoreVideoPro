import { describe, expect, it } from "vitest";
import { mapCaptureSnapshot } from "./captureSnapshotMapper";

describe("mapCaptureSnapshot", () => {
  it("normalizes raw Zoom-like participants into app snapshot state", () => {
    const snapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 3,
      activeSpeakerId: 42,
      caption: "Live caption text",
      participants: [
        {
          userId: 42,
          displayName: "  Dana Lee  ",
          role: "Presenter",
          title: "Demo Lead",
          breakoutRoomId: "green-room",
          breakoutRoomName: "Green Room",
          talking: false,
          sharingScreen: true,
          audioLevel: 91,
          networkQuality: "good"
        },
        {
          userId: "guest-7",
          displayName: "",
          videoOn: false,
          networkQuality: "low"
        }
      ]
    });

    expect(snapshot.meetingState).toBe("in_meeting");
    expect(snapshot.elapsedSeconds).toBe(24);
    expect(snapshot.caption).toBe("Live caption text");
    expect(snapshot.activeSpeakerName).toBe("Dana Lee");
    expect(snapshot.participants[0]).toMatchObject({
      id: "42",
      name: "Dana Lee",
      title: "Demo Lead",
      role: "Presenter",
      breakoutRoomId: "green-room",
      breakoutRoomName: "Green Room",
      isActiveSpeaker: true,
      isScreenSharing: true,
      health: "live"
    });
    expect(snapshot.participants[1]).toMatchObject({
      id: "guest-7",
      name: "Zoom User guest-7",
      breakoutRoomId: "main",
      breakoutRoomName: "Main room",
      health: "video-off"
    });
    expect(snapshot.mediaFrames.find((frame) => frame.participantId === "guest-7")).toBeUndefined();
    expect(snapshot.mediaFrames.find((frame) => frame.kind === "screen-share")?.participantId).toBe("42");
  });

  it("maps low and recovering network quality to production feed health", () => {
    const snapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 0,
      participants: [
        { userId: "low", displayName: "Low Net", networkQuality: "low" },
        { userId: "recovering", displayName: "Recovering Net", networkQuality: "recovering" }
      ]
    });

    expect(snapshot.participants.map((participant) => participant.health)).toEqual(["low-resolution", "recovering"]);
  });

  it("emits empty capture data when not in meeting", () => {
    const snapshot = mapCaptureSnapshot({
      meetingState: "idle",
      tick: 12,
      participants: [{ userId: "p1", displayName: "Should Not Appear" }]
    });

    expect(snapshot.participants).toHaveLength(0);
    expect(snapshot.mediaFrames).toHaveLength(0);
    expect(snapshot.screenShareActive).toBe(false);
  });

  it("freezes returned snapshot arrays and frame objects", () => {
    const snapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 0,
      participants: [{ userId: "p1", displayName: "Frozen Person" }]
    });

    expect(Object.isFrozen(snapshot)).toBe(true);
    expect(Object.isFrozen(snapshot.participants)).toBe(true);
    expect(Object.isFrozen(snapshot.participants[0])).toBe(true);
    expect(Object.isFrozen(snapshot.mediaFrames[0])).toBe(true);
    expect(Object.isFrozen(snapshot.mediaFrames[0].crop)).toBe(true);
  });
});
