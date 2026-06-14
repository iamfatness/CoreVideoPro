import type { TransitionState } from "../domain/production";

export type TransitionStyle = TransitionState["style"];

export const transitionStyles: TransitionStyle[] = ["cut", "fade", "slide", "wipe", "stinger"];

const RECOMMENDED_DURATION: Record<TransitionStyle, number> = {
  cut: 0,
  fade: 420,
  slide: 520,
  wipe: 600,
  stinger: 900
};

const EASING: Record<TransitionStyle, string> = {
  cut: "steps(1, end)",
  fade: "ease-in-out",
  slide: "cubic-bezier(0.4, 0, 0.2, 1)",
  wipe: "cubic-bezier(0.7, 0, 0.3, 1)",
  stinger: "cubic-bezier(0.65, 0, 0.35, 1)"
};

const DURATION_BOUNDS: Record<TransitionStyle, { min: number; max: number }> = {
  cut: { min: 0, max: 0 },
  fade: { min: 120, max: 1200 },
  slide: { min: 160, max: 1200 },
  wipe: { min: 200, max: 1500 },
  stinger: { min: 400, max: 2000 }
};

export function recommendedTransitionDuration(style: TransitionStyle): number {
  return RECOMMENDED_DURATION[style];
}

export function transitionEasing(style: TransitionStyle): string {
  return EASING[style];
}

/**
 * Wipe-style transitions (the hard wipe and the branded stinger) play a
 * full-frame animated overlay rather than a simple cross-dissolve.
 */
export function transitionUsesWipe(style: TransitionStyle): boolean {
  return style === "wipe" || style === "stinger";
}

export function clampTransitionDuration(style: TransitionStyle, durationMs: number): number {
  const bounds = DURATION_BOUNDS[style];
  if (Number.isNaN(durationMs)) {
    return bounds.min;
  }

  return Math.min(bounds.max, Math.max(bounds.min, Math.round(durationMs)));
}

export function describeTransition(style: TransitionStyle, durationMs: number): {
  label: string;
  easing: string;
  usesWipe: boolean;
  summary: string;
} {
  const usesWipe = transitionUsesWipe(style);
  const label = style.charAt(0).toUpperCase() + style.slice(1);
  const motion =
    style === "cut" ? "instant" : style === "stinger" ? "branded wipe" : usesWipe ? "hard wipe" : "cross-dissolve";

  return {
    label,
    easing: transitionEasing(style),
    usesWipe,
    summary: style === "cut" ? `${label} - instant` : `${label} - ${durationMs}ms ${motion}`
  };
}
