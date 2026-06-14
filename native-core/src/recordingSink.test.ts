import { describe, expect, it } from "vitest";
import { RecordingSink } from "./recordingSink.js";
import type { MediaCoreFrame } from "./protocol.js";

const frames: MediaCoreFrame[] = [
  {
    sourceId: "participant:p1",
    participantId: "p1",
    kind: "participant-video",
    frameNumber: 1,
    timestampMs: 33,
    width: 1280,
    height: 720,
    fps: 60,
    health: "live"
  },
  {
    sourceId: "participant:p2",
    participantId: "p2",
    kind: "participant-video",
    frameNumber: 1,
    timestampMs: 33,
    width: 1280,
    height: 720,
    fps: 60,
    health: "live"
  },
  {
    sourceId: "screen-share:slides",
    kind: "screen-share",
    frameNumber: 1,
    timestampMs: 33,
    width: 1920,
    height: 1080,
    fps: 30,
    health: "live"
  }
];

describe("RecordingSink", () => {
  it("creates program and ISO recording paths", () => {
    const sink = new RecordingSink();

    expect(sink.sync(["p1", "p2"], 1000)).toMatchObject({
      active: true,
      status: "recording",
      startedAtMs: 1000,
      programPath: "Recordings/CoreVideo Pro/native-core/program-1000.mp4",
      streams: [
        { kind: "program", path: "Recordings/CoreVideo Pro/native-core/program-1000.mp4" },
        { kind: "iso", participantId: "p1", path: "Recordings/CoreVideo Pro/native-core/iso-p1-1000.mp4" },
        { kind: "iso", participantId: "p2", path: "Recordings/CoreVideo Pro/native-core/iso-p2-1000.mp4" }
      ]
    });
  });

  it("counts written program and ISO frames", () => {
    const sink = new RecordingSink();
    sink.sync(["p1"], 0);

    expect(sink.writeFrames(frames, 33)).toMatchObject({
      elapsedMs: 33,
      streams: [
        { kind: "program", framesWritten: 3 },
        { kind: "iso", participantId: "p1", framesWritten: 1 }
      ],
      totalFramesWritten: 4
    });
  });

  it("clears recording state when stopped", () => {
    const sink = new RecordingSink();
    sink.sync(["p1"], 0);
    sink.stop();

    expect(sink.snapshot()).toBeUndefined();
  });
});
