import { describe, expect, it } from "vitest";
import {
  buildIssues,
  buildSuggestions,
  computeNetworkStats,
  computeQualityScore,
  diagnoseNetwork,
  scoreJitter,
  scorePacketLoss,
  scoreRtt,
  scoreToQuality,
  trimSamples,
} from "./networkDiagnostics";

function makeSample(rttMs: number, packetLossPct = 0, jitterMs = 2, timestampMs = Date.now()) {
  return { rttMs, packetLossPct, jitterMs, timestampMs };
}

const GOOD_SAMPLES = [
  makeSample(30, 0, 2),
  makeSample(35, 0, 3),
  makeSample(28, 0, 2),
];

const POOR_SAMPLES = [
  makeSample(350, 4, 45),
  makeSample(400, 5, 50),
  makeSample(380, 3, 40),
];

describe("computeNetworkStats", () => {
  it("returns zeros for empty samples", () => {
    const stats = computeNetworkStats([]);
    expect(stats.avgRttMs).toBe(0);
    expect(stats.sampleCount).toBe(0);
  });

  it("computes average RTT correctly", () => {
    const stats = computeNetworkStats([makeSample(100), makeSample(200)]);
    expect(stats.avgRttMs).toBe(150);
  });

  it("computes min and max RTT", () => {
    const stats = computeNetworkStats([makeSample(50), makeSample(200), makeSample(100)]);
    expect(stats.minRttMs).toBe(50);
    expect(stats.maxRttMs).toBe(200);
  });

  it("computes average jitter", () => {
    const stats = computeNetworkStats([makeSample(100, 0, 10), makeSample(100, 0, 20)]);
    expect(stats.avgJitterMs).toBe(15);
  });

  it("computes average packet loss", () => {
    const stats = computeNetworkStats([makeSample(100, 2), makeSample(100, 4)]);
    expect(stats.avgPacketLossPct).toBe(3);
  });

  it("computes RTT standard deviation", () => {
    const stats = computeNetworkStats([makeSample(100), makeSample(100)]);
    expect(stats.rttStdDevMs).toBe(0); // identical samples → stddev 0
  });

  it("reports sample count", () => {
    const stats = computeNetworkStats(GOOD_SAMPLES);
    expect(stats.sampleCount).toBe(3);
  });
});

describe("scoreRtt / scoreJitter / scorePacketLoss", () => {
  it("scores excellent RTT as 100", () => {
    expect(scoreRtt(30)).toBe(100);
  });

  it("scores fair RTT lower than good", () => {
    expect(scoreRtt(150)).toBeLessThan(scoreRtt(80));
  });

  it("scores critical RTT as 0", () => {
    expect(scoreRtt(500)).toBe(0);
  });

  it("scores excellent jitter as 100", () => {
    expect(scoreJitter(3)).toBe(100);
  });

  it("scores critical jitter as 0", () => {
    expect(scoreJitter(100)).toBe(0);
  });

  it("scores zero packet loss as 100", () => {
    expect(scorePacketLoss(0)).toBe(100);
  });

  it("scores high packet loss as 0", () => {
    expect(scorePacketLoss(10)).toBe(0);
  });
});

describe("computeQualityScore", () => {
  it("returns high score for good network", () => {
    const stats = computeNetworkStats(GOOD_SAMPLES);
    expect(computeQualityScore(stats)).toBeGreaterThan(80);
  });

  it("returns low score for poor network", () => {
    const stats = computeNetworkStats(POOR_SAMPLES);
    expect(computeQualityScore(stats)).toBeLessThan(40);
  });

  it("score is between 0 and 100", () => {
    const stats = computeNetworkStats(GOOD_SAMPLES);
    const score = computeQualityScore(stats);
    expect(score).toBeGreaterThanOrEqual(0);
    expect(score).toBeLessThanOrEqual(100);
  });
});

describe("scoreToQuality", () => {
  it("maps 100 to excellent", () => {
    expect(scoreToQuality(100)).toBe("excellent");
  });

  it("maps 75 to good", () => {
    expect(scoreToQuality(75)).toBe("good");
  });

  it("maps 55 to fair", () => {
    expect(scoreToQuality(55)).toBe("fair");
  });

  it("maps 30 to poor", () => {
    expect(scoreToQuality(30)).toBe("poor");
  });

  it("maps 10 to critical", () => {
    expect(scoreToQuality(10)).toBe("critical");
  });
});

describe("buildIssues", () => {
  it("returns 'No network samples' for empty stats", () => {
    const stats = computeNetworkStats([]);
    expect(buildIssues(stats)).toContain("No network samples collected");
  });

  it("flags high RTT", () => {
    const stats = computeNetworkStats([makeSample(250)]);
    expect(buildIssues(stats).some((i) => i.includes("RTT"))).toBe(true);
  });

  it("flags packet loss", () => {
    const stats = computeNetworkStats([makeSample(50, 3)]);
    expect(buildIssues(stats).some((i) => i.includes("packet loss"))).toBe(true);
  });

  it("flags RTT spike when max is 2.5× average", () => {
    const stats = computeNetworkStats([makeSample(50), makeSample(50), makeSample(200)]);
    // avg ≈ 100, max = 200 (2× avg, not 2.5×)
    // Use a clear spike: avg≈30, max=200
    const spikeStats = computeNetworkStats([makeSample(20), makeSample(20), makeSample(500)]);
    expect(buildIssues(spikeStats).some((i) => i.includes("spike"))).toBe(true);
  });

  it("returns no issues for excellent network", () => {
    const stats = computeNetworkStats(GOOD_SAMPLES);
    expect(buildIssues(stats)).toHaveLength(0);
  });
});

describe("buildSuggestions", () => {
  it("suggests SRT for high RTT", () => {
    const stats = computeNetworkStats([makeSample(250)]);
    expect(buildSuggestions(stats).some((s) => s.includes("SRT"))).toBe(true);
  });

  it("suggests FEC for high packet loss", () => {
    const stats = computeNetworkStats([makeSample(50, 6)]);
    expect(buildSuggestions(stats).some((s) => s.includes("FEC"))).toBe(true);
  });

  it("suggests buffer increase for high jitter", () => {
    const stats = computeNetworkStats([makeSample(50, 0, 40)]);
    expect(buildSuggestions(stats).some((s) => s.includes("buffer"))).toBe(true);
  });

  it("returns no suggestions for excellent network", () => {
    const stats = computeNetworkStats(GOOD_SAMPLES);
    expect(buildSuggestions(stats)).toHaveLength(0);
  });
});

describe("diagnoseNetwork", () => {
  it("classifies good samples as excellent or good", () => {
    const report = diagnoseNetwork(GOOD_SAMPLES);
    expect(["excellent", "good"]).toContain(report.quality);
  });

  it("classifies poor samples as poor or critical", () => {
    const report = diagnoseNetwork(POOR_SAMPLES);
    expect(["poor", "critical"]).toContain(report.quality);
  });

  it("returns critical quality for empty samples", () => {
    const report = diagnoseNetwork([]);
    expect(report.quality).toBe("critical");
    expect(report.summary).toBe("No data");
  });

  it("summary includes quality label and RTT", () => {
    const report = diagnoseNetwork(GOOD_SAMPLES);
    expect(report.summary).toContain("RTT");
    expect(["excellent", "good", "fair", "poor", "critical"].some((q) => report.summary.includes(q))).toBe(true);
  });
});

describe("trimSamples", () => {
  it("keeps all samples when under limit", () => {
    expect(trimSamples(GOOD_SAMPLES, 10)).toHaveLength(3);
  });

  it("trims to the most recent N samples", () => {
    const samples = [makeSample(1, 0, 0, 1000), makeSample(2, 0, 0, 2000), makeSample(3, 0, 0, 3000)];
    const trimmed = trimSamples(samples, 2);
    expect(trimmed).toHaveLength(2);
    expect(trimmed[0].rttMs).toBe(2);
    expect(trimmed[1].rttMs).toBe(3);
  });

  it("returns empty array for empty input", () => {
    expect(trimSamples([], 10)).toHaveLength(0);
  });
});
