import { describe, expect, it } from "vitest";
import {
  activeChapterAt,
  addChapter,
  chaptersFromRundown,
  exportChapters,
  formatChapterTime,
  removeChapter,
  summarizeChapters,
  updateChapter,
  validateChapters,
} from "./chapterMarker";

const EMPTY_LIST = { markers: [], recordingDurationSec: 3600 };

function buildList() {
  let list = addChapter(EMPTY_LIST, 0, "Introduction");
  list = addChapter(list, 120, "Main Topic");
  list = addChapter(list, 600, "Q&A");
  return list;
}

describe("addChapter", () => {
  it("adds a chapter to an empty list", () => {
    const list = addChapter(EMPTY_LIST, 0, "Intro");
    expect(list.markers).toHaveLength(1);
    expect(list.markers[0].title).toBe("Intro");
  });

  it("keeps list sorted by startSec when inserting out of order", () => {
    let list = addChapter(EMPTY_LIST, 300, "Middle");
    list = addChapter(list, 0, "Start");
    expect(list.markers[0].title).toBe("Start");
    expect(list.markers[1].title).toBe("Middle");
  });

  it("uses 'Chapter' as default title for blank input", () => {
    const list = addChapter(EMPTY_LIST, 0, "   ");
    expect(list.markers[0].title).toBe("Chapter");
  });

  it("clamps startSec to 0 when negative", () => {
    const list = addChapter(EMPTY_LIST, -10, "Early");
    expect(list.markers[0].startSec).toBe(0);
  });

  it("preserves description when provided", () => {
    const list = addChapter(EMPTY_LIST, 60, "Sponsor", "Ad break");
    expect(list.markers[0].description).toBe("Ad break");
  });
});

describe("removeChapter", () => {
  it("removes chapter by id", () => {
    const list = buildList();
    const id = list.markers[1].id;
    const after = removeChapter(list, id);
    expect(after.markers).toHaveLength(2);
    expect(after.markers.every((m) => m.id !== id)).toBe(true);
  });

  it("does nothing for unknown id", () => {
    const list = buildList();
    expect(removeChapter(list, "nope").markers).toHaveLength(3);
  });
});

describe("updateChapter", () => {
  it("updates title", () => {
    const list = buildList();
    const id = list.markers[0].id;
    const updated = updateChapter(list, id, { title: "Opening" });
    expect(updated.markers.find((m) => m.id === id)?.title).toBe("Opening");
  });

  it("re-sorts after startSec change", () => {
    const list = buildList();
    const id = list.markers[0].id; // was at 0
    const updated = updateChapter(list, id, { startSec: 900 });
    expect(updated.markers[updated.markers.length - 1].id).toBe(id);
  });
});

describe("validateChapters", () => {
  it("returns warning for empty list", () => {
    expect(validateChapters(EMPTY_LIST)).toContain("No chapters defined");
  });

  it("warns when YouTube first chapter is not at 00:00", () => {
    let list = addChapter(EMPTY_LIST, 30, "Intro");
    list = addChapter(list, 120, "Topic");
    list = addChapter(list, 300, "End");
    const warnings = validateChapters(list, "youtube");
    expect(warnings.some((w) => w.includes("00:00"))).toBe(true);
  });

  it("warns when fewer than 3 YouTube chapters", () => {
    let list = addChapter(EMPTY_LIST, 0, "Only");
    list = addChapter(list, 60, "Two");
    const warnings = validateChapters(list, "youtube");
    expect(warnings.some((w) => w.includes("at least 3"))).toBe(true);
  });

  it("warns when chapters are too close together", () => {
    let list = addChapter(EMPTY_LIST, 0, "A");
    list = addChapter(list, 5, "B"); // only 5s gap
    const warnings = validateChapters(list);
    expect(warnings.some((w) => w.includes("5.0s"))).toBe(true);
  });

  it("returns empty warnings for valid generic list", () => {
    const list = buildList();
    expect(validateChapters(list)).toHaveLength(0);
  });
});

describe("formatChapterTime", () => {
  it("formats under 1 hour as MM:SS", () => {
    expect(formatChapterTime(0)).toBe("00:00");
    expect(formatChapterTime(65)).toBe("01:05");
    expect(formatChapterTime(3599)).toBe("59:59");
  });

  it("formats 1 hour and over as HH:MM:SS", () => {
    expect(formatChapterTime(3600)).toBe("01:00:00");
    expect(formatChapterTime(3661)).toBe("01:01:01");
  });

  it("clamps negative values to 00:00", () => {
    expect(formatChapterTime(-10)).toBe("00:00");
  });
});

describe("exportChapters", () => {
  it("exports YouTube format with timestamps and titles", () => {
    const list = buildList();
    const out = exportChapters(list, "youtube");
    expect(out).toContain("00:00 Introduction");
    expect(out).toContain("02:00 Main Topic");
    expect(out).toContain("10:00 Q&A");
  });

  it("returns empty string for empty list on any format", () => {
    expect(exportChapters(EMPTY_LIST, "youtube")).toBe("");
    expect(exportChapters(EMPTY_LIST, "vtt")).toBe("");
    expect(exportChapters(EMPTY_LIST, "text")).toBe("");
  });

  it("exports VTT format with WEBVTT header and time ranges", () => {
    const list = buildList();
    const out = exportChapters(list, "vtt");
    expect(out).toMatch(/^WEBVTT/);
    expect(out).toContain("-->");
    expect(out).toContain("Introduction");
  });

  it("exports text format with numbered list", () => {
    const list = buildList();
    const out = exportChapters(list, "text");
    expect(out).toContain("1. [00:00] Introduction");
    expect(out).toContain("2. [02:00] Main Topic");
  });

  it("exports JSON format as valid JSON array", () => {
    const list = buildList();
    const out = exportChapters(list, "json");
    const parsed = JSON.parse(out);
    expect(Array.isArray(parsed)).toBe(true);
    expect(parsed).toHaveLength(3);
  });

  it("includes description in text format when present", () => {
    let list = addChapter(EMPTY_LIST, 0, "Intro", "Opening remarks");
    list = addChapter(list, 60, "Main");
    list = addChapter(list, 300, "End");
    const out = exportChapters(list, "text");
    expect(out).toContain("Opening remarks");
  });
});

describe("activeChapterAt", () => {
  it("returns null when no chapters", () => {
    expect(activeChapterAt(EMPTY_LIST, 100)).toBeNull();
  });

  it("returns first chapter at position 0", () => {
    const list = buildList();
    expect(activeChapterAt(list, 0)?.title).toBe("Introduction");
  });

  it("returns correct chapter mid-recording", () => {
    const list = buildList();
    expect(activeChapterAt(list, 300)?.title).toBe("Main Topic");
  });

  it("returns last chapter past its start", () => {
    const list = buildList();
    expect(activeChapterAt(list, 700)?.title).toBe("Q&A");
  });

  it("returns null before the first chapter starts", () => {
    let list = addChapter(EMPTY_LIST, 60, "Late Start");
    expect(activeChapterAt(list, 30)).toBeNull();
  });
});

describe("chaptersFromRundown", () => {
  it("generates one chapter per segment", () => {
    const list = chaptersFromRundown([
      { name: "Intro", startSec: 0 },
      { name: "Talk", startSec: 120 },
    ]);
    expect(list.markers).toHaveLength(2);
    expect(list.markers[0].title).toBe("Intro");
  });

  it("handles empty rundown", () => {
    const list = chaptersFromRundown([]);
    expect(list.markers).toHaveLength(0);
  });
});

describe("summarizeChapters", () => {
  it("reports chapter count and total duration", () => {
    const list = buildList();
    const summary = summarizeChapters(list);
    expect(summary).toContain("3 chapters");
    expect(summary).toContain("01:00:00");
  });

  it("uses singular for one chapter", () => {
    const list = addChapter({ markers: [], recordingDurationSec: 300 }, 0, "Only");
    expect(summarizeChapters(list)).toContain("1 chapter");
  });

  it("returns 'No chapters' for empty list", () => {
    expect(summarizeChapters(EMPTY_LIST)).toBe("No chapters");
  });
});
