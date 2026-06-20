import { describe, expect, it } from "vitest";
import {
  AUDIO_DBFS_FLOOR,
  AUDIO_PI,
  applyNoiseGate,
  applyPeakLimiter,
  computeMomentaryLufs,
  computeRmsDbfs,
  computeSamplePeakDbfs,
  computeShortTermLufs,
  computeTruePeakDbfs,
  dbfsToLinear,
  linearToDbfs,
  peakLimiterGainReductionDb
} from "./audioDspKernels.js";

const SAMPLE_RATE = 48000;

function sine(amplitude: number, frequencyHz: number, sampleCount: number, sampleRate = SAMPLE_RATE): Float32Array {
  const samples = new Float32Array(sampleCount);
  for (let index = 0; index < sampleCount; index += 1) {
    samples[index] = amplitude * Math.sin((2 * AUDIO_PI * frequencyHz * index) / sampleRate);
  }
  return samples;
}

function rmsLinear(samples: Float32Array, from = 0, to = samples.length): number {
  let sumSquares = 0;
  for (let index = from; index < to; index += 1) {
    sumSquares += samples[index] * samples[index];
  }
  const span = to - from;
  return span === 0 ? 0 : Math.sqrt(sumSquares / span);
}

// A fs/4 tone sampled half a sample off-phase: the discrete samples sit at
// +/-amp/sqrt(2), so the sample peak is ~3 dB below the real tone amplitude and a
// true-peak meter must recover the higher inter-sample peak.
function interSampleOvershoot(amplitude: number, sampleCount: number, sampleRate = SAMPLE_RATE): Float32Array {
  const samples = new Float32Array(sampleCount);
  for (let index = 0; index < sampleCount; index += 1) {
    samples[index] = amplitude * Math.sin((2 * AUDIO_PI * (sampleRate / 4) * (index + 0.5)) / sampleRate);
  }
  return samples;
}

describe("audioDspKernels (native-core)", () => {
  // The native-core copy must stay byte-for-byte equivalent in behavior to the
  // renderer copy and the C++ reference. These exercise the kernels the renderer
  // copy already covers plus the new gate / true-peak additions.
  describe("baseline kernels", () => {
    it("measures RMS and sample peak of a known sine", () => {
      expect(computeRmsDbfs(sine(1, 1000, SAMPLE_RATE))).toBeCloseTo(-3.0103, 2);
      expect(computeSamplePeakDbfs(sine(0.5, 1000, SAMPLE_RATE))).toBeCloseTo(-6.0206, 2);
      expect(computeRmsDbfs(new Float32Array(0))).toBe(AUDIO_DBFS_FLOOR);
    });

    it("round-trips dBFS conversions", () => {
      expect(dbfsToLinear(linearToDbfs(0.5))).toBeCloseTo(0.5, 9);
      expect(linearToDbfs(0)).toBe(AUDIO_DBFS_FLOOR);
    });

    it("reports the floor for silent loudness", () => {
      const silence = new Float32Array(SAMPLE_RATE);
      expect(computeMomentaryLufs(silence, silence, SAMPLE_RATE)).toBe(AUDIO_DBFS_FLOOR);
      expect(computeShortTermLufs(silence, silence, SAMPLE_RATE)).toBe(AUDIO_DBFS_FLOOR);
    });

    it("limits a hot signal to a brickwall threshold", () => {
      const thresholdDbfs = -1;
      const hot = sine(1.5, 1000, 4096);
      const predicted = peakLimiterGainReductionDb(hot, thresholdDbfs);
      const applied = applyPeakLimiter(hot, thresholdDbfs);
      expect(applied).toBeGreaterThan(0);
      expect(applied).toBeCloseTo(predicted, 9);
      expect(computeSamplePeakDbfs(hot)).toBeCloseTo(thresholdDbfs, 4);
    });
  });

  describe("noise gate", () => {
    it("closes fully on silence", () => {
      const silence = new Float32Array(4800);
      const gatedFraction = applyNoiseGate(silence, -30, 5, 50, SAMPLE_RATE);
      expect(gatedFraction).toBeCloseTo(1, 9);
      expect(rmsLinear(silence)).toBe(0);
    });

    it("heavily attenuates a signal well below threshold", () => {
      const quiet = sine(dbfsToLinear(-50), 1000, 9600);
      const inRms = rmsLinear(quiet);
      const gatedFraction = applyNoiseGate(quiet, -30, 5, 50, SAMPLE_RATE);
      const outRms = rmsLinear(quiet);
      expect(gatedFraction).toBeGreaterThan(0.99);
      expect(outRms).toBeLessThan(inRms * 0.01);
    });

    it("passes a loud signal nearly unchanged", () => {
      const loud = sine(0.5, 1000, 9600);
      const reference = loud.slice();
      const gatedFraction = applyNoiseGate(loud, -30, 5, 50, SAMPLE_RATE);
      expect(gatedFraction).toBeLessThan(0.1);
      const half = loud.length / 2;
      expect(rmsLinear(loud, half)).toBeCloseTo(rmsLinear(reference, half), 4);
    });

    it("is deterministic", () => {
      const a = sine(0.4, 800, 4800);
      const b = a.slice();
      const ra = applyNoiseGate(a, -30, 5, 50, SAMPLE_RATE);
      const rb = applyNoiseGate(b, -30, 5, 50, SAMPLE_RATE);
      expect(ra).toBe(rb);
      expect(Array.from(a)).toEqual(Array.from(b));
    });

    it("is safe for an empty buffer", () => {
      expect(applyNoiseGate(new Float32Array(0), -30, 5, 50, SAMPLE_RATE)).toBe(0);
    });
  });

  describe("true-peak", () => {
    it("reports the floor for silence and empty buffers", () => {
      expect(computeTruePeakDbfs(new Float32Array(0))).toBe(AUDIO_DBFS_FLOOR);
      expect(computeTruePeakDbfs(new Float32Array(1024))).toBe(AUDIO_DBFS_FLOOR);
    });

    it("is always at least the sample peak", () => {
      const tone = sine(0.7, 997, 4800);
      expect(computeTruePeakDbfs(tone)).toBeGreaterThanOrEqual(computeSamplePeakDbfs(tone) - 1e-6);
    });

    it("does not invent overshoot for a flat (DC) signal", () => {
      const dc = new Float32Array(2048).fill(1);
      const truePeak = computeTruePeakDbfs(dc);
      expect(truePeak).toBeGreaterThanOrEqual(computeSamplePeakDbfs(dc) - 1e-6);
      expect(Math.abs(truePeak)).toBeLessThan(0.05);
    });

    it("catches a known inter-sample overshoot", () => {
      const signal = interSampleOvershoot(0.9, 2048);
      const samplePeak = computeSamplePeakDbfs(signal);
      const truePeak = computeTruePeakDbfs(signal, 4);
      expect(truePeak).toBeGreaterThan(samplePeak);
      expect(truePeak - samplePeak).toBeGreaterThan(2);
      expect(truePeak).toBeGreaterThan(-1.5);
    });

    it("is deterministic", () => {
      const signal = interSampleOvershoot(0.8, 2048);
      expect(computeTruePeakDbfs(signal, 4)).toBe(computeTruePeakDbfs(signal, 4));
    });
  });
});
