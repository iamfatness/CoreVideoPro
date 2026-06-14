/**
 * Show clock and rundown timer engine.
 * Tracks elapsed time per segment and total show, compares against planned
 * durations, and surfaces over/under counts so operators can pace the show.
 */

export type RundownSegment = {
  id: string;
  name: string;
  /** Planned duration in seconds */
  plannedSeconds: number;
};

export type SegmentTimer = {
  segmentId: string;
  name: string;
  plannedSeconds: number;
  /** Elapsed seconds in this segment */
  elapsedSeconds: number;
  /** Remaining seconds (negative = over) */
  remainingSeconds: number;
  /** Fractional progress 0–1 (can exceed 1 when over) */
  progress: number;
  status: "upcoming" | "running" | "over" | "done";
};

export type ShowClockState = {
  /** Total planned show duration in seconds */
  totalPlannedSeconds: number;
  /** Total elapsed seconds since show start */
  elapsedSeconds: number;
  remainingSeconds: number;
  /** Index of the active segment (0-based), -1 if not started */
  activeSegmentIndex: number;
  segments: SegmentTimer[];
  /** true when the show is running over total planned time */
  showOver: boolean;
  /** Formatted elapsed: "mm:ss" or "h:mm:ss" */
  elapsedLabel: string;
  /** Formatted remaining or over: "+mm:ss" if over */
  remainingLabel: string;
};

export function formatClock(seconds: number, showSign = false): string {
  const abs = Math.abs(Math.round(seconds));
  const h = Math.floor(abs / 3600);
  const m = Math.floor((abs % 3600) / 60);
  const s = abs % 60;
  const sign = showSign && seconds < 0 ? "+" : showSign && seconds >= 0 ? "-" : "";
  if (h > 0) {
    return `${sign}${h}:${String(m).padStart(2, "0")}:${String(s).padStart(2, "0")}`;
  }
  return `${sign}${String(m).padStart(2, "0")}:${String(s).padStart(2, "0")}`;
}

/**
 * Compute the full show clock state given the rundown, which segment is active,
 * and how many seconds have elapsed in the current segment.
 */
export function computeShowClock(
  segments: RundownSegment[],
  activeSegmentIndex: number,
  segmentElapsedSeconds: number,
  /** Total show elapsed including all completed segments */
  totalElapsedSeconds: number
): ShowClockState {
  const totalPlannedSeconds = segments.reduce((sum, s) => sum + s.plannedSeconds, 0);
  const remainingSeconds = totalPlannedSeconds - totalElapsedSeconds;
  const showOver = totalElapsedSeconds > totalPlannedSeconds;

  // Compute elapsed per segment from the active index + segment elapsed
  let cumulativeElapsed = 0;
  const segmentTimers: SegmentTimer[] = segments.map((seg, index) => {
    let elapsed: number;
    let status: SegmentTimer["status"];

    if (index < activeSegmentIndex) {
      // Completed segment — assume full planned duration was used
      elapsed = seg.plannedSeconds;
      status = "done";
    } else if (index === activeSegmentIndex) {
      elapsed = segmentElapsedSeconds;
      status = elapsed > seg.plannedSeconds ? "over" : "running";
    } else {
      elapsed = 0;
      status = "upcoming";
    }

    cumulativeElapsed += elapsed;
    const remaining = seg.plannedSeconds - elapsed;
    const progress = seg.plannedSeconds > 0 ? elapsed / seg.plannedSeconds : 0;

    return {
      segmentId: seg.id,
      name: seg.name,
      plannedSeconds: seg.plannedSeconds,
      elapsedSeconds: elapsed,
      remainingSeconds: remaining,
      progress: Math.min(2, progress), // cap at 2× for display
      status
    };
  });

  return {
    totalPlannedSeconds,
    elapsedSeconds: totalElapsedSeconds,
    remainingSeconds,
    activeSegmentIndex,
    segments: segmentTimers,
    showOver,
    elapsedLabel: formatClock(totalElapsedSeconds),
    remainingLabel: formatClock(remainingSeconds, true)
  };
}

/**
 * Build a default rundown from scene names with equal time distribution.
 */
export function buildRundownFromScenes(
  sceneNames: string[],
  totalPlannedMinutes: number
): RundownSegment[] {
  if (sceneNames.length === 0) return [];
  const perSegmentSeconds = Math.round((totalPlannedMinutes * 60) / sceneNames.length);
  return sceneNames.map((name, index) => ({
    id: `seg-${index + 1}`,
    name,
    plannedSeconds: perSegmentSeconds
  }));
}

/**
 * Suggest total show duration from segment count and typical Zoom webinar pacing.
 */
export function suggestShowDurationMinutes(segmentCount: number): number {
  if (segmentCount <= 3) return 30;
  if (segmentCount <= 6) return 45;
  if (segmentCount <= 10) return 60;
  return 90;
}
