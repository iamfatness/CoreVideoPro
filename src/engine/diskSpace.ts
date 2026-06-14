export type DiskSpaceLevel = "ok" | "low" | "critical" | "full";

export type DiskSpaceStatus = {
  level: DiskSpaceLevel;
  availableGb: number;
  /** Estimated recording time remaining in seconds */
  remainingSeconds: number;
  /** Human-readable remaining time */
  remainingLabel: string;
  warnings: string[];
};

export type DiskSpaceThresholds = {
  /** Gb below which status becomes "low" */
  lowGb: number;
  /** Gb below which status becomes "critical" */
  criticalGb: number;
};

const DEFAULT_THRESHOLDS: DiskSpaceThresholds = {
  lowGb: 20,
  criticalGb: 5,
};

/**
 * Estimate disk write rate in MB/s from the recording quality and ISO track count.
 * These values match the multitrackAudio disk rate estimates.
 */
export function estimateDiskRateMbps(
  qualityBitrateMbps: number,
  isoTrackCount: number
): number {
  const isoRateMbps = isoTrackCount * 0.192; // 192 kbps per ISO mono track
  return qualityBitrateMbps + isoRateMbps;
}

/**
 * Estimate remaining recording time in seconds given available disk and write rate.
 */
export function estimateRemainingSeconds(availableGb: number, diskRateMbps: number): number {
  if (diskRateMbps <= 0) return Infinity;
  const availableMb = availableGb * 1024;
  return Math.floor(availableMb / diskRateMbps);
}

export function formatRemainingTime(seconds: number): string {
  if (!isFinite(seconds) || seconds > 86400 * 7) return "7+ days";
  if (seconds >= 3600) {
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    return `${h}h ${m}m`;
  }
  if (seconds >= 60) {
    const m = Math.floor(seconds / 60);
    return `${m} min`;
  }
  return `${seconds} sec`;
}

/**
 * Evaluate disk space status for the current recording configuration.
 * Returns warnings the operator should see before or during recording.
 */
export function evaluateDiskSpace(
  availableGb: number,
  diskRateMbps: number,
  thresholds: DiskSpaceThresholds = DEFAULT_THRESHOLDS
): DiskSpaceStatus {
  const warnings: string[] = [];
  const remainingSeconds = estimateRemainingSeconds(availableGb, diskRateMbps);
  const remainingLabel = formatRemainingTime(remainingSeconds);

  let level: DiskSpaceLevel;
  if (availableGb <= 0) {
    level = "full";
    warnings.push("Disk is full — recording cannot start or will be interrupted.");
  } else if (availableGb < thresholds.criticalGb) {
    level = "critical";
    warnings.push(`Only ${availableGb.toFixed(1)} GB remaining — recording will stop in ~${remainingLabel}.`);
  } else if (availableGb < thresholds.lowGb) {
    level = "low";
    warnings.push(`Disk space is low (${availableGb.toFixed(1)} GB) — ~${remainingLabel} of recording left.`);
  } else {
    level = "ok";
  }

  if (remainingSeconds < 1800 && level === "ok") {
    level = "low";
    warnings.push(`Less than 30 minutes of recording space remains.`);
  }

  return { level, availableGb, remainingSeconds, remainingLabel, warnings };
}

export function diskSpaceSummary(status: DiskSpaceStatus): string {
  if (status.level === "full") return "Disk full";
  if (status.level === "critical") return `${status.availableGb.toFixed(1)} GB — critical`;
  if (status.level === "low") return `${status.availableGb.toFixed(1)} GB — low`;
  return `${status.availableGb.toFixed(1)} GB free (~${status.remainingLabel})`;
}
