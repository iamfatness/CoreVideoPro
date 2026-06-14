/**
 * Pre-show countdown engine.
 * Manages the T-minus clock and warmup phases before a live broadcast starts.
 * Complements showClock.ts which tracks in-show segment timing.
 */

export type WarmupPhase =
  | "standby"       // Production open, not yet counting down
  | "lobby"         // Audience can join; host not yet live
  | "t-minus"       // Active countdown; presenter ready
  | "hot"           // Final 60s; go-live imminent
  | "live"          // Show has started
  | "post-show";    // Broadcast ended

export type CountdownState = {
  phase: WarmupPhase;
  /** Seconds remaining until planned go-live (negative = overdue) */
  secondsToLive: number;
  /** Formatted T-minus label: "T-05:00", "T+00:30" if overdue */
  tMinusLabel: string;
  /** Whether the operator should trigger go-live now */
  goLiveNow: boolean;
  /** Warning shown to operator during final 60s */
  hotWarning: string | null;
  summary: string;
};

export type ScheduledShow = {
  /** Planned go-live Unix timestamp in seconds */
  scheduledAtUnixSec: number;
  /** Duration of the lobby/waiting phase before countdown starts (seconds) */
  lobbyDurationSec: number;
  /** Title shown during lobby */
  showTitle: string;
};

/** Seconds before go-live when the phase flips to "hot" */
const HOT_THRESHOLD_SEC = 60;

/**
 * Compute the full countdown state given the current time and schedule.
 */
export function computeCountdown(
  nowUnixSec: number,
  show: ScheduledShow
): CountdownState {
  const secondsToLive = show.scheduledAtUnixSec - nowUnixSec;
  const lobbyStartsAt = show.scheduledAtUnixSec - show.lobbyDurationSec;

  let phase: WarmupPhase;
  if (nowUnixSec < lobbyStartsAt) {
    phase = "standby";
  } else if (nowUnixSec < show.scheduledAtUnixSec - HOT_THRESHOLD_SEC) {
    phase = "lobby";
  } else if (nowUnixSec < show.scheduledAtUnixSec) {
    phase = "hot";
  } else if (nowUnixSec === show.scheduledAtUnixSec) {
    phase = "t-minus"; // edge: exactly on time
  } else {
    // Past go-live — check if caller has explicitly set live
    phase = secondsToLive <= 0 ? "live" : "t-minus";
  }

  const tMinusLabel = formatTMinus(secondsToLive);
  const goLiveNow = phase === "hot" && secondsToLive <= 5;
  const hotWarning =
    phase === "hot"
      ? `Going live in ${formatCountdownSec(secondsToLive)} — stand by!`
      : null;

  const summary = buildSummary(phase, secondsToLive, show.showTitle);

  return { phase, secondsToLive, tMinusLabel, goLiveNow, hotWarning, summary };
}

/**
 * Advance the scheduled show by transitioning the phase to "live".
 * Returns updated state with live phase regardless of the clock.
 */
export function markShowLive(state: CountdownState): CountdownState {
  return {
    ...state,
    phase: "live",
    goLiveNow: false,
    hotWarning: null,
    summary: "Show is live",
  };
}

export function markShowOver(state: CountdownState): CountdownState {
  return {
    ...state,
    phase: "post-show",
    goLiveNow: false,
    hotWarning: null,
    summary: "Show ended",
  };
}

export function formatTMinus(secondsToLive: number): string {
  const sign = secondsToLive >= 0 ? "-" : "+";
  const abs = Math.abs(Math.round(secondsToLive));
  const h = Math.floor(abs / 3600);
  const m = Math.floor((abs % 3600) / 60);
  const s = abs % 60;
  if (h > 0) {
    return `T${sign}${h}:${String(m).padStart(2, "0")}:${String(s).padStart(2, "0")}`;
  }
  return `T${sign}${String(m).padStart(2, "0")}:${String(s).padStart(2, "0")}`;
}

function formatCountdownSec(sec: number): string {
  const abs = Math.abs(Math.round(sec));
  const m = Math.floor(abs / 60);
  const s = abs % 60;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

function buildSummary(phase: WarmupPhase, secondsToLive: number, title: string): string {
  switch (phase) {
    case "standby":
      return `${title} — standing by (${formatCountdownSec(secondsToLive)} to lobby)`;
    case "lobby":
      return `${title} — lobby open, ${formatCountdownSec(secondsToLive)} to go-live`;
    case "hot":
      return `${title} — HOT, going live in ${formatCountdownSec(secondsToLive)}`;
    case "t-minus":
      return `${title} — T-minus ${formatCountdownSec(Math.abs(secondsToLive))} (on time)`;
    case "live":
      return `${title} — LIVE`;
    case "post-show":
      return `${title} — ended`;
  }
}

/**
 * Build a cue sheet of pre-show milestones from the schedule.
 */
export type CueMilestone = {
  label: string;
  atUnixSec: number;
  secondsFromNow: number;
};

export function buildPreShowCues(
  nowUnixSec: number,
  show: ScheduledShow
): CueMilestone[] {
  const cues: CueMilestone[] = [
    {
      label: "Lobby opens",
      atUnixSec: show.scheduledAtUnixSec - show.lobbyDurationSec,
      secondsFromNow: show.scheduledAtUnixSec - show.lobbyDurationSec - nowUnixSec,
    },
    {
      label: "Hot zone",
      atUnixSec: show.scheduledAtUnixSec - HOT_THRESHOLD_SEC,
      secondsFromNow: show.scheduledAtUnixSec - HOT_THRESHOLD_SEC - nowUnixSec,
    },
    {
      label: "Go live",
      atUnixSec: show.scheduledAtUnixSec,
      secondsFromNow: show.scheduledAtUnixSec - nowUnixSec,
    },
  ];
  return cues;
}
