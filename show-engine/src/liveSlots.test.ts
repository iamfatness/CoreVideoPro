import { describe, expect, it } from "vitest";
import { LiveSlots } from "./liveSlots.js";
import type { Panelist } from "./contracts.js";

function panelist(participantId: string, overrides: Partial<Panelist> = {}): Panelist {
  return {
    participantId,
    rawName: `Name ${participantId}`,
    online: true,
    videoOn: true,
    audioOn: false,
    handRaised: false,
    zoomRole: 3,
    displayName: `Name ${participantId}`,
    location: "",
    pin: null,
    hasMukana: false,
    role: "panelist",
    ...overrides
  };
}

function makeSlots(capacity = 4): LiveSlots {
  return new LiveSlots({ capacity, utilityPinBase: 9000 });
}

describe("LiveSlots core mechanics", () => {
  it("starts with the configured number of empty slots", () => {
    const slots = makeSlots();
    expect(slots.slots()).toEqual([
      { slot: 1, panelist: null },
      { slot: 2, panelist: null },
      { slot: 3, panelist: null },
      { slot: 4, panelist: null }
    ]);
    expect(slots.occupiedCount()).toBe(0);
  });

  it("adds panelists into ascending slots", () => {
    const slots = makeSlots();
    expect(slots.add(panelist("a"))).toBe(1);
    expect(slots.add(panelist("b"))).toBe(2);
    expect(slots.occupiedCount()).toBe(2);
  });

  it("returns the existing slot when adding a panelist twice", () => {
    const slots = makeSlots();
    slots.add(panelist("a"));
    expect(slots.add(panelist("a"))).toBe(1);
    expect(slots.occupiedCount()).toBe(1);
  });

  it("returns null when every slot is taken", () => {
    const slots = makeSlots(2);
    slots.add(panelist("a"));
    slots.add(panelist("b"));
    expect(slots.add(panelist("c"))).toBeNull();
  });

  it("leaves a hole on removal instead of compacting", () => {
    const slots = makeSlots();
    slots.add(panelist("a"));
    slots.add(panelist("b"));
    slots.add(panelist("c"));
    slots.removeSlot(2);
    expect(slots.slots().map((entry) => entry.panelist?.participantId ?? null)).toEqual([
      "a",
      null,
      "c",
      null
    ]);
  });

  it("fills the first hole on the next add", () => {
    const slots = makeSlots();
    slots.add(panelist("a"));
    slots.add(panelist("b"));
    slots.add(panelist("c"));
    slots.removeSlot(2);
    expect(slots.add(panelist("d"))).toBe(2);
  });

  it("replaces the occupant of a slot in place", () => {
    const slots = makeSlots();
    slots.add(panelist("a"));
    slots.replace(1, panelist("z"));
    expect(slots.slotOf("z")).toBe(1);
    expect(slots.slotOf("a")).toBeNull();
  });

  it("clears any prior slot when replacing with a panelist already seated", () => {
    const slots = makeSlots();
    slots.add(panelist("a"));
    slots.add(panelist("b"));
    slots.replace(1, panelist("b"));
    expect(slots.slotOf("b")).toBe(1);
    expect(slots.slots()[1]).toEqual({ slot: 2, panelist: null });
  });

  it("reports null for an unseated participant", () => {
    expect(makeSlots().slotOf("nobody")).toBeNull();
  });

  it("rejects out-of-range slot numbers", () => {
    const slots = makeSlots();
    expect(() => slots.removeSlot(0)).toThrow(/slot/);
    expect(() => slots.removeSlot(5)).toThrow(/slot/);
    expect(() => slots.replace(9, panelist("a"))).toThrow(/slot/);
  });

  it("rejects a capacity below 1", () => {
    expect(() => new LiveSlots({ capacity: 0, utilityPinBase: 9000 })).toThrow(/capacity/);
  });

  it("returns copies so callers cannot mutate internal state", () => {
    const slots = makeSlots();
    slots.add(panelist("a"));
    const view = slots.slots();
    view[0] = { slot: 1, panelist: null };
    expect(slots.slotOf("a")).toBe(1);
  });
});
