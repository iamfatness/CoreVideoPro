/**
 * Caption quality and visibility engine.
 * Analyses the rolling transcript and live timing signals to decide when
 * captions should be visible, fading, or hidden, and surfaces quality
 * metrics the operator can use to assess the caption pipeline health.
 */

export type DeadAirState = "live" | "brief-pause" | "dead-air" | "silence";

export type CaptionVisibilityState = "visible" | "fading" | "hidden" | "pending";

export type CaptionVisibilityDecision = {
  state: CaptionVisibilityState;
  shouldShow: boolean;
  /** Recommended CSS transition delay before hiding (ms) */
  fadeDelayMs: number;
  reason: string;
};

export type CaptionQualityReport = {
  /** Average confidence across the window (0–100) */
  avgConfidence: number;
  /** Lowest confidence entry in the window */
  minConfidence: number;
  /** Detected quality tier */
  tier: "excellent" | "good" | "degraded" | "poor";
  /** Number of transcript entries analysed */
  sampleCount: number;
  warnings: string[];
};

export type CaptionSignal = {
  /** Confidence reported by the ASR engine (0–100) */
  confidencePct: number;
  /** Milliseconds since the last recognised word arrived */
  msSinceLastWord: number;
  /** Current caption text (empty when nothing recognised) */
  text: string;
  /** Whether an active speaker is detected */
  activeSpeakerPresent: boolean;
  /** Whether a screen share is active (lowers the urgency of caption display) */
  screenShareActive: boolean;
};

/** Thresholds for dead-air classification */
const BRIEF_PAUSE_MS = 1500;
const DEAD_AIR_MS = 4000;
const SILENCE_MS = 10000;

export function detectDeadAir(msSinceLastWord: number): DeadAirState {
  if (msSinceLastWord < BRIEF_PAUSE_MS) return "live";
  if (msSinceLastWord < DEAD_AIR_MS) return "brief-pause";
  if (msSinceLastWord < SILENCE_MS) return "dead-air";
  return "silence";
}

/** Fade delay per dead-air state: longer pauses get a longer grace window */
const FADE_DELAY_MS: Record<DeadAirState, number> = {
  live: 0,
  "brief-pause": 1000,
  "dead-air": 500,
  silence: 0,
};

/**
 * Decide whether to show, fade, or hide the caption strip based on the
 * current ASR signal and timing.
 */
export function decideCaptionVisibility(signal: CaptionSignal): CaptionVisibilityDecision {
  const deadAir = detectDeadAir(signal.msSinceLastWord);

  // No speech and no active speaker → hide
  if (deadAir === "silence" || (!signal.activeSpeakerPresent && deadAir !== "live")) {
    return {
      state: "hidden",
      shouldShow: false,
      fadeDelayMs: 0,
      reason: "No speech detected — caption strip hidden.",
    };
  }

  // Active speaker present but nothing recognised yet → pending
  if (signal.activeSpeakerPresent && signal.text.trim().length === 0 && deadAir !== "live") {
    return {
      state: "pending",
      shouldShow: false,
      fadeDelayMs: 500,
      reason: "Speaker detected but captions not yet arrived.",
    };
  }

  // Low confidence → still show but warn the caller
  if (signal.confidencePct < 70 && signal.text.trim().length > 0) {
    return {
      state: "visible",
      shouldShow: true,
      fadeDelayMs: 0,
      reason: `Low confidence (${signal.confidencePct}%) — captions shown with reduced trust.`,
    };
  }

  // Dead-air but text is still on screen → fading
  if (deadAir === "dead-air" || deadAir === "brief-pause") {
    return {
      state: "fading",
      shouldShow: true,
      fadeDelayMs: FADE_DELAY_MS[deadAir],
      reason: `Pause detected (${(signal.msSinceLastWord / 1000).toFixed(1)}s) — fading captions.`,
    };
  }

  return {
    state: "visible",
    shouldShow: true,
    fadeDelayMs: 0,
    reason: "Active speech — captions visible.",
  };
}

/**
 * Compute a quality report across a window of transcript entries.
 * Pass the entries and the time window they were collected over.
 */
export function computeCaptionQuality(
  confidenceValues: number[],
): CaptionQualityReport {
  const warnings: string[] = [];

  if (confidenceValues.length === 0) {
    return {
      avgConfidence: 0,
      minConfidence: 0,
      tier: "poor",
      sampleCount: 0,
      warnings: ["No caption samples collected yet."],
    };
  }

  const avgConfidence =
    confidenceValues.reduce((sum, c) => sum + c, 0) / confidenceValues.length;
  const minConfidence = Math.min(...confidenceValues);

  let tier: CaptionQualityReport["tier"];
  if (avgConfidence >= 92) {
    tier = "excellent";
  } else if (avgConfidence >= 80) {
    tier = "good";
  } else if (avgConfidence >= 65) {
    tier = "degraded";
    warnings.push("Caption confidence is degraded — check audio levels and background noise.");
  } else {
    tier = "poor";
    warnings.push("Caption confidence is poor — transcription may be unreliable.");
  }

  if (minConfidence < 60) {
    warnings.push(`Minimum confidence ${minConfidence}% detected in one or more captions.`);
  }

  return {
    avgConfidence: Math.round(avgConfidence),
    minConfidence,
    tier,
    sampleCount: confidenceValues.length,
    warnings,
  };
}

export function captionQualityLabel(tier: CaptionQualityReport["tier"]): string {
  switch (tier) {
    case "excellent":
      return "Excellent";
    case "good":
      return "Good";
    case "degraded":
      return "Degraded";
    case "poor":
      return "Poor";
  }
}

export function summarizeCaptionQuality(report: CaptionQualityReport): string {
  if (report.sampleCount === 0) return "No caption data";
  return `${captionQualityLabel(report.tier)} — avg ${report.avgConfidence}% confidence (${report.sampleCount} samples)`;
}
