import { describe, expect, it } from "vitest";
import { resetSyntheticBreakoutRoomState, SYNTHETIC_PROFILE, synthesizeSnapshot } from "./syntheticMediaCore.ts";
import { validateNativeMediaCoreProfile } from "../src/engine/nativeMediaCoreProtocol";
import type { NativeMediaCoreCommand } from "../src/engine/nativeMediaCoreProtocol";

describe("syntheticMediaCore", () => {
  it("announces a profile that validates as ready", () => {
    expect(validateNativeMediaCoreProfile(SYNTHETIC_PROFILE).ready).toBe(true);
  });

  it("reports an idle program with no scene graph", () => {
    const snapshot = synthesizeSnapshot([], 0, 1);
    expect(snapshot.programFrame).toBeUndefined();
    expect(snapshot.compositor.status).toBe("idle");
    expect(snapshot.outputs).toEqual([]);
  });

  it("produces a live program frame once a scene graph is loaded", () => {
    const commands: NativeMediaCoreCommand[] = [
      { type: "load-scene-graph", sceneId: "scene-1", routes: [{ routeId: "r1", mode: "active-speaker", audioRole: "mix" }] }
    ];
    const snapshot = synthesizeSnapshot(commands, 500, 7);
    expect(snapshot.sceneId).toBe("scene-1");
    expect(snapshot.routeCount).toBe(1);
    expect(snapshot.programFrame?.health).toBe("live");
    expect(snapshot.programFrame?.frameNumber).toBe(7);
    expect(snapshot.compositor.status).toBe("live");
  });

  it("reflects roster, overlays, transforms and outputs", () => {
    const commands: NativeMediaCoreCommand[] = [
      {
        type: "set-zoom-source-roster",
        sources: [
          { sourceId: "p1", participantId: "p1", displayName: "A", role: "Host", breakoutRoomId: "", breakoutRoomName: "", hasVideo: true, hasAudio: true, isMuted: false, isActiveSpeaker: true, isScreenSharing: false, audioLevel: 0.5, health: "live" }
        ]
      },
      { type: "set-overlay-asset", overlayId: "o1", text: "Hi", position: "lower-third" },
      { type: "set-participant-transform", participantId: "p1", crop: { x: 0, y: 0, width: 1, height: 1 }, scale: 1 },
      { type: "start-program-output", destinations: ["rtmp", "rtmp", "recording"], isoParticipantIds: ["p1"] }
    ];
    const snapshot = synthesizeSnapshot(commands, 1000, 2);
    expect(snapshot.sourceCount).toBe(1);
    expect(snapshot.frameCount).toBe(1);
    expect(snapshot.overlayCount).toBe(1);
    expect(snapshot.participantTransformCount).toBe(1);
    expect(snapshot.outputs).toEqual(["rtmp", "recording"]);
    expect(snapshot.isoParticipantIds).toEqual(["p1"]);
    expect(snapshot.encoderSession.status).toBe("encoding");
  });

  it("records the applied command types", () => {
    const snapshot = synthesizeSnapshot([{ type: "set-active-speaker", participantId: "p1" }], 0, 1);
    expect(snapshot.lastCommandTypes).toEqual(["set-active-speaker"]);
  });

  it("publishes breakout room fields and simulates room changes", () => {
    resetSyntheticBreakoutRoomState();
    const baseline = synthesizeSnapshot(
      [{ type: "load-scene-graph", sceneId: "scene-1", routes: [{ routeId: "r1", mode: "active-speaker", audioRole: "mix" }] }],
      500,
      1
    );
    expect(baseline.meetingState).toBe("in_meeting");
    expect(baseline.breakoutRoomId).toBe("main");
    expect(baseline.breakoutRoomName).toBe("Main room");

    const changed = synthesizeSnapshot(
      [
        {
          type: "simulate-breakout-room-change",
          breakoutRoomId: "customer-panel",
          breakoutRoomName: "Customer panel"
        },
        { type: "load-scene-graph", sceneId: "scene-1", routes: [{ routeId: "r1", mode: "active-speaker", audioRole: "mix" }] }
      ],
      1000,
      2
    );
    expect(changed.breakoutRoomId).toBe("customer-panel");
    expect(changed.breakoutRoomName).toBe("Customer panel");
  });
});
