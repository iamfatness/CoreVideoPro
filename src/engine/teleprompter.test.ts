import { describe, expect, it } from "vitest";
import {
  activeLineIndex,
  advanceScroll,
  createTeleprompter,
  formatReadTime,
  jumpToLine,
  parseScript,
  pauseScroll,
  resetScroll,
  setSpeed,
  setWordsPerMinute,
  startScroll,
  teleprompterView,
} from "./teleprompter";

const SCRIPT = [
  "Welcome to the show today.", // 5 words
  "We have a great lineup for you.", // 7 words
  "Let us get started right away.", // 6 words
].join("\n");
// total = 18 words

describe("parseScript", () => {
  it("splits into non-empty lines", () => {
    const lines = parseScript("a\n\nb\n  \nc");
    expect(lines).toHaveLength(3);
    expect(lines.map((l) => l.text)).toEqual(["a", "b", "c"]);
  });

  it("trims each line", () => {
    expect(parseScript("  hello world  ")[0].text).toBe("hello world");
  });

  it("counts words per line", () => {
    const lines = parseScript(SCRIPT);
    expect(lines[0].wordCount).toBe(5);
    expect(lines[1].wordCount).toBe(7);
    expect(lines[2].wordCount).toBe(6);
  });

  it("computes cumulative words", () => {
    const lines = parseScript(SCRIPT);
    expect(lines[0].cumulativeWords).toBe(5);
    expect(lines[1].cumulativeWords).toBe(12);
    expect(lines[2].cumulativeWords).toBe(18);
  });

  it("indexes lines from 0", () => {
    const lines = parseScript(SCRIPT);
    expect(lines.map((l) => l.index)).toEqual([0, 1, 2]);
  });

  it("returns empty array for blank script", () => {
    expect(parseScript("   \n  ")).toHaveLength(0);
  });
});

describe("createTeleprompter", () => {
  it("computes total words", () => {
    expect(createTeleprompter(SCRIPT).totalWords).toBe(18);
  });

  it("starts at position 0, not running, speed 1", () => {
    const t = createTeleprompter(SCRIPT);
    expect(t.scrollWords).toBe(0);
    expect(t.running).toBe(false);
    expect(t.speed).toBe(1);
  });

  it("uses a default wpm", () => {
    expect(createTeleprompter(SCRIPT).wordsPerMinute).toBeGreaterThan(0);
  });

  it("accepts a custom wpm", () => {
    expect(createTeleprompter(SCRIPT, 200).wordsPerMinute).toBe(200);
  });

  it("clamps wpm to at least 1", () => {
    expect(createTeleprompter(SCRIPT, 0).wordsPerMinute).toBe(1);
  });
});

describe("start / pause / reset", () => {
  it("startScroll sets running true", () => {
    expect(startScroll(createTeleprompter(SCRIPT)).running).toBe(true);
  });

  it("pauseScroll sets running false", () => {
    const t = startScroll(createTeleprompter(SCRIPT));
    expect(pauseScroll(t).running).toBe(false);
  });

  it("resetScroll returns to position 0", () => {
    let t = createTeleprompter(SCRIPT);
    t = { ...t, scrollWords: 10 };
    expect(resetScroll(t).scrollWords).toBe(0);
  });

  it("resetScroll preserves running state", () => {
    let t = startScroll(createTeleprompter(SCRIPT));
    t = { ...t, scrollWords: 10 };
    expect(resetScroll(t).running).toBe(true);
  });
});

describe("setSpeed", () => {
  it("sets the speed", () => {
    expect(setSpeed(createTeleprompter(SCRIPT), 2).speed).toBe(2);
  });

  it("clamps speed to max 4", () => {
    expect(setSpeed(createTeleprompter(SCRIPT), 10).speed).toBe(4);
  });

  it("clamps speed to min 0.25", () => {
    expect(setSpeed(createTeleprompter(SCRIPT), 0).speed).toBe(0.25);
  });
});

describe("setWordsPerMinute", () => {
  it("updates wpm", () => {
    expect(setWordsPerMinute(createTeleprompter(SCRIPT), 180).wordsPerMinute).toBe(180);
  });

  it("clamps to at least 1", () => {
    expect(setWordsPerMinute(createTeleprompter(SCRIPT), -5).wordsPerMinute).toBe(1);
  });
});

describe("advanceScroll", () => {
  it("does not move when paused", () => {
    const t = createTeleprompter(SCRIPT);
    expect(advanceScroll(t, 1000).scrollWords).toBe(0);
  });

  it("advances at wpm words/min when running", () => {
    // 60 wpm => 1 word/sec; after 5000ms => 5 words
    let t = createTeleprompter(SCRIPT, 60);
    t = startScroll(t);
    expect(advanceScroll(t, 5000).scrollWords).toBeCloseTo(5, 5);
  });

  it("scales with speed multiplier", () => {
    let t = createTeleprompter(SCRIPT, 60);
    t = startScroll(setSpeed(t, 2));
    expect(advanceScroll(t, 5000).scrollWords).toBeCloseTo(10, 5);
  });

  it("clamps at total words and stops running", () => {
    let t = createTeleprompter(SCRIPT, 60);
    t = startScroll(t);
    const advanced = advanceScroll(t, 60000); // 60 words >> 18
    expect(advanced.scrollWords).toBe(18);
    expect(advanced.running).toBe(false);
  });

  it("ignores non-positive deltas", () => {
    let t = startScroll(createTeleprompter(SCRIPT, 60));
    t = { ...t, scrollWords: 3 };
    expect(advanceScroll(t, 0).scrollWords).toBe(3);
  });
});

describe("jumpToLine", () => {
  it("jumps to the start of a line", () => {
    const t = createTeleprompter(SCRIPT);
    // line 1 starts after 5 words
    expect(jumpToLine(t, 1).scrollWords).toBe(5);
  });

  it("jumps to line 2 start", () => {
    const t = createTeleprompter(SCRIPT);
    expect(jumpToLine(t, 2).scrollWords).toBe(12);
  });

  it("is a no-op for out-of-range index", () => {
    const t = createTeleprompter(SCRIPT);
    expect(jumpToLine(t, 99).scrollWords).toBe(0);
  });
});

describe("activeLineIndex", () => {
  it("is line 0 at the start", () => {
    expect(activeLineIndex(createTeleprompter(SCRIPT))).toBe(0);
  });

  it("moves to line 1 partway through", () => {
    let t = createTeleprompter(SCRIPT);
    t = { ...t, scrollWords: 6 }; // past line 0 (5 words)
    expect(activeLineIndex(t)).toBe(1);
  });

  it("is the last line at the end", () => {
    let t = createTeleprompter(SCRIPT);
    t = { ...t, scrollWords: 18 };
    expect(activeLineIndex(t)).toBe(2);
  });

  it("returns -1 for an empty script", () => {
    expect(activeLineIndex(createTeleprompter(""))).toBe(-1);
  });
});

describe("teleprompterView", () => {
  it("reports the active line", () => {
    const view = teleprompterView(createTeleprompter(SCRIPT));
    expect(view.activeLineIndex).toBe(0);
    expect(view.activeLine?.text).toBe("Welcome to the show today.");
  });

  it("progress is 0 at the start", () => {
    expect(teleprompterView(createTeleprompter(SCRIPT)).progressPct).toBe(0);
  });

  it("progress is 100 at the end", () => {
    let t = createTeleprompter(SCRIPT);
    t = { ...t, scrollWords: 18 };
    expect(teleprompterView(t).progressPct).toBe(100);
  });

  it("remaining words decreases with scroll", () => {
    let t = createTeleprompter(SCRIPT);
    t = { ...t, scrollWords: 6 };
    expect(teleprompterView(t).remainingWords).toBe(12);
  });

  it("estimates total read time from wpm", () => {
    // 18 words at 60 wpm => 18 sec
    const view = teleprompterView(createTeleprompter(SCRIPT, 60));
    expect(view.totalReadSec).toBe(18);
  });

  it("estimates remaining seconds", () => {
    let t = createTeleprompter(SCRIPT, 60);
    t = { ...t, scrollWords: 6 }; // 12 remaining => 12 sec
    expect(teleprompterView(t).remainingSec).toBe(12);
  });

  it("handles empty script gracefully", () => {
    const view = teleprompterView(createTeleprompter(""));
    expect(view.activeLine).toBeNull();
    expect(view.progressPct).toBe(0);
    expect(view.totalReadSec).toBe(0);
  });
});

describe("formatReadTime", () => {
  it("formats seconds as M:SS", () => {
    expect(formatReadTime(90)).toBe("1:30");
  });

  it("pads seconds", () => {
    expect(formatReadTime(65)).toBe("1:05");
  });

  it("handles zero", () => {
    expect(formatReadTime(0)).toBe("0:00");
  });

  it("clamps negatives to 0:00", () => {
    expect(formatReadTime(-10)).toBe("0:00");
  });
});
