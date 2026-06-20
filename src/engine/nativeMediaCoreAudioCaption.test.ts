import { describe, expect, it } from "vitest";
import {
  IDLE_NATIVE_AUDIO_MIX_SESSION,
  NativeAudioMixSessionSimulator,
  NativeCaptionTrackSimulator
} from "./nativeMediaCoreAudioCaption";

describe("NativeAudioMixSessionSimulator", () => {
  it("returns an idle snapshot when no channels are synced", () => {
    const simulator = new NativeAudioMixSessionSimulator();
    expect(simulator.snapshot()).toEqual(IDLE_NATIVE_AUDIO_MIX_SESSION);
  });

  it("measures master level and loudness from the synthesized program mix", () => {
    const simulator = new NativeAudioMixSessionSimulator();
    const snapshot = simulator.sync([
      { participantId: "p1", inputLevel: 70, muted: false, noiseSuppression: false },
      { participantId: "p2", inputLevel: 50, muted: false, noiseSuppression: false }
    ]);

    expect(snapshot.status).toBe("live");
    expect(snapshot.participants).toHaveLength(2);
    // Master level is measured (not a fixed -16/-14 lookup) and lands in a sane
    // audible band for two mid-level sources.
    expect(snapshot.masterLevel).toBeGreaterThan(40);
    expect(snapshot.masterLevel).toBeLessThanOrEqual(100);
    // LUFS is a real BS.1770 measurement, not the old constant -16/-14 values.
    expect(snapshot.loudnessLufs).not.toBe(-16);
    expect(snapshot.loudnessLufs).not.toBe(-14);
    expect(Number.isFinite(snapshot.loudnessLufs)).toBe(true);
    expect(snapshot.limiterActive).toBe(false);
  });

  it("rises in measured master level as input levels increase within a gain band", () => {
    // Levels 55 and 70 both sit in the unity-gain band (no smart boost/duck), so
    // the measured master level and loudness rise monotonically with the input.
    const quiet = new NativeAudioMixSessionSimulator().sync([
      { participantId: "p1", inputLevel: 55, muted: false, noiseSuppression: false }
    ]);
    const loud = new NativeAudioMixSessionSimulator().sync([
      { participantId: "p1", inputLevel: 70, muted: false, noiseSuppression: false }
    ]);

    expect(quiet.participants[0]?.gainDb).toBe(0);
    expect(loud.participants[0]?.gainDb).toBe(0);
    expect(loud.masterLevel).toBeGreaterThan(quiet.masterLevel);
    expect(loud.loudnessLufs).toBeGreaterThan(quiet.loudnessLufs);
  });

  it("engages the measured limiter on a hot program mix and caps participant levels", () => {
    const snapshot = new NativeAudioMixSessionSimulator().sync([
      { participantId: "hot-a", inputLevel: 100, muted: false, noiseSuppression: false, manualGainDb: 12 },
      { participantId: "hot-b", inputLevel: 100, muted: false, noiseSuppression: false, manualGainDb: 12 }
    ]);

    expect(snapshot.limiterActive).toBe(true);
    expect(snapshot.masterLevel).toBe(100);
    expect(snapshot.participants.every((channel) => channel.limiterActive)).toBe(true);
  });

  it("reports silence-floor metrics when every channel is muted", () => {
    const snapshot = new NativeAudioMixSessionSimulator().sync([
      { participantId: "p1", inputLevel: 80, muted: true, noiseSuppression: false }
    ]);

    expect(snapshot.masterLevel).toBe(0);
    expect(snapshot.loudnessLufs).toBe(-60);
    expect(snapshot.limiterActive).toBe(false);
    expect(snapshot.participants[0]).toMatchObject({ outputLevel: 0, limiterActive: false, muted: true });
  });

  it("is deterministic across repeated snapshots for the same input", () => {
    const simulator = new NativeAudioMixSessionSimulator();
    const first = simulator.sync([
      { participantId: "p1", inputLevel: 60, muted: false, noiseSuppression: false },
      { participantId: "p2", inputLevel: 44, muted: false, noiseSuppression: true }
    ]);
    const second = simulator.snapshot();
    expect(second).toEqual(first);

    // A fresh simulator with the same channels yields an identical measurement.
    const replay = new NativeAudioMixSessionSimulator().sync([
      { participantId: "p1", inputLevel: 60, muted: false, noiseSuppression: false },
      { participantId: "p2", inputLevel: 44, muted: false, noiseSuppression: true }
    ]);
    expect(replay).toEqual(first);
  });

  it("measures a participant outputLevel that tracks its synthesized post-gain RMS", () => {
    const snapshot = new NativeAudioMixSessionSimulator().sync([
      { participantId: "balanced", inputLevel: 68, muted: false, noiseSuppression: false }
    ]);
    const channel = snapshot.participants[0];
    // No smart gain at the target level -> measured output level tracks input.
    expect(channel?.gainDb).toBe(0);
    expect(channel?.outputLevel).toBeGreaterThanOrEqual(66);
    expect(channel?.outputLevel).toBeLessThanOrEqual(70);
  });

  it("measures per-participant rmsDbfs and peakDbfs from the synthesized post-gain PCM", () => {
    const snapshot = new NativeAudioMixSessionSimulator().sync([
      { participantId: "audible", inputLevel: 68, muted: false, noiseSuppression: false },
      { participantId: "silent", inputLevel: 80, muted: true, noiseSuppression: false }
    ]);
    const audible = snapshot.participants[0];
    const muted = snapshot.participants[1];

    // A sine peaks ~3 dB above its RMS; both are negative dBFS below full scale.
    expect(audible?.rmsDbfs).toBeLessThan(0);
    expect(audible?.peakDbfs).toBeLessThanOrEqual(0);
    expect(audible?.peakDbfs).toBeGreaterThan(audible!.rmsDbfs);
    expect(Number.isFinite(audible?.rmsDbfs)).toBe(true);
    expect(Number.isFinite(audible?.peakDbfs)).toBe(true);

    // A muted channel is digital silence -> the kernel floor for both metrics.
    expect(muted?.rmsDbfs).toBe(-120);
    expect(muted?.peakDbfs).toBe(-120);
  });
});

describe("NativeCaptionTrackSimulator", () => {
  it("publishes the most recent cue and stays deterministic", () => {
    const simulator = new NativeCaptionTrackSimulator();
    const snapshot = simulator.pushCue("Welcome to the broadcast", 1200, "Host");
    expect(snapshot.status).toBe("live");
    expect(snapshot.currentCue?.text).toBe("Welcome to the broadcast");
    expect(simulator.snapshot()).toEqual(snapshot);
  });
});
