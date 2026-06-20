import { describe, expect, it } from "vitest";
import { AudioMixSessionModel } from "./audioMixSession.js";

describe("AudioMixSessionModel", () => {
  it("returns an idle snapshot when no channels are synced", () => {
    const model = new AudioMixSessionModel();

    expect(model.snapshot()).toMatchObject({
      status: "idle",
      masterLevel: 0,
      loudnessLufs: -60,
      summary: "Audio mix idle.",
      participants: []
    });
  });

  it("balances participant levels and reports boosting or ducking", () => {
    const model = new AudioMixSessionModel();
    const snapshot = model.sync([
      { participantId: "p1", inputLevel: 18, muted: false, noiseSuppression: false },
      { participantId: "p2", inputLevel: 82, muted: false, noiseSuppression: false, manualGainDb: -2 }
    ]);

    expect(snapshot.status).toBe("warning");
    expect(snapshot.participants).toHaveLength(2);
    expect(snapshot.participants[0]?.status).toBe("boosting");
    expect(snapshot.participants[1]?.status).toBe("ducking");
    expect(snapshot.summary).toContain("boosted");
    expect(snapshot.summary).toContain("ducked");
  });

  it("warns when a raw audio sync arrives without participant channels", () => {
    const model = new AudioMixSessionModel();

    const snapshot = model.sync([]);
    const afterEmptyMix = model.mix(0);

    expect(snapshot).toMatchObject({
      status: "warning",
      masterLevel: 0,
      limiterActive: false,
      mixedFrameCount: 0,
      participants: [],
      warnings: ["Raw participant audio is missing from the mix session."]
    });
    expect(afterEmptyMix).toEqual(snapshot);
  });

  it("deduplicates participant channels and bounds levels for deterministic DSP readiness", () => {
    const model = new AudioMixSessionModel();
    const snapshot = model.sync([
      { participantId: "p1", inputLevel: 140, muted: false, noiseSuppression: false, manualGainDb: 18 },
      { participantId: "p1", inputLevel: 12, muted: false, noiseSuppression: true },
      { participantId: " ", inputLevel: Number.NaN, muted: true, noiseSuppression: false }
    ]);

    expect(snapshot.status).toBe("warning");
    expect(snapshot.participants).toEqual([
      expect.objectContaining({
        participantId: "p1",
        inputLevel: 100,
        gainDb: 8,
        limiterActive: true,
        status: "boosting"
      }),
      expect.objectContaining({
        participantId: "unknown-audio-source-3",
        inputLevel: 0,
        outputLevel: 0,
        muted: true,
        status: "muted"
      })
    ]);
    expect(snapshot.warnings).toEqual(
      expect.arrayContaining([
        "Participant p1 has duplicated isolated audio channels; using the first channel for deterministic mix.",
        "Audio mix channel levels were bounded to DSP readiness limits.",
        "Limiter active in participant audio mix."
      ])
    );
  });

  it("keeps audio health state stable across empty A/V sync ticks", () => {
    const model = new AudioMixSessionModel();
    const synced = model.sync([{ participantId: "p1", inputLevel: 68, muted: false, noiseSuppression: false }]);
    const afterNoFrames = model.mix(0);
    const afterProgramFrameOnly = model.mix(1);

    expect(afterNoFrames).toEqual(synced);
    expect(afterProgramFrameOnly).toMatchObject({
      status: "live",
      limiterActive: false,
      mixedFrameCount: 1,
      participants: [expect.objectContaining({ participantId: "p1", status: "balanced" })],
      warnings: []
    });
  });

  it("keeps limiter activity off when the master limiter is bypassed", () => {
    const model = new AudioMixSessionModel();
    const snapshot = model.sync(
      [{ participantId: "hot-host", inputLevel: 100, muted: false, noiseSuppression: false, manualGainDb: 12 }],
      false
    );

    expect(snapshot.limiterEnabled).toBe(false);
    expect(snapshot.limiterActive).toBe(false);
    expect(snapshot.participants[0]).toMatchObject({
      outputLevel: 100,
      limiterActive: false
    });
    expect(snapshot.warnings).not.toContain("Limiter active in participant audio mix.");
  });
});
