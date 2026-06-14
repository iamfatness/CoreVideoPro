import { describe, expect, it } from "vitest";
import { parseCoreEvent, parseCoreResponse } from "./coreProtocol.ts";

describe("coreProtocol", () => {
  it("distinguishes unsolicited Zoom video frame events from responses", () => {
    const event = parseCoreEvent(
      JSON.stringify({
        type: "zoom-video-frame",
        frame: {
          participantId: "42",
          width: 2,
          height: 1,
          frameId: 7,
          rgba: [255, 255, 255, 255, 0, 0, 0, 255]
        }
      })
    );

    expect(event?.type).toBe("zoom-video-frame");
    expect(event?.frame.participantId).toBe("42");
    expect(event?.frame.frameId).toBe(7);
    expect(event?.frame.rgba).toBeInstanceOf(Uint8ClampedArray);
    expect([...event!.frame.rgba]).toEqual([255, 255, 255, 255, 0, 0, 0, 255]);
    expect(parseCoreResponse(JSON.stringify(event))).toBeNull();
    expect(parseCoreEvent(JSON.stringify({ id: "core-1", ok: true, type: "ping" }))).toBeNull();
  });
});
