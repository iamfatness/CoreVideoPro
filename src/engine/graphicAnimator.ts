/**
 * Graphic animator engine.
 * Manages keyframe-based animation state for lower-thirds, bugs,
 * and full-screen graphics. Pure functions — no timers or side effects.
 */

export type EasingFunction =
  | "linear"
  | "ease-in"
  | "ease-out"
  | "ease-in-out"
  | "bounce-out"
  | "spring";

export type AnimationPhase = "idle" | "intro" | "hold" | "outro" | "done";

export type AnimationDirection = "in" | "out";

export type AnimationPreset = {
  id: string;
  name: string;
  introDurationMs: number;
  holdDurationMs: number;
  outroDurationMs: number;
  easing: EasingFunction;
  /** CSS transform for the off-screen start/end position */
  offscreenTransform: string;
};

export type AnimationState = {
  preset: AnimationPreset;
  phase: AnimationPhase;
  /** Elapsed ms within the current phase */
  phaseElapsedMs: number;
  /** Overall elapsed ms since animation started */
  totalElapsedMs: number;
};

export type AnimationFrame = {
  phase: AnimationPhase;
  /** 0–1 progress within the current phase */
  phaseProgress: number;
  /** Eased 0–1 value for the current phase */
  easedValue: number;
  /** CSS transform to apply to the graphic element */
  cssTransform: string;
  /** CSS opacity */
  opacity: number;
  done: boolean;
};

/** Built-in animation presets */
export const animationPresets: AnimationPreset[] = [
  {
    id: "slide-up",
    name: "Slide Up",
    introDurationMs: 400,
    holdDurationMs: 5000,
    outroDurationMs: 300,
    easing: "ease-out",
    offscreenTransform: "translateY(60px)",
  },
  {
    id: "fade",
    name: "Fade",
    introDurationMs: 500,
    holdDurationMs: 5000,
    outroDurationMs: 400,
    easing: "ease-in-out",
    offscreenTransform: "translateY(0)",
  },
  {
    id: "slide-left",
    name: "Slide Left",
    introDurationMs: 350,
    holdDurationMs: 5000,
    outroDurationMs: 280,
    easing: "ease-out",
    offscreenTransform: "translateX(-100px)",
  },
  {
    id: "bounce",
    name: "Bounce In",
    introDurationMs: 600,
    holdDurationMs: 5000,
    outroDurationMs: 300,
    easing: "bounce-out",
    offscreenTransform: "translateY(40px)",
  },
  {
    id: "spring",
    name: "Spring",
    introDurationMs: 700,
    holdDurationMs: 5000,
    outroDurationMs: 350,
    easing: "spring",
    offscreenTransform: "scale(0.8) translateY(20px)",
  },
];

export function getAnimationPreset(id: string): AnimationPreset | undefined {
  return animationPresets.find((p) => p.id === id);
}

/**
 * Start a new animation from the beginning.
 */
export function startAnimation(preset: AnimationPreset): AnimationState {
  return { preset, phase: "intro", phaseElapsedMs: 0, totalElapsedMs: 0 };
}

/**
 * Advance animation state by deltaMs. Returns updated state.
 */
export function advanceAnimation(state: AnimationState, deltaMs: number): AnimationState {
  if (state.phase === "idle" || state.phase === "done") return state;

  const totalElapsedMs = state.totalElapsedMs + deltaMs;
  let phaseElapsedMs = state.phaseElapsedMs + deltaMs;
  let currentPhase: AnimationPhase = state.phase;

  const { introDurationMs, holdDurationMs, outroDurationMs } = state.preset;

  if (currentPhase === "intro" && phaseElapsedMs >= introDurationMs) {
    phaseElapsedMs -= introDurationMs;
    currentPhase = "hold";
  }

  if (currentPhase === "hold" && phaseElapsedMs >= holdDurationMs) {
    phaseElapsedMs -= holdDurationMs;
    currentPhase = "outro";
  }

  if (currentPhase === "outro" && phaseElapsedMs >= outroDurationMs) {
    currentPhase = "done";
    phaseElapsedMs = outroDurationMs;
  }

  return { ...state, phase: currentPhase, phaseElapsedMs, totalElapsedMs };
}

/**
 * Trigger the outro early (operator dismisses graphic).
 */
export function dismissAnimation(state: AnimationState): AnimationState {
  if (state.phase === "intro" || state.phase === "hold") {
    return { ...state, phase: "outro", phaseElapsedMs: 0 };
  }
  return state;
}

/**
 * Compute the visual frame for the current animation state.
 */
export function computeAnimationFrame(state: AnimationState): AnimationFrame {
  const { preset, phase, phaseElapsedMs } = state;

  if (phase === "idle") {
    return { phase, phaseProgress: 0, easedValue: 0, cssTransform: preset.offscreenTransform, opacity: 0, done: false };
  }

  if (phase === "done") {
    return { phase, phaseProgress: 1, easedValue: 0, cssTransform: preset.offscreenTransform, opacity: 0, done: true };
  }

  let phaseProgress: number;
  let easedValue: number;
  let cssTransform: string;
  let opacity: number;

  if (phase === "intro") {
    phaseProgress = Math.min(1, phaseElapsedMs / preset.introDurationMs);
    easedValue = applyEasing(phaseProgress, preset.easing);
    cssTransform = interpolateTransform(preset.offscreenTransform, "none", easedValue);
    opacity = easedValue;
  } else if (phase === "hold") {
    phaseProgress = Math.min(1, phaseElapsedMs / preset.holdDurationMs);
    easedValue = 1;
    cssTransform = "none";
    opacity = 1;
  } else {
    // outro
    phaseProgress = Math.min(1, phaseElapsedMs / preset.outroDurationMs);
    easedValue = applyEasing(1 - phaseProgress, "ease-in");
    cssTransform = interpolateTransform(preset.offscreenTransform, "none", 1 - easedValue);
    opacity = easedValue;
  }

  return { phase, phaseProgress, easedValue, cssTransform, opacity, done: false };
}

/**
 * Total animation duration in ms.
 */
export function animationTotalDurationMs(preset: AnimationPreset): number {
  return preset.introDurationMs + preset.holdDurationMs + preset.outroDurationMs;
}

/**
 * Apply an easing function to a linear progress value (0–1).
 */
export function applyEasing(t: number, easing: EasingFunction): number {
  const clamped = Math.max(0, Math.min(1, t));
  switch (easing) {
    case "linear":
      return clamped;
    case "ease-in":
      return clamped * clamped;
    case "ease-out":
      return 1 - (1 - clamped) * (1 - clamped);
    case "ease-in-out":
      return clamped < 0.5 ? 2 * clamped * clamped : 1 - (-2 * clamped + 2) ** 2 / 2;
    case "bounce-out": {
      const n1 = 7.5625;
      const d1 = 2.75;
      if (clamped < 1 / d1) return n1 * clamped * clamped;
      if (clamped < 2 / d1) return n1 * (clamped - 1.5 / d1) ** 2 + 0.75;
      if (clamped < 2.5 / d1) return n1 * (clamped - 2.25 / d1) ** 2 + 0.9375;
      return n1 * (clamped - 2.625 / d1) ** 2 + 0.984375;
    }
    case "spring": {
      // Damped spring approximation
      if (clamped >= 1) return 1;
      const w = 2 * Math.PI * 1.5;
      return 1 - Math.exp(-5 * clamped) * Math.cos(w * clamped);
    }
  }
}

/**
 * Linearly interpolate between two transform strings using a simple numeric scale.
 * For the subset used by presets (single translateX/Y or scale+translate).
 */
export function interpolateTransform(from: string, _to: string, t: number): string {
  if (t >= 1) return "none";
  if (t <= 0) return from;

  // Parse translate values from offscreen transform
  const translateY = parseTranslateY(from);
  const translateX = parseTranslateX(from);
  const scale = parseScale(from);

  const parts: string[] = [];
  if (scale !== null) {
    const s = 1 + (scale - 1) * (1 - t);
    parts.push(`scale(${s.toFixed(4)})`);
  }
  if (translateX !== 0) parts.push(`translateX(${(translateX * (1 - t)).toFixed(2)}px)`);
  if (translateY !== 0) parts.push(`translateY(${(translateY * (1 - t)).toFixed(2)}px)`);

  return parts.length > 0 ? parts.join(" ") : "none";
}

function parseTranslateY(transform: string): number {
  const m = transform.match(/translateY\((-?[\d.]+)px\)/);
  return m ? parseFloat(m[1]) : 0;
}

function parseTranslateX(transform: string): number {
  const m = transform.match(/translateX\((-?[\d.]+)px\)/);
  return m ? parseFloat(m[1]) : 0;
}

function parseScale(transform: string): number | null {
  const m = transform.match(/scale\(([\d.]+)\)/);
  return m ? parseFloat(m[1]) : null;
}
