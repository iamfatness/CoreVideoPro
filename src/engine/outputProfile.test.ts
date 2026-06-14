import { describe, expect, it } from "vitest";
import type { OutputDestination, OutputProfile } from "../domain/production";
import { computeOutputProfileReadout, isUltraHdProfile } from "./outputProfile";

function profile(overrides: Partial<OutputProfile>): OutputProfile {
  return {
    id: "1080p60",
    name: "1080p 60",
    resolution: "1920x1080",
    fps: 60,
    targetBitrateMbps: 8.2,
    ...overrides
  };
}

function destination(overrides: Partial<OutputDestination>): OutputDestination {
  return {
    id: "dest",
    name: "Destination",
    protocol: "RTMP",
    enabled: true,
    latencyMs: 2100,
    endpoint: "rtmp://live.example.com/app",
    streamKey: "key",
    ...overrides
  };
}

const profiles = {
  fullHd60: profile({}),
  fullHd30: profile({ id: "1080p30", name: "1080p 30", fps: 30, targetBitrateMbps: 6 }),
  uhd30: profile({ id: "4k30", name: "4K 30", resolution: "3840x2160", fps: 30, targetBitrateMbps: 18 }),
  uhd60: profile({ id: "4k60", name: "4K 60", resolution: "3840x2160", fps: 60, targetBitrateMbps: 28 })
};

describe("isUltraHdProfile", () => {
  it("flags 4K profiles and rejects 1080p", () => {
    expect(isUltraHdProfile(profiles.uhd30)).toBe(true);
    expect(isUltraHdProfile(profiles.uhd60)).toBe(true);
    expect(isUltraHdProfile(profiles.fullHd60)).toBe(false);
  });
});

describe("computeOutputProfileReadout", () => {
  it("scores 1080p60 at full headroom and the reference load factor", () => {
    const readout = computeOutputProfileReadout(profiles.fullHd60, []);

    expect(readout.encoderLoadFactor).toBe(1);
    expect(readout.encoderHeadroomPercent).toBe(75);
    expect(readout.isUltraHd).toBe(false);
    expect(readout.warnings).toHaveLength(0);
  });

  it("drains encoder headroom to zero for 4K60 and warns", () => {
    const readout = computeOutputProfileReadout(profiles.uhd60, []);

    expect(readout.encoderLoadFactor).toBe(4);
    expect(readout.encoderHeadroomPercent).toBe(0);
    expect(readout.isUltraHd).toBe(true);
    expect(readout.warnings[0]).toMatch(/only 0% encoder headroom/);
  });

  it("sums upload bandwidth across armed network destinations only", () => {
    const readout = computeOutputProfileReadout(profiles.fullHd60, [
      destination({ id: "a" }),
      destination({ id: "b", name: "YouTube" }),
      destination({ id: "ndi", name: "NDI Program", protocol: "NDI", endpoint: "CoreVideo Pro" }),
      destination({ id: "off", name: "Disabled", enabled: false })
    ]);

    expect(readout.networkDestinationCount).toBe(2);
    expect(readout.uploadBandwidthMbps).toBe(16.4);
  });

  it("warns when a 4K profile fans out to multiple network destinations", () => {
    const readout = computeOutputProfileReadout(profiles.uhd30, [
      destination({ id: "a" }),
      destination({ id: "b", name: "YouTube" })
    ]);

    expect(readout.uploadBandwidthMbps).toBe(36);
    expect(readout.warnings.some((warning) => /36 Mbps upload/.test(warning))).toBe(true);
  });

  it("summarizes headroom and upload in one line", () => {
    const readout = computeOutputProfileReadout(profiles.fullHd30, [destination({})]);

    expect(readout.summary).toBe("1080p 30 - 88% encoder headroom, 6 Mbps upload");
  });
});
