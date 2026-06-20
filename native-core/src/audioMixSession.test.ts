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

  it("measures master level and BS.1770 loudness from the synthesized program mix", () => {
    const model = new AudioMixSessionModel();
    const snapshot = model.sync([
      { participantId: "p1", inputLevel: 70, muted: false, noiseSuppression: false },
      { participantId: "p2", inputLevel: 50, muted: false, noiseSuppression: false }
    ]);

    // Loudness is a real measurement, not the old -16/-14 lookup constants.
    expect(snapshot.loudnessLufs).not.toBe(-16);
    expect(snapshot.loudnessLufs).not.toBe(-14);
    expect(Number.isFinite(snapshot.loudnessLufs)).toBe(true);
    expect(snapshot.masterLevel).toBeGreaterThan(40);
    expect(snapshot.masterLevel).toBeLessThanOrEqual(100);
  });

  it("reports the silence floor for loudness when every channel is muted", () => {
    const model = new AudioMixSessionModel();
    const snapshot = model.sync([{ participantId: "p1", inputLevel: 80, muted: true, noiseSuppression: false }]);

    expect(snapshot.masterLevel).toBe(0);
    expect(snapshot.loudnessLufs).toBe(-60);
    expect(snapshot.limiterActive).toBe(false);
  });

  it("measures per-participant rmsDbfs and peakDbfs from the synthesized post-gain PCM", () => {
    const model = new AudioMixSessionModel();
    const snapshot = model.sync([
      { participantId: "audible", inputLevel: 68, muted: false, noiseSuppression: false },
      { participantId: "silent", inputLevel: 80, muted: true, noiseSuppression: false }
    ]);

    const audible = snapshot.participants[0];
    const muted = snapshot.participants[1];

    // A sine peaks ~3 dB above its RMS, so peak should sit above RMS and both
    // are negative dBFS (below full scale) for a sub-full-scale signal.
    expect(audible?.rmsDbfs).toBeLessThan(0);
    expect(audible?.peakDbfs).toBeLessThanOrEqual(0);
    expect(audible?.peakDbfs).toBeGreaterThan(audible!.rmsDbfs);
    expect(Number.isFinite(audible?.rmsDbfs)).toBe(true);
    expect(Number.isFinite(audible?.peakDbfs)).toBe(true);

    // A muted channel is digital silence -> the kernel floor for both metrics.
    expect(muted?.rmsDbfs).toBe(-120);
    expect(muted?.peakDbfs).toBe(-120);
  });

  it("engages the measured master limiter on a hot program mix", () => {
    const model = new AudioMixSessionModel();
    const snapshot = model.sync([
      { participantId: "hot-a", inputLevel: 100, muted: false, noiseSuppression: false, manualGainDb: 12 },
      { participantId: "hot-b", inputLevel: 100, muted: false, noiseSuppression: false, manualGainDb: 12 }
    ]);

    expect(snapshot.limiterActive).toBe(true);
    expect(snapshot.masterLevel).toBe(100);
  });

  it("produces a deterministic measured snapshot for the same channels", () => {
    const channels = [
      { participantId: "p1", inputLevel: 60, muted: false, noiseSuppression: false },
      { participantId: "p2", inputLevel: 44, muted: false, noiseSuppression: true }
    ];
    const first = new AudioMixSessionModel().sync(channels.map((channel) => ({ ...channel })));
    const second = new AudioMixSessionModel().sync(channels.map((channel) => ({ ...channel })));
    expect(second).toEqual(first);
  });
});
