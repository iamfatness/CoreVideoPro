import type { CaptureDeviceState } from "../domain/production";

export type CaptureFleetSummary = {
  connectedCount: number;
  liveSourceCount: number;
  dualCapture: boolean;
  estimatedBandwidthMbps: number;
  warnings: string[];
  summary: string;
};

// A 1080p60 capture proxy is the reference point for ~10 Mbps.
const REFERENCE_PIXELS_PER_SECOND = 1920 * 1080 * 60;
const REFERENCE_MBPS = 10;

export function estimateDeviceBandwidthMbps(device: CaptureDeviceState): number {
  const pixelsPerSecond = device.resolution.width * device.resolution.height * device.frameRate;
  const mbps = (pixelsPerSecond / REFERENCE_PIXELS_PER_SECOND) * REFERENCE_MBPS;
  return Math.round(mbps * 10) / 10;
}

/**
 * Summarize the local capture fleet so the operator can run two devices at
 * once with confidence: how many are connected, how many are live program
 * sources, the combined bandwidth, and any signal/format/sync warnings.
 */
export function summarizeCaptureFleet(devices: CaptureDeviceState[]): CaptureFleetSummary {
  const connected = devices.filter((device) => device.connectionState === "connected");
  const liveSources = connected.filter((device) => device.signalPresent);
  const dualCapture = liveSources.length >= 2;

  const estimatedBandwidthMbps =
    Math.round(connected.reduce((total, device) => total + estimateDeviceBandwidthMbps(device), 0) * 10) / 10;

  const warnings: string[] = [];

  connected
    .filter((device) => !device.signalPresent)
    .forEach((device) => warnings.push(`${device.name} is connected but has no signal.`));

  devices
    .filter((device) => device.connectionState === "format-mismatch")
    .forEach((device) => warnings.push(`${device.name} has a format mismatch; set the input format manually.`));

  if (dualCapture) {
    warnings.push("Two capture sources are live; confirm each device's A/V sync offset against Zoom audio.");
  }

  return {
    connectedCount: connected.length,
    liveSourceCount: liveSources.length,
    dualCapture,
    estimatedBandwidthMbps,
    warnings,
    summary: buildSummary(connected.length, liveSources.length, estimatedBandwidthMbps)
  };
}

function buildSummary(connectedCount: number, liveSourceCount: number, bandwidthMbps: number): string {
  if (connectedCount === 0) {
    return "No capture devices online";
  }

  return `${liveSourceCount} live / ${connectedCount} connected - ${bandwidthMbps} Mbps`;
}
