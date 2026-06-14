/**
 * Audio loudness normalisation engine.
 * Models integrated loudness (LUFS), true peak (dBTP), and loudness range (LU)
 * against broadcast and streaming platform targets.
 */

export type LoudnessTarget = {
  id: string;
  name: string;
  /** Integrated loudness target in LUFS (negative) */
  targetLufs: number;
  /** Maximum true peak in dBTP */
  maxTruePeakDbtp: number;
  /** EBU R128 or platform-specific */
  standard: "EBU R128" | "ATSC A/85" | "Spotify" | "YouTube" | "Apple Podcasts" | "Zoom";
};

export type LoudnessReading = {
  /** Measured integrated loudness in LUFS */
  integratedLufs: number;
  /** Measured short-term loudness in LUFS */
  shortTermLufs: number;
  /** Measured true peak in dBTP */
  truePeakDbtp: number;
  /** Loudness range in LU */
  loudnessRangeLu: number;
};

export type LoudnessStatus = {
  /** LUFS deviation from target (positive = too loud, negative = too quiet) */
  deviationLu: number;
  /** Gain adjustment to apply to reach target in dB */
  gainAdjustmentDb: number;
  /** true when true peak headroom is exceeded */
  peakClipping: boolean;
  level: "ok" | "too-loud" | "too-quiet" | "clipping";
  warnings: string[];
  summary: string;
};

export type NormalisationPlan = {
  target: LoudnessTarget;
  status: LoudnessStatus;
  /** Suggested compressor ratio for dynamic range control (1:1 = no compression) */
  suggestedRatio: number;
  /** Suggested limiter ceiling in dBTP */
  limiterCeilingDbtp: number;
};

export const loudnessTargets: LoudnessTarget[] = [
  { id: "ebu-r128", name: "EBU R128 (Broadcast)", targetLufs: -23, maxTruePeakDbtp: -1, standard: "EBU R128" },
  { id: "atsc-a85", name: "ATSC A/85 (US Broadcast)", targetLufs: -24, maxTruePeakDbtp: -2, standard: "ATSC A/85" },
  { id: "youtube", name: "YouTube", targetLufs: -14, maxTruePeakDbtp: -1, standard: "YouTube" },
  { id: "spotify", name: "Spotify", targetLufs: -14, maxTruePeakDbtp: -2, standard: "Spotify" },
  { id: "apple-podcasts", name: "Apple Podcasts", targetLufs: -16, maxTruePeakDbtp: -1, standard: "Apple Podcasts" },
  { id: "zoom", name: "Zoom Live (recommended)", targetLufs: -14, maxTruePeakDbtp: -3, standard: "Zoom" }
];

export function getLoudnessTarget(id: string): LoudnessTarget | undefined {
  return loudnessTargets.find((t) => t.id === id);
}

/** Acceptable tolerance window around the target LUFS (±1 LU) */
const TOLERANCE_LU = 1;

export function evaluateLoudness(reading: LoudnessReading, target: LoudnessTarget): LoudnessStatus {
  const warnings: string[] = [];
  const deviation = reading.integratedLufs - target.targetLufs;
  const gainAdjustmentDb = -deviation;
  const peakClipping = reading.truePeakDbtp > target.maxTruePeakDbtp;

  if (peakClipping) {
    warnings.push(`True peak ${reading.truePeakDbtp.toFixed(1)} dBTP exceeds limit of ${target.maxTruePeakDbtp} dBTP — limiter needed.`);
  }

  let level: LoudnessStatus["level"];
  if (peakClipping) {
    level = "clipping";
  } else if (deviation > TOLERANCE_LU) {
    level = "too-loud";
    warnings.push(`Programme is ${deviation.toFixed(1)} LU above target — reduce gain or apply compression.`);
  } else if (deviation < -TOLERANCE_LU) {
    level = "too-quiet";
    warnings.push(`Programme is ${Math.abs(deviation).toFixed(1)} LU below target — increase gain by ${Math.abs(gainAdjustmentDb).toFixed(1)} dB.`);
  } else {
    level = "ok";
  }

  if (reading.loudnessRangeLu > 20) {
    warnings.push(`Loudness range ${reading.loudnessRangeLu} LU is wide — consider a gentle compressor.`);
  }

  const summary =
    level === "ok"
      ? `${reading.integratedLufs.toFixed(1)} LUFS — on target`
      : `${reading.integratedLufs.toFixed(1)} LUFS — ${level.replace("-", " ")}`;

  return { deviationLu: deviation, gainAdjustmentDb, peakClipping, level, warnings, summary };
}

/**
 * Build a full normalisation plan: evaluate loudness and recommend a compressor
 * ratio based on the loudness range to tighten the dynamic range.
 */
export function planLoudnessNormalisation(
  reading: LoudnessReading,
  targetId: string = "zoom"
): NormalisationPlan {
  const target = getLoudnessTarget(targetId) ?? loudnessTargets[5];
  const status = evaluateLoudness(reading, target);

  // Ratio suggestion: narrow range → no compression; wide range → gentle ratio
  let suggestedRatio: number;
  if (reading.loudnessRangeLu <= 6) {
    suggestedRatio = 1; // no compression needed
  } else if (reading.loudnessRangeLu <= 12) {
    suggestedRatio = 2;
  } else if (reading.loudnessRangeLu <= 20) {
    suggestedRatio = 3;
  } else {
    suggestedRatio = 4;
  }

  const limiterCeilingDbtp = target.maxTruePeakDbtp;

  return { target, status, suggestedRatio, limiterCeilingDbtp };
}

export function formatLufs(lufs: number): string {
  return `${lufs.toFixed(1)} LUFS`;
}

export function formatDbtp(dbtp: number): string {
  return `${dbtp >= 0 ? "+" : ""}${dbtp.toFixed(1)} dBTP`;
}
