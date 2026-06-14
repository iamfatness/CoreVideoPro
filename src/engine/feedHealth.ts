/**
 * Feed health state machine for Zoom participant sources.
 * Tracks the full lifecycle: waiting → live → stale → recovering → disconnected.
 * Each transition carries a reason string for operator display.
 */

export type FeedHealthState =
  | "waiting"
  | "live"
  | "stale"
  | "low-resolution"
  | "muted"
  | "video-off"
  | "disconnected"
  | "recovering";

export type FeedHealthSignal = {
  hasVideo: boolean;
  hasAudio: boolean;
  isMuted: boolean;
  isVideoOff: boolean;
  /** 0 = no video; 1 = 720p; 2 = 1080p; 3 = 4K */
  resolutionTier: 0 | 1 | 2 | 3;
  /** ms since last frame received; undefined = never connected */
  msSinceLastFrame?: number;
  /** true when the peer is in process of reconnecting */
  isReconnecting: boolean;
};

export type FeedHealthStatus = {
  state: FeedHealthState;
  label: string;
  detail: string;
  /** true when feed is usable for program output */
  usable: boolean;
  /** true when operator attention is recommended */
  needsAttention: boolean;
};

const STALE_THRESHOLD_MS = 3000;
const DISCONNECTED_THRESHOLD_MS = 10000;

export function evaluateFeedHealth(signal: FeedHealthSignal): FeedHealthStatus {
  const { msSinceLastFrame, isReconnecting, isVideoOff, isMuted, hasVideo, resolutionTier } = signal;

  // Never connected
  if (msSinceLastFrame === undefined) {
    return {
      state: "waiting",
      label: "Waiting",
      detail: "No frames received yet — participant may not have joined.",
      usable: false,
      needsAttention: false,
    };
  }

  // Reconnecting
  if (isReconnecting) {
    return {
      state: "recovering",
      label: "Recovering",
      detail: "Participant is reconnecting — feed will resume shortly.",
      usable: false,
      needsAttention: true,
    };
  }

  // Disconnected (long gap)
  if (msSinceLastFrame >= DISCONNECTED_THRESHOLD_MS) {
    return {
      state: "disconnected",
      label: "Disconnected",
      detail: `No frames for ${(msSinceLastFrame / 1000).toFixed(0)} s — participant may have left.`,
      usable: false,
      needsAttention: true,
    };
  }

  // Stale (short gap, likely network hiccup)
  if (msSinceLastFrame >= STALE_THRESHOLD_MS) {
    return {
      state: "stale",
      label: "Stale",
      detail: `Feed paused for ${(msSinceLastFrame / 1000).toFixed(1)} s — possible network issue.`,
      usable: false,
      needsAttention: true,
    };
  }

  // Video explicitly turned off
  if (isVideoOff || !hasVideo) {
    return {
      state: "video-off",
      label: "Video off",
      detail: "Participant has disabled their camera.",
      usable: false,
      needsAttention: false,
    };
  }

  // Low resolution
  if (resolutionTier === 1) {
    return {
      state: "low-resolution",
      label: "Low res",
      detail: "Feed is 720p or below — may look soft on program output.",
      usable: true,
      needsAttention: false,
    };
  }

  // No video at all (resolution 0) but wasn't explicitly off
  if (resolutionTier === 0) {
    return {
      state: "video-off",
      label: "No video",
      detail: "Participant is not sending a video stream.",
      usable: false,
      needsAttention: false,
    };
  }

  // Audio-only concerns
  if (isMuted) {
    return {
      state: "muted",
      label: "Muted",
      detail: "Participant is muted — audio will not reach program mix.",
      usable: true,
      needsAttention: false,
    };
  }

  // Fully live
  return {
    state: "live",
    label: "Live",
    detail: resolutionTier >= 3 ? "4K feed — excellent quality." : "Feed is live and healthy.",
    usable: true,
    needsAttention: false,
  };
}

/**
 * Summarize feed health across a roster so the operator gets a single line.
 * Returns counts by state and a highlight of the worst issue.
 */
export type RosterHealthSummary = {
  total: number;
  live: number;
  needsAttention: number;
  usable: number;
  worstState: FeedHealthState | null;
  summary: string;
};

export function summarizeRosterHealth(statuses: FeedHealthStatus[]): RosterHealthSummary {
  if (statuses.length === 0) {
    return { total: 0, live: 0, needsAttention: 0, usable: 0, worstState: null, summary: "No participants" };
  }

  const liveCount = statuses.filter((s) => s.state === "live").length;
  const usableCount = statuses.filter((s) => s.usable).length;
  const attentionCount = statuses.filter((s) => s.needsAttention).length;

  const worstPriority: FeedHealthState[] = ["disconnected", "recovering", "stale", "video-off", "muted", "low-resolution", "waiting", "live"];
  const worstState = statuses
    .map((s) => s.state)
    .sort((a, b) => worstPriority.indexOf(a) - worstPriority.indexOf(b))[0] ?? null;

  let summary: string;
  if (attentionCount === 0) {
    summary = `${liveCount}/${statuses.length} live`;
  } else {
    summary = `${liveCount}/${statuses.length} live — ${attentionCount} need${attentionCount === 1 ? "s" : ""} attention`;
  }

  return {
    total: statuses.length,
    live: liveCount,
    needsAttention: attentionCount,
    usable: usableCount,
    worstState,
    summary,
  };
}

export function feedHealthBadgeColor(state: FeedHealthState): string {
  switch (state) {
    case "live": return "green";
    case "low-resolution":
    case "muted": return "yellow";
    case "stale":
    case "recovering":
    case "video-off": return "orange";
    case "disconnected": return "red";
    case "waiting": return "gray";
  }
}
