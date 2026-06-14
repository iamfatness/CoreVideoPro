import { describe, expect, it } from "vitest";
import {
  addCue,
  computeCueProgress,
  createCueSheet,
  cueTypeLabel,
  endCue,
  exportCueSheet,
  goLiveCue,
  nextCue,
  removeCue,
  reorderCue,
  skipCue,
  summarizeCueSheet,
} from "./cueSheet";

const NOW = 1000;

function makeSheet() {
  let s = createCueSheet("Test Show");
  s = addCue(s, "intro", "Welcome", 60);
  s = addCue(s, "segment", "Topic 1", 300);
  s = addCue(s, "break", "Break", 120);
  s = addCue(s, "qa", "Q&A", 180);
  s = addCue(s, "outro", "Goodbye", 60);
  return s;
}

// ---------------------------------------------------------------------------
// createCueSheet
// ---------------------------------------------------------------------------

describe("createCueSheet", () => {
  it("creates a sheet with the given title", () => {
    const s = createCueSheet("My Show");
    expect(s.showTitle).toBe("My Show");
  });

  it("starts with no cues when none provided", () => {
    const s = createCueSheet("My Show");
    expect(s.cues).toHaveLength(0);
  });

  it("starts with activeCueId null", () => {
    const s = createCueSheet("My Show");
    expect(s.activeCueId).toBeNull();
  });

  it("accepts partial cues and fills defaults", () => {
    const s = createCueSheet("Show", [{ title: "Intro", type: "intro" }]);
    expect(s.cues).toHaveLength(1);
    expect(s.cues[0].status).toBe("pending");
    expect(s.cues[0].plannedDurationSec).toBe(0);
  });

  it("numbers initial cues starting at 1", () => {
    const s = createCueSheet("Show", [{ title: "A" }, { title: "B" }]);
    expect(s.cues[0].order).toBe(1);
    expect(s.cues[1].order).toBe(2);
  });
});

// ---------------------------------------------------------------------------
// addCue
// ---------------------------------------------------------------------------

describe("addCue", () => {
  it("appends a cue at the end", () => {
    const s = addCue(createCueSheet("Show"), "intro", "Welcome", 60);
    expect(s.cues).toHaveLength(1);
    expect(s.cues[0].title).toBe("Welcome");
  });

  it("sets correct order for first cue", () => {
    const s = addCue(createCueSheet("Show"), "intro", "Welcome", 60);
    expect(s.cues[0].order).toBe(1);
  });

  it("increments order for subsequent cues", () => {
    const s = makeSheet();
    expect(s.cues[4].order).toBe(5);
  });

  it("sets planned duration", () => {
    const s = addCue(createCueSheet("Show"), "segment", "Talk", 300);
    expect(s.cues[0].plannedDurationSec).toBe(300);
  });

  it("sets status to pending", () => {
    const s = addCue(createCueSheet("Show"), "segment", "Talk", 300);
    expect(s.cues[0].status).toBe("pending");
  });

  it("sets notes when provided", () => {
    const s = addCue(createCueSheet("Show"), "segment", "Talk", 300, "Smile!");
    expect(s.cues[0].notes).toBe("Smile!");
  });

  it("defaults notes to empty string", () => {
    const s = addCue(createCueSheet("Show"), "segment", "Talk", 300);
    expect(s.cues[0].notes).toBe("");
  });

  it("starts with null actualStartSec and actualEndSec", () => {
    const s = addCue(createCueSheet("Show"), "intro", "Welcome", 60);
    expect(s.cues[0].actualStartSec).toBeNull();
    expect(s.cues[0].actualEndSec).toBeNull();
  });
});

// ---------------------------------------------------------------------------
// removeCue
// ---------------------------------------------------------------------------

describe("removeCue", () => {
  it("removes the specified cue", () => {
    const s = makeSheet();
    const id = s.cues[2].id;
    const updated = removeCue(s, id);
    expect(updated.cues.find((c) => c.id === id)).toBeUndefined();
  });

  it("re-numbers remaining cues 1-based", () => {
    const s = makeSheet();
    const id = s.cues[1].id;
    const updated = removeCue(s, id);
    const orders = updated.cues.map((c) => c.order);
    expect(orders).toEqual([1, 2, 3, 4]);
  });

  it("clears activeCueId if removed cue was active", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    const activeId = s.activeCueId!;
    s = removeCue(s, activeId);
    expect(s.activeCueId).toBeNull();
  });

  it("preserves activeCueId if a different cue was removed", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    const activeId = s.activeCueId!;
    s = removeCue(s, s.cues[1].id);
    expect(s.activeCueId).toBe(activeId);
  });

  it("is a no-op for unknown id", () => {
    const s = makeSheet();
    expect(removeCue(s, "bad-id").cues).toHaveLength(5);
  });
});

// ---------------------------------------------------------------------------
// reorderCue
// ---------------------------------------------------------------------------

describe("reorderCue", () => {
  it("moves a cue to a new position", () => {
    const s = makeSheet();
    const cueId = s.cues[4].id; // last (outro) → move to position 1
    const updated = reorderCue(s, cueId, 1);
    expect(updated.cues[0].id).toBe(cueId);
  });

  it("re-numbers after reorder", () => {
    const s = makeSheet();
    const updated = reorderCue(s, s.cues[0].id, 3);
    const orders = updated.cues.map((c) => c.order);
    expect(orders).toEqual([1, 2, 3, 4, 5]);
  });

  it("clamps order to valid range (lower)", () => {
    const s = makeSheet();
    const updated = reorderCue(s, s.cues[2].id, 0);
    expect(updated.cues[0].id).toBe(s.cues[2].id);
  });

  it("clamps order to valid range (upper)", () => {
    const s = makeSheet();
    const updated = reorderCue(s, s.cues[0].id, 999);
    expect(updated.cues[4].id).toBe(s.cues[0].id);
  });

  it("returns unchanged sheet for unknown cueId", () => {
    const s = makeSheet();
    expect(reorderCue(s, "nope", 2).cues).toEqual(s.cues);
  });
});

// ---------------------------------------------------------------------------
// goLiveCue
// ---------------------------------------------------------------------------

describe("goLiveCue", () => {
  it("sets cue status to live", () => {
    const s = makeSheet();
    const updated = goLiveCue(s, s.cues[0].id, NOW);
    expect(updated.cues[0].status).toBe("live");
  });

  it("records actualStartSec", () => {
    const s = makeSheet();
    const updated = goLiveCue(s, s.cues[0].id, NOW);
    expect(updated.cues[0].actualStartSec).toBe(NOW);
  });

  it("sets activeCueId", () => {
    const s = makeSheet();
    const id = s.cues[0].id;
    const updated = goLiveCue(s, id, NOW);
    expect(updated.activeCueId).toBe(id);
  });

  it("auto-ends the previous live cue", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    s = goLiveCue(s, s.cues[1].id, NOW + 60);
    expect(s.cues[0].status).toBe("done");
    expect(s.cues[0].actualEndSec).toBe(NOW + 60);
  });

  it("new cue becomes live after auto-end", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    s = goLiveCue(s, s.cues[1].id, NOW + 60);
    expect(s.cues[1].status).toBe("live");
    expect(s.activeCueId).toBe(s.cues[1].id);
  });
});

// ---------------------------------------------------------------------------
// endCue
// ---------------------------------------------------------------------------

describe("endCue", () => {
  it("sets cue status to done", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    s = endCue(s, s.cues[0].id, NOW + 60);
    expect(s.cues[0].status).toBe("done");
  });

  it("records actualEndSec", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    s = endCue(s, s.cues[0].id, NOW + 60);
    expect(s.cues[0].actualEndSec).toBe(NOW + 60);
  });

  it("clears activeCueId when ending the active cue", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    s = endCue(s, s.cues[0].id, NOW + 60);
    expect(s.activeCueId).toBeNull();
  });

  it("preserves activeCueId when ending a different cue", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    s = endCue(s, s.cues[1].id, NOW + 60);
    expect(s.activeCueId).toBe(s.cues[0].id);
  });
});

// ---------------------------------------------------------------------------
// skipCue
// ---------------------------------------------------------------------------

describe("skipCue", () => {
  it("skips a pending cue", () => {
    const s = makeSheet();
    const updated = skipCue(s, s.cues[1].id);
    expect(updated.cues[1].status).toBe("skipped");
  });

  it("skips a ready cue", () => {
    let s = makeSheet();
    s = { ...s, cues: s.cues.map((c, i) => i === 0 ? { ...c, status: "ready" as const } : c) };
    const updated = skipCue(s, s.cues[0].id);
    expect(updated.cues[0].status).toBe("skipped");
  });

  it("does not skip a live cue", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    const updated = skipCue(s, s.cues[0].id);
    expect(updated.cues[0].status).toBe("live");
  });

  it("does not skip a done cue", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    s = endCue(s, s.cues[0].id, NOW + 60);
    const updated = skipCue(s, s.cues[0].id);
    expect(updated.cues[0].status).toBe("done");
  });
});

// ---------------------------------------------------------------------------
// nextCue
// ---------------------------------------------------------------------------

describe("nextCue", () => {
  it("returns first pending cue when no active cue", () => {
    const s = makeSheet();
    expect(nextCue(s)?.id).toBe(s.cues[0].id);
  });

  it("returns first pending/ready cue after the active cue", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    expect(nextCue(s)?.id).toBe(s.cues[1].id);
  });

  it("skips over skipped cues", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    s = skipCue(s, s.cues[1].id);
    expect(nextCue(s)?.id).toBe(s.cues[2].id);
  });

  it("returns null when no pending cues remain", () => {
    let s = makeSheet();
    for (const cue of s.cues) {
      s = skipCue(s, cue.id);
    }
    expect(nextCue(s)).toBeNull();
  });

  it("returns first pending if no pending cues after active", () => {
    let s = makeSheet();
    // Skip all but last
    s = skipCue(s, s.cues[0].id);
    s = skipCue(s, s.cues[1].id);
    s = skipCue(s, s.cues[2].id);
    s = goLiveCue(s, s.cues[3].id, NOW);
    // cues[4] is still pending, after active
    expect(nextCue(s)?.id).toBe(s.cues[4].id);
  });
});

// ---------------------------------------------------------------------------
// computeCueProgress
// ---------------------------------------------------------------------------

describe("computeCueProgress", () => {
  it("returns 0 elapsed when not started (uses nowSec as start)", () => {
    const s = makeSheet();
    const progress = computeCueProgress(s.cues[0], NOW);
    expect(progress.elapsedSec).toBe(0);
  });

  it("computes elapsed seconds correctly", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    const progress = computeCueProgress(s.cues[0], NOW + 30);
    expect(progress.elapsedSec).toBe(30);
  });

  it("computes remaining seconds correctly", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW); // planned = 60s
    const progress = computeCueProgress(s.cues[0], NOW + 40);
    expect(progress.remainingSec).toBe(20);
  });

  it("overrun is 0 when within planned time", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    const progress = computeCueProgress(s.cues[0], NOW + 50);
    expect(progress.overrunSec).toBe(0);
    expect(progress.overrun).toBe(false);
  });

  it("detects overrun when elapsed exceeds planned", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    const progress = computeCueProgress(s.cues[0], NOW + 90); // 30s overrun
    expect(progress.overrunSec).toBe(30);
    expect(progress.overrun).toBe(true);
  });

  it("computes pct correctly (50% through)", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW); // planned 60s
    const progress = computeCueProgress(s.cues[0], NOW + 30);
    expect(progress.pct).toBeCloseTo(50, 0);
  });

  it("clamps pct to 100 when overrun", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    const progress = computeCueProgress(s.cues[0], NOW + 200);
    expect(progress.pct).toBe(100);
  });
});

// ---------------------------------------------------------------------------
// summarizeCueSheet
// ---------------------------------------------------------------------------

describe("summarizeCueSheet", () => {
  it("computes totalPlannedSec from all cues", () => {
    const s = makeSheet(); // 60+300+120+180+60 = 720
    const summary = summarizeCueSheet(s, NOW);
    expect(summary.totalPlannedSec).toBe(720);
  });

  it("counts done cues", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    s = endCue(s, s.cues[0].id, NOW + 60);
    const summary = summarizeCueSheet(s, NOW + 60);
    expect(summary.doneCount).toBe(1);
  });

  it("counts pending cues", () => {
    const s = makeSheet();
    const summary = summarizeCueSheet(s, NOW);
    expect(summary.pendingCount).toBe(5);
  });

  it("reduces pendingCount when cue is done", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    s = endCue(s, s.cues[0].id, NOW + 60);
    const summary = summarizeCueSheet(s, NOW + 60);
    expect(summary.pendingCount).toBe(4);
  });

  it("computes overrunSec for done cues", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW); // planned 60s
    s = endCue(s, s.cues[0].id, NOW + 90); // 30s overrun
    const summary = summarizeCueSheet(s, NOW + 90);
    expect(summary.overrunSec).toBe(30);
  });

  it("estimatedEndSec equals nowSec + remaining planned time", () => {
    let s = makeSheet(); // total planned 720s
    s = goLiveCue(s, s.cues[0].id, NOW);
    s = endCue(s, s.cues[0].id, NOW + 60);
    // remaining: 300+120+180+60 = 660
    const summary = summarizeCueSheet(s, NOW + 60);
    expect(summary.estimatedEndSec).toBe(NOW + 60 + 660);
  });

  it("summary string contains done and pending counts", () => {
    const s = makeSheet();
    const summary = summarizeCueSheet(s, NOW);
    expect(summary.summary).toContain("0 done");
    expect(summary.summary).toContain("5 pending");
  });
});

// ---------------------------------------------------------------------------
// exportCueSheet — text
// ---------------------------------------------------------------------------

describe("exportCueSheet (text)", () => {
  it("includes the show title", () => {
    const s = makeSheet();
    const out = exportCueSheet(s, "text");
    expect(out).toContain("Test Show");
  });

  it("includes cue titles", () => {
    const s = makeSheet();
    const out = exportCueSheet(s, "text");
    expect(out).toContain("Welcome");
  });

  it("includes cue notes when present", () => {
    let s = createCueSheet("Show");
    s = addCue(s, "intro", "Welcome", 60, "Smile!");
    const out = exportCueSheet(s, "text");
    expect(out).toContain("Smile!");
  });

  it("marks live cues with [LIVE]", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    const out = exportCueSheet(s, "text");
    expect(out).toContain("[LIVE]");
  });

  it("marks done cues with [DONE]", () => {
    let s = makeSheet();
    s = goLiveCue(s, s.cues[0].id, NOW);
    s = endCue(s, s.cues[0].id, NOW + 60);
    const out = exportCueSheet(s, "text");
    expect(out).toContain("[DONE]");
  });

  it("marks skipped cues with [SKIP]", () => {
    let s = makeSheet();
    s = skipCue(s, s.cues[1].id);
    const out = exportCueSheet(s, "text");
    expect(out).toContain("[SKIP]");
  });
});

// ---------------------------------------------------------------------------
// exportCueSheet — json
// ---------------------------------------------------------------------------

describe("exportCueSheet (json)", () => {
  it("produces valid JSON", () => {
    const s = makeSheet();
    const out = exportCueSheet(s, "json");
    expect(() => JSON.parse(out)).not.toThrow();
  });

  it("JSON includes showTitle", () => {
    const s = makeSheet();
    const parsed = JSON.parse(exportCueSheet(s, "json"));
    expect(parsed.showTitle).toBe("Test Show");
  });

  it("JSON includes all cues", () => {
    const s = makeSheet();
    const parsed = JSON.parse(exportCueSheet(s, "json"));
    expect(parsed.cues).toHaveLength(5);
  });
});

// ---------------------------------------------------------------------------
// exportCueSheet — csv
// ---------------------------------------------------------------------------

describe("exportCueSheet (csv)", () => {
  it("starts with a header row", () => {
    const s = makeSheet();
    const lines = exportCueSheet(s, "csv").split("\n");
    expect(lines[0]).toBe("order,type,title,planned,status");
  });

  it("has one data row per cue", () => {
    const s = makeSheet();
    const lines = exportCueSheet(s, "csv").split("\n");
    expect(lines).toHaveLength(s.cues.length + 1); // header + cues
  });

  it("includes planned duration in csv row", () => {
    let s = createCueSheet("Show");
    s = addCue(s, "intro", "Intro", 90);
    const lines = exportCueSheet(s, "csv").split("\n");
    expect(lines[1]).toContain("90");
  });

  it("escapes double quotes in title", () => {
    let s = createCueSheet("Show");
    s = addCue(s, "segment", 'He said "hello"', 30);
    const out = exportCueSheet(s, "csv");
    expect(out).toContain('""hello""');
  });
});

// ---------------------------------------------------------------------------
// cueTypeLabel
// ---------------------------------------------------------------------------

describe("cueTypeLabel", () => {
  it("intro → Intro", () => expect(cueTypeLabel("intro")).toBe("Intro"));
  it("segment → Segment", () => expect(cueTypeLabel("segment")).toBe("Segment"));
  it("break → Break", () => expect(cueTypeLabel("break")).toBe("Break"));
  it("qa → Q&A", () => expect(cueTypeLabel("qa")).toBe("Q&A"));
  it("outro → Outro", () => expect(cueTypeLabel("outro")).toBe("Outro"));
  it("ad-break → Ad Break", () => expect(cueTypeLabel("ad-break")).toBe("Ad Break"));
  it("bumper → Bumper", () => expect(cueTypeLabel("bumper")).toBe("Bumper"));
});
