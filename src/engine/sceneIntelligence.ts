/**
 * Scene intelligence engine.
 * Analyses real-time participant signals — active speaker, screen share,
 * participant count, roles — and recommends the most appropriate layout.
 * Pure functions; no React or side-effect dependencies.
 */

export type SceneLayout =
  | "host-focus"
  | "two-up"
  | "speaker-slides"
  | "smart-grid"
  | "outro";

export type SceneSignal = {
  /** Number of participants with video on */
  videoOnCount: number;
  /** Whether a screen share is currently active */
  screenShareActive: boolean;
  /** Whether an active speaker has been identified */
  hasActiveSpeaker: boolean;
  /** Whether the host's video is on */
  hostVideoOn: boolean;
  /** Whether the show is in an outro / wrap-up phase */
  isOutro: boolean;
};

export type SceneRecommendation = {
  layout: SceneLayout;
  sceneName: string;
  confidence: "high" | "medium" | "low";
  reason: string;
};

export type AutoSwitchDecision = {
  shouldSwitch: boolean;
  recommendation: SceneRecommendation;
  /** Minimum time to hold the current scene before switching (ms) */
  holdUntilMs: number;
};

/**
 * Recommend a scene layout from live participant signals.
 * Priority order: outro → screen share → participant count + speaker.
 */
export function recommendScene(signal: SceneSignal): SceneRecommendation {
  if (signal.isOutro) {
    return {
      layout: "outro",
      sceneName: "Closing",
      confidence: "high",
      reason: "Show is in outro phase — host closing card.",
    };
  }

  if (signal.screenShareActive) {
    return {
      layout: "speaker-slides",
      sceneName: "Presentation",
      confidence: "high",
      reason: "Screen share active — slides layout protects slide real estate.",
    };
  }

  if (signal.videoOnCount <= 1) {
    return {
      layout: "host-focus",
      sceneName: "Intro",
      confidence: signal.hostVideoOn ? "high" : "medium",
      reason:
        signal.videoOnCount === 0
          ? "No participant video — holding on host."
          : "Single video — host focus.",
    };
  }

  if (signal.videoOnCount === 2) {
    return {
      layout: "two-up",
      sceneName: "Interview",
      confidence: "high",
      reason: "Two active cameras — two-up interview layout.",
    };
  }

  if (signal.videoOnCount <= 6) {
    return {
      layout: "smart-grid",
      sceneName: "Panel",
      confidence: signal.hasActiveSpeaker ? "high" : "medium",
      reason: signal.hasActiveSpeaker
        ? `${signal.videoOnCount} participants — grid with active speaker highlight.`
        : `${signal.videoOnCount} participants — grid layout, no active speaker detected.`,
    };
  }

  // Large meeting (7+)
  return {
    layout: "smart-grid",
    sceneName: "Panel",
    confidence: "medium",
    reason: `Large meeting (${signal.videoOnCount} cameras) — smart grid.`,
  };
}

/**
 * Score how well a candidate layout matches the current signal (0–100).
 * Used to rank user-defined scenes against the recommendation.
 */
export function scoreLayoutMatch(
  candidateLayout: SceneLayout,
  signal: SceneSignal
): number {
  const recommended = recommendScene(signal);
  if (candidateLayout === recommended.layout) return 100;

  // Partial matches
  if (signal.screenShareActive && candidateLayout === "speaker-slides") return 90;
  if (!signal.screenShareActive && candidateLayout === "smart-grid" && signal.videoOnCount > 2)
    return 70;
  if (candidateLayout === "host-focus" && signal.videoOnCount <= 1) return 80;
  if (candidateLayout === "two-up" && signal.videoOnCount === 2) return 80;
  return 20;
}

/** Minimum hold time per layout before auto-switching (avoids thrash) */
const HOLD_MS: Record<SceneLayout, number> = {
  "host-focus": 3000,
  "two-up": 4000,
  "speaker-slides": 5000,
  "smart-grid": 4000,
  outro: 10000,
};

/**
 * Decide whether to auto-switch the scene given the current layout, signal,
 * and how long the current scene has been showing.
 */
export function decideAutoSwitch(
  currentLayout: SceneLayout,
  signal: SceneSignal,
  currentSceneElapsedMs: number
): AutoSwitchDecision {
  const recommendation = recommendScene(signal);
  const holdRequired = HOLD_MS[currentLayout];

  if (recommendation.layout === currentLayout) {
    return { shouldSwitch: false, recommendation, holdUntilMs: 0 };
  }

  if (currentSceneElapsedMs < holdRequired) {
    return {
      shouldSwitch: false,
      recommendation,
      holdUntilMs: holdRequired - currentSceneElapsedMs,
    };
  }

  // Only switch with medium or high confidence
  if (recommendation.confidence === "low") {
    return { shouldSwitch: false, recommendation, holdUntilMs: 0 };
  }

  return { shouldSwitch: true, recommendation, holdUntilMs: 0 };
}

export function describeRecommendation(rec: SceneRecommendation): string {
  return `${rec.sceneName} (${rec.layout}) — ${rec.confidence} confidence: ${rec.reason}`;
}

/**
 * Summarize the overall intelligence state for the UI status line.
 */
export function summarizeSceneIntelligence(
  signal: SceneSignal,
  automationOn: boolean
): string {
  const rec = recommendScene(signal);
  const mode = automationOn ? "Auto" : "Manual";
  return `${mode}: ${rec.sceneName} recommended (${rec.confidence} confidence)`;
}
