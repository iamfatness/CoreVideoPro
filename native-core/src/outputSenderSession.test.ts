import { describe, expect, it } from "vitest";
import { OutputSenderSessionModel } from "./outputSenderSession.js";
import type { MediaCoreOutputProfile, MediaCoreProgramFrame } from "./protocol.js";

const outputProfile: MediaCoreOutputProfile = {
  profileId: "1080p60",
  resolution: "1920x1080",
  width: 1920,
  height: 1080,
  fps: 60,
  targetBitrateMbps: 8.2
};

const programFrame: MediaCoreProgramFrame = {
  frameNumber: 1,
  timestampMs: 1000,
  renderPlanId: "rp-test",
  sceneId: "scene-a",
  width: 1920,
  height: 1080,
  fps: 60,
  layerCount: 2,
  colorGrade: { lut: "none", exposure: 0, contrast: 0, saturation: 0, temperature: 0 },
  health: "live"
};

describe("OutputSenderSessionModel", () => {
  it("tracks active network senders and stopped senders separately from recording", () => {
    const model = new OutputSenderSessionModel();

    const live = model.sync(["recording", "rtmp", "ndi"], programFrame, outputProfile, 1000);

    expect(live).toMatchObject({
      status: "live",
      activeSenderCount: 2,
      senders: [
        { destination: "ndi", status: "live", framesSent: 1, latencyMs: 80, bitrateMbps: 13.1 },
        { destination: "rtmp", status: "live", framesSent: 1, latencyMs: 2100, bitrateMbps: 8.2 }
      ]
    });

    const stopped = model.sync(["recording"], programFrame, outputProfile, 1200);

    expect(stopped).toMatchObject({
      status: "idle",
      activeSenderCount: 0,
      senders: [
        { destination: "ndi", status: "stopped", stoppedAtMs: 1200 },
        { destination: "rtmp", status: "stopped", stoppedAtMs: 1200 }
      ]
    });
  });

  it("reports degraded and dropped program frames as sender warnings", () => {
    const model = new OutputSenderSessionModel();
    const degraded = model.sync(["srt"], { ...programFrame, health: "degraded" }, outputProfile, 1000);

    expect(degraded).toMatchObject({
      status: "warning",
      activeSenderCount: 1,
      senders: [{ destination: "srt", status: "warning", framesSent: 1, warning: "SRT sender is publishing degraded program frames." }],
      warnings: ["SRT sender is publishing degraded program frames."]
    });

    const dropped = model.sync(["srt"], { ...programFrame, frameNumber: 2, health: "dropped" }, outputProfile, 1016);

    expect(dropped).toMatchObject({
      status: "warning",
      activeSenderCount: 1,
      senders: [{ destination: "srt", status: "warning", framesSent: 1, retryCount: 1, warning: "SRT sender skipped a dropped program frame." }]
    });
  });

  it("keeps failed senders failed until explicitly recovered", () => {
    const model = new OutputSenderSessionModel();
    model.sync(["rtmp"], programFrame, outputProfile, 1000);

    const failed = model.fail("rtmp", "RTMP connection refused.", 1100);

    expect(failed).toMatchObject({
      status: "failed",
      activeSenderCount: 0,
      senders: [{ destination: "rtmp", status: "failed", retryCount: 1, warning: "RTMP connection refused." }],
      warnings: ["RTMP connection refused."]
    });

    const stillFailed = model.sync(["rtmp"], { ...programFrame, frameNumber: 2 }, outputProfile, 1200);

    expect(stillFailed).toMatchObject({
      status: "failed",
      senders: [{ destination: "rtmp", status: "failed", framesSent: 1, warning: "RTMP connection refused." }]
    });

    model.recover("rtmp", 1300, "RTMP sender recovered after reconnect.");
    const recovered = model.sync(["rtmp"], { ...programFrame, frameNumber: 3 }, outputProfile, 1316);

    expect(recovered).toMatchObject({
      status: "live",
      activeSenderCount: 1,
      senders: [{ destination: "rtmp", status: "live", startedAtMs: 1300, framesSent: 2, retryCount: 1, warning: undefined }]
    });
  });
});
