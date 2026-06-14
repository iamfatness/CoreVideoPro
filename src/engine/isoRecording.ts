/**
 * ISO recording plan engine.
 * Assigns per-participant ISO tracks, estimates file sizes, validates disk
 * constraints, and builds the output folder structure for a recording session.
 */

export type IsoTrackSource = "participant" | "program-mix" | "screen-share" | "ambient";

export type IsoResolutionTier = "1080p" | "720p" | "480p";

export type IsoTrack = {
  trackIndex: number;
  source: IsoTrackSource;
  /** Participant id when source is "participant" */
  participantId?: string;
  label: string;
  /** Estimated video bitrate in Mbps */
  estimatedBitrateMbps: number;
  resolutionTier: IsoResolutionTier;
};

export type IsoRecordingPlan = {
  tracks: IsoTrack[];
  /** Sum of all track bitrates */
  estimatedTotalMbps: number;
  /** Estimated combined file size in GB for one hour of recording */
  estimatedFileSizeGbPerHour: number;
  /** Suggested output folder path (relative) */
  outputFolder: string;
  warnings: string[];
  valid: boolean;
};

export type IsoRecordingOptions = {
  /** Include a program mix (down-mix of all participants) track */
  includeProgramMix?: boolean;
  /** Include a screen-share ISO track when a screen share is active */
  includeScreenShare?: boolean;
  /** Resolution tier to record participant tracks at */
  participantResolution?: IsoResolutionTier;
  /** Session name used for the output folder */
  sessionName?: string;
  /** Maximum number of participant tracks (hardware/license limit) */
  maxParticipantTracks?: number;
};

/** Approximate video bitrates per resolution tier at H.264 HQ settings */
const BITRATE_BY_TIER: Record<IsoResolutionTier, number> = {
  "1080p": 6,
  "720p": 4,
  "480p": 2,
};

const MAX_TRACKS_DEFAULT = 8;

export function estimateIsoTrackBitrateMbps(
  source: IsoTrackSource,
  resolution: IsoResolutionTier = "1080p"
): number {
  if (source === "program-mix") return BITRATE_BY_TIER[resolution] * 1.5;
  if (source === "screen-share") return 8; // screen share needs higher bitrate for text clarity
  if (source === "ambient") return 1;
  return BITRATE_BY_TIER[resolution];
}

export function isoTrackLabel(
  source: IsoTrackSource,
  participantName?: string
): string {
  switch (source) {
    case "participant":
      return participantName ?? "Participant";
    case "program-mix":
      return "Program Mix";
    case "screen-share":
      return "Screen Share";
    case "ambient":
      return "Ambient";
  }
}

/**
 * Build the ISO recording plan from a list of participant ids+names, plus
 * optional program-mix and screen-share tracks.
 */
export function planIsoRecording(
  participants: Array<{ id: string; name: string }>,
  options: IsoRecordingOptions = {}
): IsoRecordingPlan {
  const {
    includeProgramMix = true,
    includeScreenShare = false,
    participantResolution = "1080p",
    sessionName = "session",
    maxParticipantTracks = MAX_TRACKS_DEFAULT,
  } = options;

  const warnings: string[] = [];
  const tracks: IsoTrack[] = [];

  // Participant tracks (capped at maxParticipantTracks)
  const selected = participants.slice(0, maxParticipantTracks);
  if (participants.length > maxParticipantTracks) {
    warnings.push(
      `${participants.length - maxParticipantTracks} participant(s) dropped — limit is ${maxParticipantTracks} ISO tracks.`
    );
  }

  for (const [i, p] of selected.entries()) {
    tracks.push({
      trackIndex: i,
      source: "participant",
      participantId: p.id,
      label: isoTrackLabel("participant", p.name),
      estimatedBitrateMbps: estimateIsoTrackBitrateMbps("participant", participantResolution),
      resolutionTier: participantResolution,
    });
  }

  // Screen share track
  if (includeScreenShare) {
    tracks.push({
      trackIndex: tracks.length,
      source: "screen-share",
      label: isoTrackLabel("screen-share"),
      estimatedBitrateMbps: estimateIsoTrackBitrateMbps("screen-share"),
      resolutionTier: "1080p",
    });
  }

  // Program mix track (always last)
  if (includeProgramMix) {
    tracks.push({
      trackIndex: tracks.length,
      source: "program-mix",
      label: isoTrackLabel("program-mix"),
      estimatedBitrateMbps: estimateIsoTrackBitrateMbps("program-mix", participantResolution),
      resolutionTier: participantResolution,
    });
  }

  const estimatedTotalMbps = tracks.reduce((sum, t) => sum + t.estimatedBitrateMbps, 0);
  // Mbps → bytes/s → GB/hour: Mbps * 1e6 / 8 / 1e9 * 3600
  const estimatedFileSizeGbPerHour = (estimatedTotalMbps * 1e6 * 3600) / (8 * 1e9);

  if (tracks.length === 0) {
    warnings.push("No tracks planned — select at least one participant.");
  }

  if (estimatedTotalMbps > 80) {
    warnings.push(
      `Combined ISO bitrate ${estimatedTotalMbps.toFixed(0)} Mbps may exceed storage throughput on slower drives.`
    );
  }

  const slug = sessionName.toLowerCase().replace(/\s+/g, "-").replace(/[^a-z0-9-]/g, "");
  const outputFolder = `recordings/${slug}`;

  return {
    tracks,
    estimatedTotalMbps,
    estimatedFileSizeGbPerHour,
    outputFolder,
    warnings,
    valid: tracks.length > 0,
  };
}

/**
 * Validate whether available disk space is sufficient for the planned session.
 * Returns a list of blocking error strings (empty = ok).
 */
export function validateIsoAgainstDisk(
  plan: IsoRecordingPlan,
  availableDiskGb: number,
  sessionDurationMinutes: number
): string[] {
  const errors: string[] = [];
  const requiredGb = (plan.estimatedFileSizeGbPerHour * sessionDurationMinutes) / 60;

  if (requiredGb > availableDiskGb) {
    errors.push(
      `Need ${requiredGb.toFixed(1)} GB for ${sessionDurationMinutes} min at ${plan.estimatedTotalMbps.toFixed(0)} Mbps — only ${availableDiskGb.toFixed(1)} GB available.`
    );
  } else if (requiredGb > availableDiskGb * 0.8) {
    errors.push(
      `Recording will consume ${((requiredGb / availableDiskGb) * 100).toFixed(0)}% of available disk — consider freeing space first.`
    );
  }

  return errors;
}

/**
 * Build a per-track output file path for use in an NLE or archive.
 */
export function isoOutputPath(
  outputFolder: string,
  trackLabel: string,
  trackIndex: number
): string {
  const slug = trackLabel.toLowerCase().replace(/\s+/g, "-").replace(/[^a-z0-9-]/g, "");
  return `${outputFolder}/track-${String(trackIndex + 1).padStart(2, "0")}-${slug}.mov`;
}

export function summarizeIsoPlan(plan: IsoRecordingPlan): string {
  if (!plan.valid) return "No ISO tracks configured";
  return `${plan.tracks.length} ISO tracks — ${plan.estimatedTotalMbps.toFixed(0)} Mbps total, ~${plan.estimatedFileSizeGbPerHour.toFixed(1)} GB/hr`;
}
