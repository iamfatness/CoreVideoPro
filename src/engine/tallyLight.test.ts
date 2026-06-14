import { describe, expect, it } from "vitest";
import {
  computeTally,
  exportTallyReport,
  filterByTally,
  isOnPreview,
  isOnProgram,
  mergeTallySnapshots,
  TALLY_COLORS,
  tallyColor,
  tallyColorHex,
} from "./tallyLight";

const sources = [
  { sourceId: "p1", label: "Alice" },
  { sourceId: "p2", label: "Bob" },
  { sourceId: "p3", label: "Carol" },
  { sourceId: "ss1", label: "Screen Share" },
];

describe("computeTally", () => {
  it("marks program sources correctly", () => {
    const snap = computeTally(sources, ["p1"], ["p2"]);
    expect(snap.sources.find((s) => s.sourceId === "p1")?.tally).toBe("program");
  });

  it("marks preview sources correctly", () => {
    const snap = computeTally(sources, ["p1"], ["p2"]);
    expect(snap.sources.find((s) => s.sourceId === "p2")?.tally).toBe("preview");
  });

  it("marks non-assigned sources as idle", () => {
    const snap = computeTally(sources, ["p1"], ["p2"]);
    expect(snap.sources.find((s) => s.sourceId === "p3")?.tally).toBe("idle");
    expect(snap.sources.find((s) => s.sourceId === "ss1")?.tally).toBe("idle");
  });

  it("program takes precedence over preview for same source", () => {
    const snap = computeTally(sources, ["p1"], ["p1", "p2"]);
    expect(snap.sources.find((s) => s.sourceId === "p1")?.tally).toBe("program");
  });

  it("counts on-air sources correctly", () => {
    const snap = computeTally(sources, ["p1", "p2"], ["p3"]);
    expect(snap.onAirCount).toBe(2);
  });

  it("returns zero on-air count when no program sources", () => {
    const snap = computeTally(sources, [], ["p1"]);
    expect(snap.onAirCount).toBe(0);
  });

  it("handles empty source list", () => {
    const snap = computeTally([], [], []);
    expect(snap.sources).toHaveLength(0);
    expect(snap.summary).toBe("No sources");
  });

  it("summary lists on-air source labels", () => {
    const snap = computeTally(sources, ["p1", "p2"], []);
    expect(snap.summary).toContain("Alice");
    expect(snap.summary).toContain("Bob");
  });

  it("summary says 'All sources idle' when nothing on air", () => {
    const snap = computeTally(sources, [], []);
    expect(snap.summary).toBe("All sources idle");
  });
});

describe("tallyColor", () => {
  it("returns red for program", () => {
    const color = tallyColor("program");
    expect(color).toEqual(TALLY_COLORS.program);
    expect(color.r).toBe(255);
    expect(color.g).toBe(0);
  });

  it("returns green for preview", () => {
    const color = tallyColor("preview");
    expect(color.g).toBe(255);
    expect(color.r).toBe(0);
  });

  it("returns black for idle", () => {
    const color = tallyColor("idle");
    expect(color.r).toBe(0);
    expect(color.g).toBe(0);
    expect(color.b).toBe(0);
  });
});

describe("tallyColorHex", () => {
  it("returns #ff0000 for program", () => {
    expect(tallyColorHex("program")).toBe("#ff0000");
  });

  it("returns #00ff00 for preview", () => {
    expect(tallyColorHex("preview")).toBe("#00ff00");
  });

  it("returns #000000 for idle", () => {
    expect(tallyColorHex("idle")).toBe("#000000");
  });
});

describe("filterByTally", () => {
  it("returns only program sources", () => {
    const snap = computeTally(sources, ["p1"], ["p2"]);
    const program = filterByTally(snap, "program");
    expect(program).toHaveLength(1);
    expect(program[0].sourceId).toBe("p1");
  });

  it("returns only preview sources", () => {
    const snap = computeTally(sources, ["p1"], ["p2", "p3"]);
    const preview = filterByTally(snap, "preview");
    expect(preview).toHaveLength(2);
  });

  it("returns idle sources", () => {
    const snap = computeTally(sources, ["p1"], ["p2"]);
    const idle = filterByTally(snap, "idle");
    expect(idle.length).toBe(2);
  });
});

describe("isOnProgram / isOnPreview", () => {
  it("correctly identifies program source", () => {
    const snap = computeTally(sources, ["p1"], ["p2"]);
    expect(isOnProgram(snap, "p1")).toBe(true);
    expect(isOnProgram(snap, "p2")).toBe(false);
  });

  it("correctly identifies preview source", () => {
    const snap = computeTally(sources, ["p1"], ["p2"]);
    expect(isOnPreview(snap, "p2")).toBe(true);
    expect(isOnPreview(snap, "p1")).toBe(false);
  });

  it("returns false for idle source", () => {
    const snap = computeTally(sources, ["p1"], ["p2"]);
    expect(isOnProgram(snap, "p3")).toBe(false);
    expect(isOnPreview(snap, "p3")).toBe(false);
  });
});

describe("exportTallyReport", () => {
  it("returns a flat map of sourceId to tally state", () => {
    const snap = computeTally(sources, ["p1"], ["p2"]);
    const report = exportTallyReport(snap);
    expect(report["p1"]).toBe("program");
    expect(report["p2"]).toBe("preview");
    expect(report["p3"]).toBe("idle");
    expect(report["ss1"]).toBe("idle");
  });

  it("includes all sources in the report", () => {
    const snap = computeTally(sources, [], []);
    const report = exportTallyReport(snap);
    expect(Object.keys(report)).toHaveLength(sources.length);
  });
});

describe("mergeTallySnapshots", () => {
  it("combines program sources from both snapshots", () => {
    const a = computeTally(
      [{ sourceId: "p1", label: "Alice" }],
      ["p1"],
      []
    );
    const b = computeTally(
      [{ sourceId: "p2", label: "Bob" }],
      ["p2"],
      []
    );
    const merged = mergeTallySnapshots(a, b);
    expect(merged.programSourceIds).toContain("p1");
    expect(merged.programSourceIds).toContain("p2");
  });

  it("program wins over preview in merge", () => {
    const a = computeTally(
      [{ sourceId: "p1", label: "Alice" }],
      ["p1"],
      []
    );
    const b = computeTally(
      [{ sourceId: "p1", label: "Alice" }],
      [],
      ["p1"]
    );
    const merged = mergeTallySnapshots(a, b);
    const p1 = merged.sources.find((s) => s.sourceId === "p1");
    expect(p1?.tally).toBe("program");
  });

  it("preview sources that are not in program appear as preview", () => {
    const a = computeTally(
      [{ sourceId: "p1", label: "Alice" }],
      ["p1"],
      []
    );
    const b = computeTally(
      [{ sourceId: "p2", label: "Bob" }],
      [],
      ["p2"]
    );
    const merged = mergeTallySnapshots(a, b);
    expect(merged.previewSourceIds).toContain("p2");
    expect(merged.programSourceIds).not.toContain("p2");
  });
});
