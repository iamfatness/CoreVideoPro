import { describe, expect, it } from "vitest";
import { TestPatternMediaSource } from "./mediaSource.js";

describe("TestPatternMediaSource", () => {
  it("reports subscribed frames and degraded source telemetry", () => {
    const source = new TestPatternMediaSource();
    const sources = Array.from({ length: 5 }, (_, index) => ({
      sourceId: `participant:p${index + 1}`,
      participantId: `p${index + 1}`,
      kind: "participant-video" as const
    }));

    const first = source.render(sources, 1000);

    expect(first.snapshot).toMatchObject({
      adapterId: "test-pattern",
      kind: "test-pattern",
      status: "degraded",
      subscribedSourceCount: 5,
      liveFrameCount: 4,
      lowResolutionFrameCount: 1,
      lastFrameTimestampMs: 1000,
      warnings: ["1 source frame below target resolution."]
    });
    expect(first.frames[4]).toMatchObject({
      sourceId: "participant:p5",
      width: 960,
      height: 540,
      health: "low-resolution"
    });
  });
});
