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

  it("rejects messages that carry a request id", () => {
    const line = JSON.stringify({
      id: "x",
      type: "zoom-video-frame",
      frame: { participantId: "p1", width: 1, height: 1, frameId: 1, rgba: [0, 0, 0, 255] }
    });
    expect(parseCoreEvent(line)).toBeNull();
  });

  it("rejects unknown event types and malformed fields", () => {
    expect(parseCoreEvent(JSON.stringify({ type: "something-else" }))).toBeNull();
    expect(
      parseCoreEvent(
        JSON.stringify({
          type: "zoom-video-frame",
          frame: { participantId: "p1", width: "320", height: 180, frameId: 1, rgba: [0, 0, 0, 255] }
        })
      )
    ).toBeNull();
    expect(parseCoreEvent("not json")).toBeNull();
    expect(parseCoreEvent("   ")).toBeNull();
  });

  it("does not parse a response line as an event, and vice versa", () => {
    const responseLine = JSON.stringify({ id: "1", ok: true, type: "ping" });
    expect(parseCoreEvent(responseLine)).toBeNull();
    expect(parseCoreResponse(responseLine)).not.toBeNull();

    const eventLine = JSON.stringify({
      type: "zoom-video-frame",
      frame: { participantId: "p1", width: 2, height: 2, frameId: 1, rgba: [0, 0, 0, 255] }
    });
    expect(parseCoreResponse(eventLine)).toBeNull();
    expect(parseCoreEvent(eventLine)).not.toBeNull();
  });
});