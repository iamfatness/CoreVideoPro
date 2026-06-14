import { describe, expect, it } from "vitest";
import {
  addTickerItem,
  advanceTicker,
  clearTicker,
  createTicker,
  currentTickerItem,
  pauseTicker,
  priorityLabel,
  removeTickerItem,
  reorderTickerItem,
  setTickerLoop,
  setTickerSpeed,
  startTicker,
  stopTicker,
  summarizeTicker
} from "./newsTicker";

describe("createTicker", () => {
  it("starts empty and paused", () => {
    const s = createTicker();
    expect(s.items).toHaveLength(0);
    expect(s.running).toBe(false);
    expect(s.speedMultiplier).toBe(1);
    expect(s.loop).toBe(true);
  });
});

describe("addTickerItem", () => {
  it("appends an item with default priority", () => {
    const s = addTickerItem(createTicker(), "Markets up 2%");
    expect(s.items).toHaveLength(1);
    expect(s.items[0].text).toBe("Markets up 2%");
    expect(s.items[0].priority).toBe("normal");
  });

  it("preserves tag and priority", () => {
    const s = addTickerItem(createTicker(), "Earthquake warning", "breaking", "ALERT");
    expect(s.items[0].priority).toBe("breaking");
    expect(s.items[0].tag).toBe("ALERT");
  });

  it("trims whitespace from text", () => {
    const s = addTickerItem(createTicker(), "  trimmed  ");
    expect(s.items[0].text).toBe("trimmed");
  });

  it("throws on empty text", () => {
    expect(() => addTickerItem(createTicker(), "   ")).toThrow();
  });

  it("can add multiple items", () => {
    let s = createTicker();
    s = addTickerItem(s, "Item 1");
    s = addTickerItem(s, "Item 2");
    s = addTickerItem(s, "Item 3");
    expect(s.items).toHaveLength(3);
  });
});

describe("removeTickerItem", () => {
  it("removes by id", () => {
    let s = createTicker();
    s = addTickerItem(s, "A");
    s = addTickerItem(s, "B");
    const id = s.items[0].id;
    s = removeTickerItem(s, id);
    expect(s.items).toHaveLength(1);
    expect(s.items[0].text).toBe("B");
  });

  it("is a no-op for unknown id", () => {
    const s = addTickerItem(createTicker(), "A");
    expect(removeTickerItem(s, "nonexistent")).toBe(s);
  });

  it("clamps currentIndex when last item is removed", () => {
    let s = createTicker();
    s = addTickerItem(s, "A");
    s = addTickerItem(s, "B");
    s = advanceTicker(s); // currentIndex = 1
    s = removeTickerItem(s, s.items[1].id); // remove B, now only A at 0
    expect(s.currentIndex).toBe(0);
  });
});

describe("reorderTickerItem", () => {
  it("moves item to new position", () => {
    let s = createTicker();
    s = addTickerItem(s, "A");
    s = addTickerItem(s, "B");
    s = addTickerItem(s, "C");
    const idA = s.items[0].id;
    s = reorderTickerItem(s, idA, 2);
    expect(s.items.map((i) => i.text)).toEqual(["B", "C", "A"]);
  });

  it("clamps to valid range", () => {
    let s = createTicker();
    s = addTickerItem(s, "A");
    s = addTickerItem(s, "B");
    const idA = s.items[0].id;
    s = reorderTickerItem(s, idA, 99);
    expect(s.items[s.items.length - 1].text).toBe("A");
  });
});

describe("start / pause / stop", () => {
  it("startTicker sets running", () => {
    expect(startTicker(createTicker()).running).toBe(true);
  });

  it("pauseTicker clears running but keeps position", () => {
    let s = createTicker();
    s = addTickerItem(s, "A");
    s = addTickerItem(s, "B");
    s = startTicker(s);
    s = advanceTicker(s);
    s = pauseTicker(s);
    expect(s.running).toBe(false);
    expect(s.currentIndex).toBe(1);
  });

  it("stopTicker resets position and cycle count", () => {
    let s = createTicker();
    s = addTickerItem(s, "A");
    s = addTickerItem(s, "B");
    s = startTicker(s);
    s = advanceTicker(s);
    s = { ...s, cycleCount: 3 };
    s = stopTicker(s);
    expect(s.running).toBe(false);
    expect(s.currentIndex).toBe(0);
    expect(s.cycleCount).toBe(0);
  });
});

describe("setTickerSpeed", () => {
  it("sets speed within bounds", () => {
    const s = setTickerSpeed(createTicker(), 2);
    expect(s.speedMultiplier).toBe(2);
  });

  it("clamps to minimum 0.25", () => {
    expect(setTickerSpeed(createTicker(), 0).speedMultiplier).toBe(0.25);
  });

  it("clamps to maximum 4", () => {
    expect(setTickerSpeed(createTicker(), 10).speedMultiplier).toBe(4);
  });
});

describe("advanceTicker", () => {
  it("moves to next item", () => {
    let s = createTicker();
    s = addTickerItem(s, "A");
    s = addTickerItem(s, "B");
    s = startTicker(s);
    s = advanceTicker(s);
    expect(s.currentIndex).toBe(1);
  });

  it("loops back to 0 when loop is true", () => {
    let s = createTicker();
    s = addTickerItem(s, "A");
    s = addTickerItem(s, "B");
    s = setTickerLoop(s, true);
    s = startTicker(s);
    s = advanceTicker(s); // → 1
    s = advanceTicker(s); // → loop back to 0
    expect(s.currentIndex).toBe(0);
    expect(s.cycleCount).toBe(1);
  });

  it("stops at last item when loop is false", () => {
    let s = createTicker();
    s = addTickerItem(s, "A");
    s = addTickerItem(s, "B");
    s = setTickerLoop(s, false);
    s = startTicker(s);
    s = advanceTicker(s); // → 1
    s = advanceTicker(s); // → stops, no loop
    expect(s.running).toBe(false);
    expect(s.currentIndex).toBe(1);
  });

  it("is a no-op on empty ticker", () => {
    const s = createTicker();
    expect(advanceTicker(s)).toBe(s);
  });
});

describe("clearTicker", () => {
  it("removes all items and resets state", () => {
    let s = createTicker();
    s = addTickerItem(s, "A");
    s = addTickerItem(s, "B");
    s = startTicker(s);
    s = clearTicker(s);
    expect(s.items).toHaveLength(0);
    expect(s.running).toBe(false);
    expect(s.currentIndex).toBe(0);
  });
});

describe("currentTickerItem", () => {
  it("returns the active item", () => {
    let s = createTicker();
    s = addTickerItem(s, "First");
    s = addTickerItem(s, "Second");
    s = advanceTicker(s);
    expect(currentTickerItem(s)?.text).toBe("Second");
  });

  it("returns undefined on empty ticker", () => {
    expect(currentTickerItem(createTicker())).toBeUndefined();
  });
});

describe("priorityLabel", () => {
  it("returns correct labels", () => {
    expect(priorityLabel("normal")).toBe("NEWS");
    expect(priorityLabel("breaking")).toBe("BREAKING");
    expect(priorityLabel("sponsored")).toBe("SPONSORED");
  });
});

describe("summarizeTicker", () => {
  it("reports empty state", () => {
    expect(summarizeTicker(createTicker())).toBe("Ticker empty");
  });

  it("shows running status with current item", () => {
    let s = createTicker();
    s = addTickerItem(s, "Breaking: storm approaching coast");
    s = startTicker(s);
    const summary = summarizeTicker(s);
    expect(summary).toContain("Running");
    expect(summary).toContain("1 items");
    expect(summary).toContain("Breaking:");
  });

  it("truncates long item text at 40 chars with ellipsis", () => {
    let s = createTicker();
    s = addTickerItem(s, "A".repeat(50));
    s = startTicker(s);
    const summary = summarizeTicker(s);
    expect(summary).toContain("…");
  });

  it("shows cycle count after first loop", () => {
    let s = createTicker();
    s = addTickerItem(s, "A");
    s = startTicker(s);
    s = advanceTicker(s); // loops, cycleCount = 1
    const summary = summarizeTicker(s);
    expect(summary).toContain("cycle 2");
  });
});
