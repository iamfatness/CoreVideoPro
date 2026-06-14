import { describe, expect, it } from "vitest";
import {
  diskSpaceSummary,
  estimateDiskRateMbps,
  estimateRemainingSeconds,
  evaluateDiskSpace,
  formatRemainingTime,
} from "./diskSpace";

describe("estimateDiskRateMbps", () => {
  it("returns just the quality bitrate with no ISO tracks", () => {
    expect(estimateDiskRateMbps(8, 0)).toBe(8);
  });

  it("adds ~192 kbps per ISO track", () => {
    const rate = estimateDiskRateMbps(8, 2);
    expect(rate).toBeCloseTo(8.384, 2);
  });
});

describe("estimateRemainingSeconds", () => {
  it("returns Infinity when disk rate is 0", () => {
    expect(estimateRemainingSeconds(100, 0)).toBe(Infinity);
  });

  it("calculates seconds from GB and MB/s", () => {
    // 1 GB = 1024 MB; at 1 MB/s → 1024 seconds
    expect(estimateRemainingSeconds(1, 1)).toBe(1024);
  });

  it("scales linearly with available space", () => {
    const base = estimateRemainingSeconds(10, 8);
    const doubled = estimateRemainingSeconds(20, 8);
    expect(doubled).toBe(base * 2);
  });
});

describe("formatRemainingTime", () => {
  it("formats seconds under 60 as seconds", () => {
    expect(formatRemainingTime(45)).toBe("45 sec");
  });

  it("formats minutes", () => {
    expect(formatRemainingTime(125)).toBe("2 min");
  });

  it("formats hours and minutes", () => {
    expect(formatRemainingTime(3660)).toBe("1h 1m");
    expect(formatRemainingTime(7200)).toBe("2h 0m");
  });

  it("caps at 7+ days for very large values", () => {
    expect(formatRemainingTime(Infinity)).toBe("7+ days");
    expect(formatRemainingTime(86400 * 10)).toBe("7+ days");
  });
});

describe("evaluateDiskSpace", () => {
  it("returns ok for ample space", () => {
    const status = evaluateDiskSpace(100, 8);
    expect(status.level).toBe("ok");
    expect(status.warnings).toHaveLength(0);
    expect(status.availableGb).toBe(100);
  });

  it("returns low when below lowGb threshold", () => {
    const status = evaluateDiskSpace(15, 8);
    expect(status.level).toBe("low");
    expect(status.warnings.some((w) => w.includes("low"))).toBe(true);
  });

  it("returns critical when below criticalGb threshold", () => {
    const status = evaluateDiskSpace(3, 8);
    expect(status.level).toBe("critical");
    expect(status.warnings.some((w) => w.includes("critical") || w.includes("Only"))).toBe(true);
  });

  it("returns full when no space available", () => {
    const status = evaluateDiskSpace(0, 8);
    expect(status.level).toBe("full");
    expect(status.warnings.some((w) => w.includes("full"))).toBe(true);
  });

  it("returns low when remaining time is under 30 minutes even with ample space", () => {
    // 30 GB / 8 MB/s × 1024 MB/GB = 3840 seconds = 64 min → ok
    // Very high rate: 30 GB / 60 MB/s = 512 s = ~8 min → low
    const status = evaluateDiskSpace(30, 60);
    expect(status.level).toBe("low");
    expect(status.warnings.some((w) => w.includes("30 minutes"))).toBe(true);
  });

  it("respects custom thresholds", () => {
    const status = evaluateDiskSpace(50, 8, { lowGb: 60, criticalGb: 40 });
    expect(status.level).toBe("low");
  });

  it("includes remaining label in status", () => {
    const status = evaluateDiskSpace(100, 8);
    expect(status.remainingLabel).toMatch(/h|min|days/);
  });
});

describe("diskSpaceSummary", () => {
  it("formats ok status with free space and time", () => {
    const status = evaluateDiskSpace(100, 8);
    const summary = diskSpaceSummary(status);
    expect(summary).toContain("100.0 GB");
    expect(summary).toContain("free");
  });

  it("formats low status", () => {
    const status = evaluateDiskSpace(15, 8);
    const summary = diskSpaceSummary(status);
    expect(summary).toContain("low");
  });

  it("formats full status", () => {
    const status = evaluateDiskSpace(0, 8);
    expect(diskSpaceSummary(status)).toBe("Disk full");
  });
});
