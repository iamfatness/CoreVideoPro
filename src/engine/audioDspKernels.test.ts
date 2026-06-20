import { describe, expect, it } from "vitest";
import {
  AUDIO_DBFS_FLOOR,
  AUDIO_PI,
  applyPeakLimiter,
  computeMomentaryLufs,
  computeRmsDbfs,
  computeSamplePeakDbfs,
  computeShortTermLufs,
  dbfsToLinear,
  linearToDbfs,
  peakLimiterGainReductionDb
} from "./audioDspKernels";

const SAMPLE_RATE = 48000;

function sine(amplitude: number, frequencyHz: number, sampleCount: number, sampleRate = SAMPLE_RATE): Float32Array {
  const samples = new Float32Array(sampleCount);
  for (let index = 0; index < sampleCount; index += 1) {
    samples[index] = amplitude * Math.sin((2 * AUDIO_PI * frequencyHz * index) / sampleRate);
  }
  return samples;
}

describe("audioDspKernels", () => {
  describe("computeRmsDbfs", () => {
    it("reports the silence floor for an empty or silent buffer", () => {
      expect(computeRmsDbfs(new Float32Array(0))).toBe(AUDIO_DBFS_FLOOR);
      expect(computeRmsDbfs(new Float32Array(512))).toBe(AUDIO_DBFS_FLOOR);
    });

    it("reports ~0 dBFS for a full-scale square wave", () => {
      const square = Float32Array.from({ length: 1024 }, (_unused, index) => (index % 2 === 0 ? 1 : -1));
      expect(computeRmsDbfs(square)).toBeCloseTo(0, 6);
    });

    it("reports ~-3.01 dBFS for a full-amplitude sine", () => {
      expect(computeRmsDbfs(sine(1, 1000, SAMPLE_RATE))).toBeCloseTo(-3.0103, 2);
    });

    it("accepts a plain number[] buffer", () => {
      expect(computeRmsDbfs([1, -1, 1, -1])).toBeCloseTo(0, 9);
    });
  });

  describe("computeSamplePeakDbfs", () => {
    it("reports the silence floor for an empty buffer", () => {
      expect(computeSamplePeakDbfs(new Float32Array(0))).toBe(AUDIO_DBFS_FLOOR);
    });

    it("reports ~0 dBFS at full scale", () => {
      expect(computeSamplePeakDbfs(sine(1, 1000, SAMPLE_RATE))).toBeCloseTo(0, 6);
    });

    it("reports ~-6 dBFS for a half-amplitude signal", () => {
      expect(computeSamplePeakDbfs(sine(0.5, 1000, SAMPLE_RATE))).toBeCloseTo(-6.0206, 2);
    });

    it("detects the loudest sample even when it is brief", () => {
      const buffer = new Float32Array(2048);
      buffer[1234] = 0.25;
      expect(computeSamplePeakDbfs(buffer)).toBeCloseTo(linearToDbfs(0.25), 9);
    });
  });

  describe("linearToDbfs / dbfsToLinear", () => {
    it("round-trips a linear ratio through dBFS", () => {
      expect(dbfsToLinear(linearToDbfs(0.5))).toBeCloseTo(0.5, 9);
      expect(dbfsToLinear(0)).toBeCloseTo(1, 9);
    });

    it("clamps non-positive and non-finite input to the floor", () => {
      expect(linearToDbfs(0)).toBe(AUDIO_DBFS_FLOOR);
      expect(linearToDbfs(-1)).toBe(AUDIO_DBFS_FLOOR);
      expect(linearToDbfs(Number.NaN)).toBe(AUDIO_DBFS_FLOOR);
    });
  });

  describe("BS.1770 loudness", () => {
    it("reports the floor for silence", () => {
      const silence = new Float32Array(SAMPLE_RATE);
      expect(computeMomentaryLufs(silence, silence, SAMPLE_RATE)).toBe(AUDIO_DBFS_FLOOR);
    });

    it("measures the short-term loudness of a known reference tone within tolerance", () => {
      // A -20 dBFS RMS 997 Hz tone (full-scale-referenced) on both channels.
      const amplitude = dbfsToLinear(-20) * Math.SQRT2;
      const tone = sine(amplitude, 997, SAMPLE_RATE * 3);
      // K-weighting + L=R=1.0 stereo summing places this near -17 LUFS.
      expect(computeShortTermLufs(tone, tone, SAMPLE_RATE)).toBeCloseTo(-16.99, 1);
    });

    it("rises by ~6 LU when amplitude doubles", () => {
      const quiet = sine(dbfsToLinear(-20) * Math.SQRT2, 997, SAMPLE_RATE * 3);
      const loud = sine(dbfsToLinear(-14) * Math.SQRT2, 997, SAMPLE_RATE * 3);
      const quietLufs = computeShortTermLufs(quiet, quiet, SAMPLE_RATE);
      const loudLufs = computeShortTermLufs(loud, loud, SAMPLE_RATE);
      expect(loudLufs - quietLufs).toBeCloseTo(6, 1);
    });

    it("uses the trailing 400 ms for momentary loudness", () => {
      // First half silent, second half a tone: momentary (400 ms tail) sees the tone.
      const total = SAMPLE_RATE; // 1 s
      const samples = sine(dbfsToLinear(-20) * Math.SQRT2, 997, total);
      for (let index = 0; index < total / 2; index += 1) {
        samples[index] = 0;
      }
      expect(computeMomentaryLufs(samples, samples, SAMPLE_RATE)).toBeGreaterThan(-30);
    });
  });

  describe("peak limiter", () => {
    it("applies no reduction to a signal already under threshold", () => {
      const samples = sine(0.5, 1000, 1024);
      const reduction = applyPeakLimiter(samples, -3);
      expect(reduction).toBe(0);
      expect(computeSamplePeakDbfs(samples)).toBeCloseTo(-6.0206, 2);
    });

    it("caps a hot signal with a positive gain reduction and a brickwall output", () => {
      const thresholdDbfs = -1;
      const hot = sine(1.5, 1000, 4096); // intentionally over full scale
      const predicted = peakLimiterGainReductionDb(hot, thresholdDbfs);
      const applied = applyPeakLimiter(hot, thresholdDbfs);

      expect(applied).toBeGreaterThan(0);
      expect(applied).toBeCloseTo(predicted, 9);
      // Output sample peak never exceeds the threshold (brickwall holds exactly).
      const thresholdLinear = dbfsToLinear(thresholdDbfs);
      for (let index = 0; index < hot.length; index += 1) {
        expect(Math.abs(hot[index])).toBeLessThanOrEqual(thresholdLinear + 1e-6);
      }
      expect(computeSamplePeakDbfs(hot)).toBeCloseTo(thresholdDbfs, 4);
    });

    it("returns zero reduction for an empty buffer", () => {
      expect(applyPeakLimiter(new Float32Array(0), -1)).toBe(0);
      expect(peakLimiterGainReductionDb(new Float32Array(0), -1)).toBe(0);
    });
  });
});
