import { describe, expect, it } from "vitest";
import {
  addMarker,
  formatTimecode,
  nearestMarker,
  parseTimecode,
  removeMarker,
  rippleTrim,
  snapToFrame,
  summarizeTrim,
  trimClip,
} from "./clipTrimmer";

const BASE_REGION = { inPointSec: 10, outPointSec: 40, fps: 30 };

describe("trimClip", () => {
  it("returns valid result for a clean region", () => {
    const result = trimClip(BASE_REGION, 60);
    expect(result.valid).toBe(true);
    expect(result.durationSec).toBe(30);
    expect(result.warnings).toHaveLength(0);
  });

  it("clamps in point to 0 when negative", () => {
    const result = trimClip({ ...BASE_REGION, inPointSec: -5 }, 60);
    expect(result.region.inPointSec).toBe(0);
    expect(result.warnings.some((w) => w.includes("In point clamped"))).toBe(true);
  });

  it("clamps out point to source duration when too long", () => {
    const result = trimClip({ ...BASE_REGION, outPointSec: 100 }, 60);
    expect(result.region.outPointSec).toBe(60);
    expect(result.warnings.some((w) => w.includes("Out point clamped"))).toBe(true);
  });

  it("fixes inverted in/out points", () => {
    const result = trimClip({ ...BASE_REGION, inPointSec: 50, outPointSec: 20 }, 60);
    expect(result.region.outPointSec).toBeGreaterThan(result.region.inPointSec);
    expect(result.warnings.some((w) => w.includes("In point must be before"))).toBe(true);
  });

  it("returns valid=false when warnings exist", () => {
    const result = trimClip({ ...BASE_REGION, inPointSec: -1 }, 60);
    expect(result.valid).toBe(false);
  });

  it("computes durationSec correctly", () => {
    const result = trimClip({ inPointSec: 5, outPointSec: 25, fps: 30 }, 60);
    expect(result.durationSec).toBe(20);
  });
});

describe("rippleTrim", () => {
  it("moves the in point forward", () => {
    const result = rippleTrim(BASE_REGION, { edge: "in", deltaSec: 5, sourceDurationSec: 60 });
    expect(result.region.inPointSec).toBe(15);
    expect(result.region.outPointSec).toBe(40); // unchanged
  });

  it("moves the out point earlier", () => {
    const result = rippleTrim(BASE_REGION, { edge: "out", deltaSec: -10, sourceDurationSec: 60 });
    expect(result.region.outPointSec).toBe(30);
  });

  it("clamps in point at 0 when delta is too negative", () => {
    const result = rippleTrim(BASE_REGION, { edge: "in", deltaSec: -20, sourceDurationSec: 60 });
    expect(result.region.inPointSec).toBe(0);
  });

  it("clamps out point at source duration when delta is too large", () => {
    const result = rippleTrim(BASE_REGION, { edge: "out", deltaSec: 50, sourceDurationSec: 60 });
    expect(result.region.outPointSec).toBe(60);
  });
});

describe("snapToFrame", () => {
  it("snaps to nearest frame at 30fps", () => {
    // 1/30 = 0.03333... per frame; position 0.01 (0.01*30=0.3) snaps to frame 0
    expect(snapToFrame(0.01, 30)).toBeCloseTo(0, 4);
  });

  it("snaps up to next frame when closer", () => {
    // 0.045 is closer to frame 1 (0.0333) than frame 2 (0.0667)
    expect(snapToFrame(0.045, 30)).toBeCloseTo(1 / 30, 4);
  });

  it("returns position unchanged for fps <= 0", () => {
    expect(snapToFrame(1.5, 0)).toBe(1.5);
  });

  it("snaps correctly at 25fps", () => {
    // frame 2 at 25fps = 0.08s; 0.079 should snap to frame 2
    expect(snapToFrame(0.079, 25)).toBeCloseTo(2 / 25, 4);
  });
});

describe("formatTimecode", () => {
  it("formats HH:MM:SS:FF at 30fps", () => {
    // 65.5s = 1 min 5 sec 15 frames (0.5 * 30 = 15)
    expect(formatTimecode(65.5, 30)).toBe("00:01:05:15");
  });

  it("formats HH:MM:SS.mmm", () => {
    expect(formatTimecode(65.5, 30, "HH:MM:SS.mmm")).toBe("00:01:05.500");
  });

  it("formats seconds", () => {
    expect(formatTimecode(10.25, 30, "seconds")).toBe("10.250s");
  });

  it("formats hours correctly", () => {
    expect(formatTimecode(3661, 30)).toBe("01:01:01:00");
  });

  it("clamps negative values to 0", () => {
    expect(formatTimecode(-5, 30)).toBe("00:00:00:00");
  });
});

describe("parseTimecode", () => {
  it("parses HH:MM:SS:FF", () => {
    // 00:01:05:15 at 30fps = 65 + 15/30 = 65.5s
    expect(parseTimecode("00:01:05:15", 30)).toBeCloseTo(65.5, 5);
  });

  it("parses HH:MM:SS.mmm", () => {
    expect(parseTimecode("00:01:05.500", 30)).toBeCloseTo(65.5, 5);
  });

  it("parses plain seconds", () => {
    expect(parseTimecode("10.25", 30)).toBeCloseTo(10.25, 5);
  });

  it("parses seconds with 's' suffix", () => {
    expect(parseTimecode("10.25s", 30)).toBeCloseTo(10.25, 5);
  });

  it("returns 0 for unrecognised format", () => {
    expect(parseTimecode("gibberish", 30)).toBe(0);
  });

  it("round-trips formatTimecode → parseTimecode", () => {
    const sec = 125.4;
    const tc = formatTimecode(sec, 30);
    expect(parseTimecode(tc, 30)).toBeCloseTo(sec, 1);
  });
});

describe("addMarker / removeMarker", () => {
  it("adds a marker and keeps list sorted by position", () => {
    let markers = addMarker([], 30, "Key moment");
    markers = addMarker(markers, 10, "Early cue");
    expect(markers[0].positionSec).toBe(10);
    expect(markers[1].positionSec).toBe(30);
  });

  it("marker has the provided label and color", () => {
    const markers = addMarker([], 5, "Chapter", "#ff0000");
    expect(markers[0].label).toBe("Chapter");
    expect(markers[0].color).toBe("#ff0000");
  });

  it("uses default color when none provided", () => {
    const markers = addMarker([], 5, "No color");
    expect(markers[0].color).toBeTruthy();
  });

  it("removes marker by id", () => {
    const markers = addMarker([], 5, "To remove");
    const id = markers[0].id;
    const after = removeMarker(markers, id);
    expect(after).toHaveLength(0);
  });

  it("ignores unknown id in removeMarker", () => {
    const markers = addMarker([], 5, "Keep me");
    const after = removeMarker(markers, "non-existent");
    expect(after).toHaveLength(1);
  });
});

describe("nearestMarker", () => {
  it("returns closest marker within tolerance", () => {
    let markers = addMarker([], 10, "A");
    markers = addMarker(markers, 20, "B");
    const result = nearestMarker(markers, 10.3, 0.5);
    expect(result?.label).toBe("A");
  });

  it("returns null when no marker within tolerance", () => {
    const markers = addMarker([], 10, "Far away");
    expect(nearestMarker(markers, 15, 0.5)).toBeNull();
  });

  it("returns null for empty marker list", () => {
    expect(nearestMarker([], 5, 1)).toBeNull();
  });
});

describe("summarizeTrim", () => {
  it("returns formatted summary for valid trim", () => {
    const result = trimClip(BASE_REGION, 60);
    const summary = summarizeTrim(result, 30);
    expect(summary).toContain("In:");
    expect(summary).toContain("Out:");
  });

  it("returns warning summary for invalid trim", () => {
    const result = trimClip({ inPointSec: -5, outPointSec: 10, fps: 30 }, 60);
    const summary = summarizeTrim(result, 30);
    expect(summary).toContain("Invalid clip");
  });
});
