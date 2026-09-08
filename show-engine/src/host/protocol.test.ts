import { describe, expect, it } from "vitest";
import { decodeRequest, encodeLine, PROTOCOL_VERSION } from "./protocol.js";

describe("protocol codec", () => {
  it("round-trips every request type through one line with no newline", () => {
    const requests = [
      { id: "r1", type: "handshake" },
      { id: "r2", type: "invoke", action: "ohg.program.cut", args: [] },
      { id: "r3", type: "zoomEvent", event: { kind: "left", participantId: "p1" } },
      { id: "r4", type: "activeSpeaker", participantId: "p1" },
      { id: "r5", type: "capacity", capacity: 10 },
      { id: "r6", type: "ping" },
      { id: "r7", type: "shutdown" }
    ] as const;
    for (const request of requests) {
      const line = encodeLine(request);
      expect(line).not.toMatch(/[\r\n]/);
      const decoded = decodeRequest(line);
      expect(decoded.kind).toBe("request");
      if (decoded.kind === "request") expect(decoded.request).toEqual(request);
    }
  });

  it("serializes Map args as [key, value] pairs so a C# reader needs no Map type", () => {
    const line = encodeLine({
      event: "hostCommand", generation: 1, seq: 1, name: "applyLook",
      args: [{ lookId: "teatime", scenePreset: "s1", hostSlot: 1, readerSlot: null, boxes: new Map([[1, 2], [2, null]]) }]
    });
    expect(JSON.parse(line).args[0].boxes).toEqual([[1, 2], [2, null]]);
  });

  it("never throws on bigint or circular values — it emits an error line instead", () => {
    const circular: Record<string, unknown> = {};
    circular.self = circular;
    const bigLine = encodeLine({ id: "x", ok: true, value: 10n } as never);
    const circLine = encodeLine({ event: "log", level: "info", message: circular as never });
    expect(JSON.parse(bigLine)).toMatchObject({ id: null, ok: false });
    expect(JSON.parse(circLine)).toMatchObject({ event: "log", level: "error" });
  });

  it("classifies malformed lines with the exact reason and keeps the id when it has one", () => {
    expect(decodeRequest("not json")).toEqual({ kind: "malformed", id: null, reason: expect.stringContaining("JSON") });
    expect(decodeRequest('{"type":"ping"}')).toMatchObject({ kind: "malformed", id: null });
    expect(decodeRequest('{"id":"q","type":"nope"}')).toEqual({ kind: "malformed", id: "q", reason: "unknown request type 'nope'" });
    expect(decodeRequest('{"id":"q","type":"invoke","action":5,"args":[]}')).toMatchObject({ kind: "malformed", id: "q" });
    expect(decodeRequest('{"id":"q","type":"capacity","capacity":"10"}')).toMatchObject({ kind: "malformed", id: "q" });
    // A numeric id is malformed with id: null — the C# side correlates by string ids only.
    expect(decodeRequest('{"id":7,"type":"ping"}')).toEqual({ kind: "malformed", id: null, reason: expect.any(String) });
  });

  it("pins the protocol version", () => {
    expect(PROTOCOL_VERSION).toBe(1);
  });
});
