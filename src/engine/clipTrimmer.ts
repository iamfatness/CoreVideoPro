/**
 * Clip trimmer engine.
 * Manages in/out point editing, ripple trim, and timecode helpers
 * for media bin clips before they enter the rundown or graphics stack.
 */

export type TimecodeFormat = "HH:MM:SS:FF" | "HH:MM:SS.mmm" | "seconds";

export type ClipRegion = {
  /** In point in seconds from the start of the source file */
  inPointSec: number;
  /** Out point in seconds from the start of the source file */
  outPointSec: number;
  /** Frame rate of the source clip */
  fps: number;
};

export type TrimResult = {
  region: ClipRegion;
  durationSec: number;
  warnings: string[];
  valid: boolean;
};

export type RippleTrimOperation = {
  /** Which edge to move */
  edge: "in" | "out";
  /** Delta in seconds (positive = move later, negative = move earlier) */
  deltaSec: number;
  /** Clamp to source file bounds */
  sourceDurationSec: number;
};

export type ClipMarker = {
  id: string;
  positionSec: number;
  label: string;
  color: string;
};

/** Minimum clip duration to avoid zero-length clips */
const MIN_DURATION_SEC = 1 / 30; // one frame at 30fps

/**
 * Validate and clamp a clip region against source bounds.
 */
export function trimClip(
  region: ClipRegion,
  sourceDurationSec: number
): TrimResult {
  const warnings: string[] = [];

  let { inPointSec, outPointSec, fps } = region;

  if (inPointSec < 0) {
    warnings.push(`In point clamped to 0 (was ${inPointSec.toFixed(3)}s)`);
    inPointSec = 0;
  }

  if (outPointSec > sourceDurationSec) {
    warnings.push(`Out point clamped to source end (${sourceDurationSec.toFixed(3)}s)`);
    outPointSec = sourceDurationSec;
  }

  if (inPointSec >= outPointSec) {
    warnings.push("In point must be before out point — out point pushed forward");
    outPointSec = Math.min(inPointSec + MIN_DURATION_SEC, sourceDurationSec);
  }

  const durationSec = outPointSec - inPointSec;
  const valid = durationSec >= MIN_DURATION_SEC && warnings.length === 0;

  return {
    region: { inPointSec, outPointSec, fps },
    durationSec,
    warnings,
    valid,
  };
}

/**
 * Apply a ripple trim — move the in or out point by a delta.
 */
export function rippleTrim(
  region: ClipRegion,
  op: RippleTrimOperation
): TrimResult {
  let next: ClipRegion;

  if (op.edge === "in") {
    next = { ...region, inPointSec: region.inPointSec + op.deltaSec };
  } else {
    next = { ...region, outPointSec: region.outPointSec + op.deltaSec };
  }

  return trimClip(next, op.sourceDurationSec);
}

/**
 * Snap a position to the nearest frame boundary.
 */
export function snapToFrame(positionSec: number, fps: number): number {
  if (fps <= 0) return positionSec;
  const frameDuration = 1 / fps;
  return Math.round(positionSec / frameDuration) * frameDuration;
}

/**
 * Convert seconds to a timecode string.
 */
export function formatTimecode(
  seconds: number,
  fps: number,
  format: TimecodeFormat = "HH:MM:SS:FF"
): string {
  const totalSeconds = Math.max(0, seconds);

  if (format === "seconds") {
    return `${totalSeconds.toFixed(3)}s`;
  }

  if (format === "HH:MM:SS.mmm") {
    const h = Math.floor(totalSeconds / 3600);
    const m = Math.floor((totalSeconds % 3600) / 60);
    const s = Math.floor(totalSeconds % 60);
    const ms = Math.round((totalSeconds % 1) * 1000);
    return `${String(h).padStart(2, "0")}:${String(m).padStart(2, "0")}:${String(s).padStart(2, "0")}.${String(ms).padStart(3, "0")}`;
  }

  // HH:MM:SS:FF (default)
  const h = Math.floor(totalSeconds / 3600);
  const m = Math.floor((totalSeconds % 3600) / 60);
  const s = Math.floor(totalSeconds % 60);
  const frame = Math.round((totalSeconds % 1) * fps);
  return `${String(h).padStart(2, "0")}:${String(m).padStart(2, "0")}:${String(s).padStart(2, "0")}:${String(frame).padStart(2, "0")}`;
}

/**
 * Parse a timecode string to seconds.
 * Accepts "HH:MM:SS:FF" (with fps) or "HH:MM:SS.mmm".
 */
export function parseTimecode(timecode: string, fps: number): number {
  // Try HH:MM:SS:FF
  const frameMatch = timecode.match(/^(\d+):(\d+):(\d+):(\d+)$/);
  if (frameMatch) {
    const [, h, m, s, f] = frameMatch.map(Number);
    return h * 3600 + m * 60 + s + f / fps;
  }

  // Try HH:MM:SS.mmm
  const msMatch = timecode.match(/^(\d+):(\d+):(\d+)\.(\d+)$/);
  if (msMatch) {
    const [, h, m, s, ms] = msMatch.map(Number);
    return h * 3600 + m * 60 + s + ms / 1000;
  }

  // Try plain seconds
  const secMatch = timecode.match(/^(\d+(?:\.\d+)?)s?$/);
  if (secMatch) {
    return parseFloat(secMatch[1]);
  }

  return 0;
}

/**
 * Add a marker to a clip.
 */
export function addMarker(
  markers: ClipMarker[],
  positionSec: number,
  label: string,
  color = "#fbbf24"
): ClipMarker[] {
  const id = `m-${Date.now()}-${Math.random().toString(36).slice(2, 6)}`;
  return [...markers, { id, positionSec, label, color }].sort(
    (a, b) => a.positionSec - b.positionSec
  );
}

/**
 * Remove a marker by id.
 */
export function removeMarker(markers: ClipMarker[], markerId: string): ClipMarker[] {
  return markers.filter((m) => m.id !== markerId);
}

/**
 * Find the nearest marker to a given position within a tolerance.
 */
export function nearestMarker(
  markers: ClipMarker[],
  positionSec: number,
  toleranceSec = 0.5
): ClipMarker | null {
  let best: ClipMarker | null = null;
  let bestDist = Infinity;
  for (const marker of markers) {
    const dist = Math.abs(marker.positionSec - positionSec);
    if (dist <= toleranceSec && dist < bestDist) {
      best = marker;
      bestDist = dist;
    }
  }
  return best;
}

/**
 * Summarize a trimmed clip region for display.
 */
export function summarizeTrim(result: TrimResult, fps: number): string {
  const inTC = formatTimecode(result.region.inPointSec, fps);
  const outTC = formatTimecode(result.region.outPointSec, fps);
  const dur = formatTimecode(result.durationSec, fps);
  if (!result.valid) {
    return `Invalid clip: ${result.warnings.join("; ")}`;
  }
  return `In: ${inTC} → Out: ${outTC} (${dur})`;
}
