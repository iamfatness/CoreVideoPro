export type HealthGrade = "excellent" | "good" | "degraded" | "critical";

export type StreamMetricSample = {
  bitrateKbps: number;
  droppedFrames: number;
  rttMs: number;
  encoderLoadPct: number;
  timestampMs: number;
};

export type StreamHealthReport = {
  score: number;
  grade: HealthGrade;
  bitrateKbps: number;
  droppedFrames: number;
  rttMs: number;
  encoderLoadPct: number;
  warnings: string[];
};

export type DestinationHealthReport = StreamHealthReport & {
  destinationId: string;
};

export type AggregateHealth = {
  worstGrade: HealthGrade;
  avgScore: number;
  allHealthy: boolean;
};

const GRADE_THRESHOLDS: Record<HealthGrade, number> = {
  excellent: 85,
  good: 65,
  degraded: 40,
  critical: 0,
};

export function gradeFromScore(score: number): HealthGrade {
  if (score >= GRADE_THRESHOLDS.excellent) return "excellent";
  if (score >= GRADE_THRESHOLDS.good) return "good";
  if (score >= GRADE_THRESHOLDS.degraded) return "degraded";
  return "critical";
}

/**
 * Score a single stream metric sample against a target bitrate (0–100).
 * Deductions: bitrate shortfall, dropped frames, high RTT, encoder overload.
 */
export function scoreStreamHealth(
  sample: StreamMetricSample,
  targetBitrateKbps: number
): StreamHealthReport {
  const warnings: string[] = [];
  let score = 100;

  // Bitrate health: deduct up to 40 pts if bitrate is below target
  if (targetBitrateKbps > 0) {
    const bitrateRatio = sample.bitrateKbps / targetBitrateKbps;
    if (bitrateRatio < 1) {
      const deduction = Math.round((1 - bitrateRatio) * 40);
      score -= deduction;
      if (bitrateRatio < 0.5) warnings.push("Bitrate critically low");
      else if (bitrateRatio < 0.8) warnings.push("Bitrate below target");
    }
  }

  // Dropped frames: deduct up to 30 pts (proportional, caps at 30 frames)
  if (sample.droppedFrames > 0) {
    const deduction = Math.min(30, sample.droppedFrames);
    score -= deduction;
    if (sample.droppedFrames >= 10) warnings.push("Significant frame drops");
    else warnings.push("Frame drops detected");
  }

  // RTT: deduct up to 20 pts (>500ms is critical)
  if (sample.rttMs > 100) {
    const rttDeduction = Math.min(20, Math.round(((sample.rttMs - 100) / 400) * 20));
    score -= rttDeduction;
    if (sample.rttMs > 500) warnings.push("High network latency");
    else if (sample.rttMs > 250) warnings.push("Elevated RTT");
  }

  // Encoder load: deduct up to 10 pts (>90% is overloaded)
  if (sample.encoderLoadPct > 80) {
    const loadDeduction = Math.min(10, Math.round(((sample.encoderLoadPct - 80) / 20) * 10));
    score -= loadDeduction;
    if (sample.encoderLoadPct >= 95) warnings.push("Encoder overloaded");
    else warnings.push("Encoder under pressure");
  }

  const finalScore = Math.max(0, Math.min(100, score));

  return {
    score: finalScore,
    grade: gradeFromScore(finalScore),
    bitrateKbps: sample.bitrateKbps,
    droppedFrames: sample.droppedFrames,
    rttMs: sample.rttMs,
    encoderLoadPct: sample.encoderLoadPct,
    warnings,
  };
}

export function summarizeStreamHealth(report: StreamHealthReport): string {
  if (report.warnings.length === 0) {
    return `${report.grade.charAt(0).toUpperCase() + report.grade.slice(1)} — ${report.score}/100`;
  }
  return `${report.grade.charAt(0).toUpperCase() + report.grade.slice(1)} — ${report.warnings[0]}`;
}

const GRADE_ORDER: HealthGrade[] = ["excellent", "good", "degraded", "critical"];

export function aggregateStreamHealth(reports: StreamHealthReport[]): AggregateHealth {
  if (reports.length === 0) {
    return { worstGrade: "excellent", avgScore: 100, allHealthy: true };
  }
  const avgScore = Math.round(reports.reduce((sum, r) => sum + r.score, 0) / reports.length);
  const worstGrade = reports.reduce<HealthGrade>((worst, r) => {
    return GRADE_ORDER.indexOf(r.grade) > GRADE_ORDER.indexOf(worst) ? r.grade : worst;
  }, "excellent");
  return {
    worstGrade,
    avgScore,
    allHealthy: worstGrade === "excellent" || worstGrade === "good",
  };
}

export function formatBitrate(kbps: number): string {
  if (kbps >= 1000) return `${(kbps / 1000).toFixed(1)} Mbps`;
  return `${Math.round(kbps)} kbps`;
}
