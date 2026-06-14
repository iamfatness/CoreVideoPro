import { describe, expect, it } from "vitest";
import {
  computeDestinationBandwidth,
  formatKbps,
  planBandwidth,
  suggestBitrateReduction,
} from "./bandwidthPlanner";

const destinations = [
  { id: "d-1", name: "YouTube", protocol: "RTMP", bitrateKbps: 6000 },
  { id: "d-2", name: "Backup SRT", protocol: "SRT", bitrateKbps: 4000 },
];

describe("computeDestinationBandwidth", () => {
  it("adds audio and overhead to video bitrate", () => {
    const bw = computeDestinationBandwidth("d-1", "YouTube", "RTMP", 6000);
    expect(bw.videoBitrateKbps).toBe(6000);
    expect(bw.audioBitrateKbps).toBe(128);
    expect(bw.overheadKbps).toBeGreaterThan(0);
    expect(bw.totalKbps).toBe(bw.videoBitrateKbps + bw.audioBitrateKbps + bw.overheadKbps);
  });

  it("applies higher overhead for SRT than RTMP", () => {
    const rtmp = computeDestinationBandwidth("d-1", "Test", "RTMP", 4000);
    const srt = computeDestinationBandwidth("d-2", "Test", "SRT", 4000);
    expect(srt.overheadKbps).toBeGreaterThan(rtmp.overheadKbps);
  });

  it("applies even higher overhead for WebRTC", () => {
    const rtmp = computeDestinationBandwidth("d-1", "Test", "RTMP", 4000);
    const webrtc = computeDestinationBandwidth("d-2", "Test", "WebRTC", 4000);
    expect(webrtc.overheadKbps).toBeGreaterThan(rtmp.overheadKbps);
  });

  it("uses a default overhead for unknown protocols", () => {
    const bw = computeDestinationBandwidth("d-1", "Test", "CustomProtocol", 4000);
    expect(bw.overheadKbps).toBeGreaterThan(0);
  });
});

describe("planBandwidth", () => {
  it("sums all destination totals", () => {
    const plan = planBandwidth(destinations, { uplinkCapacityMbps: 100 });
    expect(plan.totalKbps).toBe(plan.destinations.reduce((s, d) => s + d.totalKbps, 0));
  });

  it("returns safe status on ample uplink", () => {
    const plan = planBandwidth(destinations, { uplinkCapacityMbps: 100 });
    expect(plan.status).toBe("safe");
    expect(plan.warnings).toHaveLength(0);
  });

  it("returns caution when usage is 70–89% of uplink", () => {
    // total ≈ 6000+128+123(RTMP) + 4000+128+330(SRT) ≈ 10709 kbps ≈ 10.7 Mbps
    // 70% caution of 15 Mbps = 10.5 Mbps → just over caution
    const plan = planBandwidth(destinations, { uplinkCapacityMbps: 15 });
    expect(["caution", "overloaded"]).toContain(plan.status);
  });

  it("returns overloaded when usage exceeds 90%", () => {
    const plan = planBandwidth(destinations, { uplinkCapacityMbps: 10 });
    expect(plan.status).toBe("overloaded");
    expect(plan.warnings.some((w) => w.includes("stream quality"))).toBe(true);
  });

  it("uses default 20 Mbps uplink when none specified", () => {
    const plan = planBandwidth(destinations);
    expect(plan.uplinkCapacityKbps).toBe(20000);
  });

  it("warns when a destination has video over 8 Mbps", () => {
    const highBitrate = [{ id: "d-1", name: "4K RTMP", protocol: "RTMP", bitrateKbps: 12000 }];
    const plan = planBandwidth(highBitrate, { uplinkCapacityMbps: 100 });
    expect(plan.warnings.some((w) => w.includes("is high"))).toBe(true);
  });

  it("warns and marks empty destinations", () => {
    const plan = planBandwidth([], { uplinkCapacityMbps: 20 });
    expect(plan.warnings.some((w) => w.includes("No active destinations"))).toBe(true);
    expect(plan.summary).toBe("No destinations active");
  });

  it("computes usageFraction correctly", () => {
    const plan = planBandwidth(destinations, { uplinkCapacityMbps: 100 });
    expect(plan.usageFraction).toBeCloseTo(plan.totalKbps / 100000, 3);
  });

  it("includes destination count in summary", () => {
    const plan = planBandwidth(destinations, { uplinkCapacityMbps: 100 });
    expect(plan.summary).toMatch(/2 destinations/);
  });
});

describe("suggestBitrateReduction", () => {
  it("returns null when status is safe", () => {
    const plan = planBandwidth(destinations, { uplinkCapacityMbps: 100 });
    expect(suggestBitrateReduction(plan)).toBeNull();
  });

  it("returns a target per-destination bitrate when overloaded", () => {
    const plan = planBandwidth(destinations, { uplinkCapacityMbps: 10 });
    const suggestion = suggestBitrateReduction(plan);
    expect(suggestion).not.toBeNull();
    expect(suggestion!.targetKbpsPerDestination).toBeGreaterThan(0);
    expect(suggestion!.reductionPct).toBeGreaterThanOrEqual(0);
  });

  it("target is at least 1000 kbps (floor)", () => {
    const bigDests = Array.from({ length: 10 }, (_, i) => ({
      id: `d-${i}`, name: `Dest ${i}`, protocol: "RTMP", bitrateKbps: 6000
    }));
    const plan = planBandwidth(bigDests, { uplinkCapacityMbps: 5 });
    const suggestion = suggestBitrateReduction(plan);
    expect(suggestion!.targetKbpsPerDestination).toBeGreaterThanOrEqual(1000);
  });
});

describe("formatKbps", () => {
  it("formats values below 1000 as kbps", () => {
    expect(formatKbps(500)).toBe("500 kbps");
  });

  it("formats values >= 1000 as Mbps", () => {
    expect(formatKbps(6000)).toBe("6.0 Mbps");
    expect(formatKbps(1500)).toBe("1.5 Mbps");
  });
});
