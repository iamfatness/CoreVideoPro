import { describe, expect, it } from "vitest";
import { FakeFrameProducer } from "./fakeFrameProducer.js";

describe("FakeFrameProducer", () => {
  it("generates deterministic frame metadata for participant and screen-share sources", () => {
    const producer = new FakeFrameProducer();
    const frames = producer.render(
      [
        { sourceId: "participant:p1", participantId: "p1", kind: "participant-video" },
        { sourceId: "screen-share:slides", kind: "screen-share" }
      ],
      1000
    );

    expect(frames).toEqual([
      {
        sourceId: "participant:p1",
        participantId: "p1",
        kind: "participant-video",
        frameNumber: 1,
        timestampMs: 1000,
        width: 1280,
        height: 720,
        fps: 60,
        health: "live"
      },
      {
        sourceId: "screen-share:slides",
        participantId: undefined,
        kind: "screen-share",
        frameNumber: 1,
        timestampMs: 1000,
        width: 1920,
        height: 1080,
        fps: 30,
        health: "live"
      }
    ]);
  });

  it("increments frame numbers per source", () => {
    const producer = new FakeFrameProducer();
    producer.render([{ sourceId: "participant:p1", participantId: "p1", kind: "participant-video" }], 0);

    expect(producer.render([{ sourceId: "participant:p1", participantId: "p1", kind: "participant-video" }], 33)[0]).toMatchObject({
      frameNumber: 2,
      timestampMs: 33
    });
  });
});
