/**
 * Network diagnostics engine.
 * Analyses RTT, jitter, and packet loss samples to classify connection quality
 * and surface operator-facing remediation suggestions.
 */

export type NetworkQuality = "excellent" | "good" | "fair" | "poor" | "critical";

export type NetworkSample = {
  /** Round-trip time in milliseconds */
  rttMs: number;
  /** Packet loss percentage 0–100 */
  packetLossPct: number;
  /** Jitter in milliseconds */
  jitterMs: number;
  /** Unix timestamp of this measurement */
  timestampMs: number;
};

export type NetworkStats = {
  avgRttMs: number;
  maxRttMs: number;
  minRttMs: number;
  avgJitterMs: number;
  avgPacketLossPct: number;
  /** Standard deviation of RTT samples */
  rttStdDevMs: number;
  sampleCount: number;
};

export type NetworkDiagnosticReport = {
  stats: NetworkStats;
  quality: NetworkQuality;
  qualityScore: number; // 0–100
  issues: string[];
  suggestions: string[];
  summary: string;
};

/** Thresholds for quality classification */
const THRESHOLDS = {
  rtt: { excellent: 50, good: 100, fair: 200, poor: 400 },
  jitter: { excellent: 5, good: 15, fair: 30, poor: 60 },
  loss: { excellent: 0.1, good: 0.5, fair: 2, poor: 5 },
};

/**
 * Compute aggregate statistics from a set of network samples.
 */
export function computeNetworkStats(samples: NetworkSample[]): NetworkStats {
  if (samples.length === 0) {
    return { avgRttMs: 0, maxRttMs: 0, minRttMs: 0, avgJitterMs: 0, avgPacketLossPct: 0, rttStdDevMs: 0, sampleCount: 0 };
  }

  const rtts = samples.map((s) => s.rttMs);
  const avgRttMs = rtts.reduce((a, b) => a + b, 0) / rtts.length;
  const maxRttMs = Math.max(...rtts);
  const minRttMs = Math.min(...rtts);
  const avgJitterMs = samples.reduce((a, s) => a + s.jitterMs, 0) / samples.length;
  const avgPacketLossPct = samples.reduce((a, s) => a + s.packetLossPct, 0) / samples.length;

  const variance = rtts.reduce((sum, r) => sum + (r - avgRttMs) ** 2, 0) / rtts.length;
  const rttStdDevMs = Math.sqrt(variance);

  return {
    avgRttMs: Math.round(avgRttMs),
    maxRttMs: Math.round(maxRttMs),
    minRttMs: Math.round(minRttMs),
    avgJitterMs: Math.round(avgJitterMs * 10) / 10,
    avgPacketLossPct: Math.round(avgPacketLossPct * 100) / 100,
    rttStdDevMs: Math.round(rttStdDevMs),
    sampleCount: samples.length,
  };
}

/**
 * Score a single metric dimension 0–100 (higher = better).
 */
export function scoreRtt(avgRttMs: number): number {
  if (avgRttMs <= THRESHOLDS.rtt.excellent) return 100;
  if (avgRttMs <= THRESHOLDS.rtt.good) return 80;
  if (avgRttMs <= THRESHOLDS.rtt.fair) return 55;
  if (avgRttMs <= THRESHOLDS.rtt.poor) return 25;
  return 0;
}

export function scoreJitter(avgJitterMs: number): number {
  if (avgJitterMs <= THRESHOLDS.jitter.excellent) return 100;
  if (avgJitterMs <= THRESHOLDS.jitter.good) return 80;
  if (avgJitterMs <= THRESHOLDS.jitter.fair) return 55;
  if (avgJitterMs <= THRESHOLDS.jitter.poor) return 25;
  return 0;
}

export function scorePacketLoss(avgLossPct: number): number {
  if (avgLossPct <= THRESHOLDS.loss.excellent) return 100;
  if (avgLossPct <= THRESHOLDS.loss.good) return 80;
  if (avgLossPct <= THRESHOLDS.loss.fair) return 55;
  if (avgLossPct <= THRESHOLDS.loss.poor) return 25;
  return 0;
}

/**
 * Combine RTT, jitter, and loss scores into an overall quality score (weighted).
 */
export function computeQualityScore(stats: NetworkStats): number {
  const rttScore = scoreRtt(stats.avgRttMs);
  const jitterScore = scoreJitter(stats.avgJitterMs);
  const lossScore = scorePacketLoss(stats.avgPacketLossPct);
  // Packet loss is most disruptive for live streaming
  return Math.round(rttScore * 0.3 + jitterScore * 0.3 + lossScore * 0.4);
}

/**
 * Map a quality score to a NetworkQuality label.
 */
export function scoreToQuality(score: number): NetworkQuality {
  if (score >= 90) return "excellent";
  if (score >= 70) return "good";
  if (score >= 50) return "fair";
  if (score >= 25) return "poor";
  return "critical";
}

/**
 * Build remediation suggestions based on observed metrics.
 */
export function buildSuggestions(stats: NetworkStats): string[] {
  const suggestions: string[] = [];

  if (stats.avgRttMs > THRESHOLDS.rtt.fair) {
    suggestions.push("High RTT — switch to a closer streaming ingest endpoint or use SRT with adaptive latency");
  }

  if (stats.avgJitterMs > THRESHOLDS.jitter.fair) {
    suggestions.push("High jitter — increase encoder buffer frames or switch to a wired ethernet connection");
  }

  if (stats.avgPacketLossPct > THRESHOLDS.loss.fair) {
    suggestions.push("Packet loss detected — enable SRT ARQ retransmission or reduce output bitrate");
  }

  if (stats.rttStdDevMs > 50) {
    suggestions.push("Unstable RTT — consider disabling background network activity or use QoS traffic shaping");
  }

  if (stats.avgPacketLossPct > THRESHOLDS.loss.poor) {
    suggestions.push("Critical loss — drop to 720p or lower and enable 2× FEC redundancy immediately");
  }

  return suggestions;
}

/**
 * Identify specific issues in human-readable form.
 */
export function buildIssues(stats: NetworkStats): string[] {
  const issues: string[] = [];

  if (stats.sampleCount === 0) {
    return ["No network samples collected"];
  }

  if (stats.avgRttMs > THRESHOLDS.rtt.poor) {
    issues.push(`RTT ${stats.avgRttMs}ms is critically high (threshold ${THRESHOLDS.rtt.poor}ms)`);
  } else if (stats.avgRttMs > THRESHOLDS.rtt.fair) {
    issues.push(`RTT ${stats.avgRttMs}ms exceeds fair threshold (${THRESHOLDS.rtt.fair}ms)`);
  }

  if (stats.avgJitterMs > THRESHOLDS.jitter.fair) {
    issues.push(`Jitter ${stats.avgJitterMs}ms may cause audio/video artefacts`);
  }

  if (stats.avgPacketLossPct > THRESHOLDS.loss.good) {
    issues.push(`${stats.avgPacketLossPct.toFixed(2)}% packet loss will cause visible glitches`);
  }

  if (stats.maxRttMs > stats.avgRttMs * 2.5) {
    issues.push(`RTT spike to ${stats.maxRttMs}ms detected (${Math.round(stats.maxRttMs / stats.avgRttMs)}× average)`);
  }

  return issues;
}

/**
 * Full diagnostic report from a set of samples.
 */
export function diagnoseNetwork(samples: NetworkSample[]): NetworkDiagnosticReport {
  const stats = computeNetworkStats(samples);
  const qualityScore = samples.length === 0 ? 0 : computeQualityScore(stats);
  const quality = samples.length === 0 ? "critical" : scoreToQuality(qualityScore);
  const issues = buildIssues(stats);
  const suggestions = buildSuggestions(stats);

  const summary =
    samples.length === 0
      ? "No data"
      : `${quality} — RTT ${stats.avgRttMs}ms, jitter ${stats.avgJitterMs}ms, loss ${stats.avgPacketLossPct}%`;

  return { stats, quality, qualityScore, issues, suggestions, summary };
}

/**
 * Keep only the most recent N samples (sliding window).
 */
export function trimSamples(samples: NetworkSample[], maxCount: number): NetworkSample[] {
  if (samples.length <= maxCount) return samples;
  return samples.slice(samples.length - maxCount);
}
