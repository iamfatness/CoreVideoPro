/**
 * NDI output engine for local-network program publishing.
 * NDI (Network Device Interface) by NewTek/Vizrt is a zero-latency LAN protocol;
 * no ingest URL or stream key — just a source name that peers discover via mDNS.
 */

export type NdiSourceName = {
  /** Machine name shown to NDI receivers, e.g. "STUDIO-MAC" */
  machineName: string;
  /** Program name appended after the machine name, e.g. "Program" */
  programName: string;
  /** Full canonical name: "MACHINE (Program)" */
  canonical: string;
};

export type NdiValidation = {
  valid: boolean;
  sourceName: NdiSourceName | null;
  errors: string[];
  warnings: string[];
};

export type NdiNetworkStatus = {
  discoverable: boolean;
  /** Estimated LAN bandwidth usage in Mbps for the given output profile */
  estimatedBandwidthMbps: number;
  /** true when bandwidth may saturate a 100 Mbps LAN */
  bandwidthWarning: boolean;
  summary: string;
};

const NDI_MAX_SOURCE_NAME_LENGTH = 64;
const NDI_RESERVED_CHARS = /[<>:"\/\\|?*\x00-\x1f]/;
// NDI uses SpeedHQ codec internally; approximate 1080p60 compressed bandwidth.
const NDI_BANDWIDTH_MBPS_1080P_60 = 125;
const NDI_BANDWIDTH_MBPS_4K_60 = 500;
const LAN_SATURATION_MBPS = 100;

/**
 * Parse and validate an NDI source name string entered by the operator.
 * NDI source names follow the convention "MACHINE NAME (Program Name)".
 * We accept either the full canonical form or just the program name portion,
 * normalising to canonical form on success.
 */
export function parseNdiSourceName(input: string): NdiValidation {
  const errors: string[] = [];
  const warnings: string[] = [];
  const trimmed = input.trim();

  if (!trimmed) {
    return { valid: false, sourceName: null, errors: ["NDI source name is required."], warnings };
  }

  if (trimmed.length > NDI_MAX_SOURCE_NAME_LENGTH) {
    errors.push(`NDI source name must be ${NDI_MAX_SOURCE_NAME_LENGTH} characters or fewer.`);
  }

  if (NDI_RESERVED_CHARS.test(trimmed)) {
    errors.push("NDI source name contains invalid characters.");
  }

  // Detect canonical "MACHINE (Program)" form
  const canonicalMatch = trimmed.match(/^(.+?)\s*\((.+)\)$/);
  let machineName: string;
  let programName: string;

  if (canonicalMatch) {
    machineName = canonicalMatch[1].trim().toUpperCase();
    programName = canonicalMatch[2].trim();
  } else {
    // Treat as program name only; use a generic machine placeholder
    machineName = "COREVIDEO";
    programName = trimmed;
    warnings.push(`NDI source will be published as "COREVIDEO (${trimmed})".`);
  }

  if (!programName) {
    errors.push("NDI program name cannot be empty.");
  }

  if (programName.toLowerCase() === "program" || programName.toLowerCase() === "preview") {
    // Fine — these are the conventional names
  } else if (programName.length < 2) {
    warnings.push("Short NDI program names may conflict with other sources on the network.");
  }

  if (errors.length > 0) {
    return { valid: false, sourceName: null, errors, warnings };
  }

  const canonical = `${machineName} (${programName})`;
  return {
    valid: true,
    sourceName: { machineName, programName, canonical },
    errors,
    warnings
  };
}

/**
 * Estimate NDI LAN bandwidth and check for saturation risk.
 * NDI uses SpeedHQ intraframe compression: ~125 Mbps for 1080p60, ~500 Mbps for 4K60.
 */
export function estimateNdiBandwidth(resolution: string, fps: number): NdiNetworkStatus {
  const is4K = resolution.startsWith("3840") || resolution.startsWith("4096") || resolution.toLowerCase().includes("4k");
  const isHighFps = fps >= 50;

  let estimatedBandwidthMbps: number;
  if (is4K) {
    estimatedBandwidthMbps = isHighFps ? NDI_BANDWIDTH_MBPS_4K_60 : Math.round(NDI_BANDWIDTH_MBPS_4K_60 * 0.6);
  } else {
    estimatedBandwidthMbps = isHighFps ? NDI_BANDWIDTH_MBPS_1080P_60 : Math.round(NDI_BANDWIDTH_MBPS_1080P_60 * 0.6);
  }

  const bandwidthWarning = estimatedBandwidthMbps > LAN_SATURATION_MBPS;

  let summary: string;
  if (bandwidthWarning) {
    summary = `~${estimatedBandwidthMbps} Mbps — requires Gigabit LAN`;
  } else {
    summary = `~${estimatedBandwidthMbps} Mbps — fits standard LAN`;
  }

  return {
    discoverable: true,
    estimatedBandwidthMbps,
    bandwidthWarning,
    summary
  };
}

export function describeNdiSource(sourceName: NdiSourceName): string {
  return `Publishing as "${sourceName.canonical}" on the local network`;
}

/**
 * List the NDI receiver applications and hardware commonly used with CoreVideo Pro.
 */
export function ndiReceiverSuggestions(): string[] {
  return [
    "NDI Monitor (NewTek / Vizrt)",
    "desktop production software with direct NDI input",
    "NDI-compatible recording or switching tools",
    "Blackmagic Web Presenter HD",
    "any NDI-capable hardware switcher"
  ];
}
