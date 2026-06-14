import { describe, expect, it } from "vitest";
import { RecordingSink } from "./recordingSink.js";
import type { MediaCoreFrame, MediaCoreProgramFrame } from "./protocol.js";

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

const programFrame: MediaCoreProgramFrame = {
  frameNumber: 1,
  timestampMs: 33,
  renderPlanId: "rp-test",
  sceneId: "speaker-slides",
  width: 1920,
  height: 1080,
  fps: 60,
  layerCount: 3,
  colorGrade: { lut: "none", exposure: 0, contrast: 0, saturation: 0, temperature: 0 },
  health: "live"
};

describe("RecordingSink", () => {
  it("creates program and ISO recording paths", () => {
    const sink = new RecordingSink();

    expect(sink.sync(["p1", "p2"], 1000)).toMatchObject({
      active: true,
      status: "recording",
      startedAtMs: 1000,
      writerStatus: "writing",
      targetFolder: "Recordings/CoreVideo Pro/native-core",
      filenamePrefix: "program",
      programPath: "Recordings/CoreVideo Pro/native-core/program-program-1000.mp4",
      streams: [
        { kind: "program", status: "writing", path: "Recordings/CoreVideo Pro/native-core/program-program-1000.mp4" },
        { kind: "iso", participantId: "p1", status: "writing", path: "Recordings/CoreVideo Pro/native-core/program-iso-p1-1000.mp4" },
        { kind: "iso", participantId: "p2", status: "writing", path: "Recordings/CoreVideo Pro/native-core/program-iso-p2-1000.mp4" }
      ]
    });
  });

  it("counts written program and ISO frames", () => {
    const sink = new RecordingSink();
    sink.sync(["p1"], 0);

    expect(sink.writeFrames(frames, 33, programFrame)).toMatchObject({
      elapsedMs: 33,
      streams: [
        { kind: "program", framesWritten: 1 },
        { kind: "iso", participantId: "p1", framesWritten: 1 }
      ],
      totalFramesWritten: 2
    });
  });

  it("keeps stopped session metadata after stop", () => {
    const sink = new RecordingSink();
    sink.sync(["p1"], 0);
    sink.stop();

    expect(sink.snapshot()).toMatchObject({
      active: false,
      status: "stopped",
      writerStatus: "stopped",
      streams: [
        { kind: "program", status: "stopped" },
        { kind: "iso", participantId: "p1", status: "stopped" }
      ]
    });
  });

  it("captures failed writer state for diagnostics", () => {
    const sink = new RecordingSink();
    sink.start({ isoParticipantIds: ["p1"] }, 10, "show-1");
    sink.fail("Encoder process exited.", 15);

    expect(sink.snapshot()).toMatchObject({
      sessionId: "show-1",
      active: false,
      status: "failed",
      writerStatus: "failed",
      error: "Encoder process exited.",
      streams: [
        { kind: "program", status: "failed" },
        { kind: "iso", participantId: "p1", status: "failed" }
      ]
    });
  });
});
