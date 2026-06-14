import { describe, expect, it } from "vitest";
import { parseCoreEvent, parseCoreResponse } from "./coreProtocol";

describe("parseCoreEvent", () => {
  it("parses a well-formed zoom-video-frame event", () => {
    const line = JSON.stringify({
      type: "zoom-video-frame",
      participantId: "p1",
      width: 320,
      height: 180,
      frameId: 12,
      rgbaBase64: "AAAA"
    });
    expect(parseCoreEvent(line)).toEqual({
      type: "zoom-video-frame",
      participantId: "p1",
      width: 320,
      height: 180,
      frameId: 12,
      rgbaBase64: "AAAA"
    });
  });

  it("rejects messages that carry a request id (those are responses)", () => {
    const line = JSON.stringify({ id: "x", type: "zoom-video-frame", participantId: "p1", width: 1, height: 1, frameId: 1, rgbaBase64: "AA" });
    expect(parseCoreEvent(line)).toBeNull();
  });

  it("rejects unknown event types and malformed fields", () => {
    expect(parseCoreEvent(JSON.stringify({ type: "something-else" }))).toBeNull();
    expect(parseCoreEvent(JSON.stringify({ type: "zoom-video-frame", participantId: "p1", width: "320", height: 180, frameId: 1, rgbaBase64: "AA" }))).toBeNull();
    expect(parseCoreEvent("not json")).toBeNull();
    expect(parseCoreEvent("   ")).toBeNull();
  });

  it("does not parse a response line as an event, and vice versa", () => {
    const responseLine = JSON.stringify({ id: "1", ok: true, type: "ping" });
    expect(parseCoreEvent(responseLine)).toBeNull();
    expect(parseCoreResponse(responseLine)).not.toBeNull();

    const eventLine = JSON.stringify({ type: "zoom-video-frame", participantId: "p1", width: 2, height: 2, frameId: 1, rgbaBase64: "AA" });
    expect(parseCoreResponse(eventLine)).toBeNull();
    expect(parseCoreEvent(eventLine)).not.toBeNull();
  });
});
