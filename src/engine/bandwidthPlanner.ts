/**
 * Bandwidth planner engine.
 * Computes total upload bandwidth across all active streaming destinations,
 * accounts for audio, protocol overhead, and SRT/WebRTC redundancy, and
 * warns when the combined bitrate approaches or exceeds typical uplink limits.
 */

export type DestinationBandwidth = {
  destinationId: string;
  name: string;
  protocol: string;
  videoBitrateKbps: number;
  audioBitrateKbps: number;
  overheadKbps: number;
  totalKbps: number;
};

export type BandwidthPlan = {
  destinations: DestinationBandwidth[];
  totalKbps: number;
  totalMbps: number;
  /** Safety headroom recommendation: keep usage below this fraction */
  recommendedMaxFraction: number;
  /** Upload link capacity entered by operator or assumed default */
  uplinkCapacityKbps: number;
  usageFraction: number;
  status: "safe" | "caution" | "overloaded";
  warnings: string[];
  summary: string;
};

export type BandwidthPlanOptions = {
  /** Operator's measured or estimated upload bandwidth in Mbps */
  uplinkCapacityMbps?: number;
};

/** Audio bitrate defaults by protocol (AAC stereo) */
const AUDIO_BITRATE_KBPS = 128;

/** Protocol overhead as a fraction of video+audio bitrate */
const PROTOCOL_OVERHEAD: Record<string, number> = {
  RTMP: 0.02,
  SRT: 0.08,  // SRT ARQ and FEC packets add ~5–10%
  WebRTC: 0.12, // RTP/SRTP + RTCP overhead
  NDI: 0.05,
};

const DEFAULT_UPLINK_MBPS = 20; // Conservative home-office assumption
const CAUTION_FRACTION = 0.7;
const OVERLOAD_FRACTION = 0.9;

export function computeDestinationBandwidth(
  destinationId: string,
  name: string,
  protocol: string,
  videoBitrateKbps: number
): DestinationBandwidth {
  const audioBitrateKbps = AUDIO_BITRATE_KBPS;
  const overheadFraction = PROTOCOL_OVERHEAD[protocol] ?? 0.03;
  const overheadKbps = Math.round((videoBitrateKbps + audioBitrateKbps) * overheadFraction);
  const totalKbps = videoBitrateKbps + audioBitrateKbps + overheadKbps;
  return { destinationId, name, protocol, videoBitrateKbps, audioBitrateKbps, overheadKbps, totalKbps };
}

export function planBandwidth(
  destinations: Array<{ id: string; name: string; protocol: string; bitrateKbps: number }>,
  options: BandwidthPlanOptions = {}
): BandwidthPlan {
  const uplinkCapacityKbps = (options.uplinkCapacityMbps ?? DEFAULT_UPLINK_MBPS) * 1000;
  const warnings: string[] = [];

  const computed = destinations.map((d) =>
    computeDestinationBandwidth(d.id, d.name, d.protocol, d.bitrateKbps)
  );

  const totalKbps = computed.reduce((sum, d) => sum + d.totalKbps, 0);
  const totalMbps = totalKbps / 1000;
  const usageFraction = uplinkCapacityKbps > 0 ? totalKbps / uplinkCapacityKbps : 1;

  let status: BandwidthPlan["status"];
  if (usageFraction >= OVERLOAD_FRACTION) {
    status = "overloaded";
    warnings.push(
      `Combined bitrate ${totalMbps.toFixed(1)} Mbps exceeds ${Math.round(OVERLOAD_FRACTION * 100)}% of uplink (${(uplinkCapacityKbps / 1000).toFixed(0)} Mbps) — stream quality will suffer.`
    );
  } else if (usageFraction >= CAUTION_FRACTION) {
    status = "caution";
    warnings.push(
      `Combined bitrate ${totalMbps.toFixed(1)} Mbps uses ${Math.round(usageFraction * 100)}% of uplink — leave headroom for packet retransmits.`
    );
  } else {
    status = "safe";
  }

  // Warn if any single destination is very high bitrate
  for (const dest of computed) {
    if (dest.videoBitrateKbps > 8000) {
      warnings.push(`${dest.name}: ${(dest.videoBitrateKbps / 1000).toFixed(1)} Mbps video is high — consider 6 Mbps max for reliability.`);
    }
  }

  if (destinations.length === 0) {
    warnings.push("No active destinations — nothing to stream.");
  }

  const statusLabel = status === "safe" ? "OK" : status === "caution" ? "Caution" : "Overloaded";
  const summary = destinations.length === 0
    ? "No destinations active"
    : `${statusLabel} — ${totalMbps.toFixed(1)} Mbps across ${destinations.length} destination${destinations.length !== 1 ? "s" : ""}`;

  return {
    destinations: computed,
    totalKbps,
    totalMbps,
    recommendedMaxFraction: CAUTION_FRACTION,
    uplinkCapacityKbps,
    usageFraction,
    status,
    warnings,
    summary,
  };
}

export function formatKbps(kbps: number): string {
  if (kbps >= 1000) return `${(kbps / 1000).toFixed(1)} Mbps`;
  return `${kbps} kbps`;
}

/**
 * Suggest a per-destination bitrate reduction to bring total under the
 * safe fraction of uplink capacity. Returns null if already within budget.
 */
export function suggestBitrateReduction(
  plan: BandwidthPlan
): { targetKbpsPerDestination: number; reductionPct: number } | null {
  if (plan.status === "safe") return null;
  const targetTotalKbps = plan.uplinkCapacityKbps * plan.recommendedMaxFraction;
  const n = plan.destinations.length;
  if (n === 0) return null;
  const targetPerDest = Math.floor(targetTotalKbps / n);
  const avgCurrentVideo =
    plan.destinations.reduce((s, d) => s + d.videoBitrateKbps, 0) / n;
  const reductionPct = Math.round((1 - targetPerDest / (avgCurrentVideo * 1.15)) * 100);
  return { targetKbpsPerDestination: Math.max(1000, targetPerDest), reductionPct: Math.max(0, reductionPct) };
}
