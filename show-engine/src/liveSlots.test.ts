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

describe("LiveSlots utility tail", () => {
  it("seats a utility PIN at the last slot", () => {
    const slots = makeSlots(8);
    expect(slots.add(panelist("bot", { pin: "9000" }))).toBe(8);
  });

  it("offsets successive utility PINs from the end", () => {
    const slots = makeSlots(8);
    expect(slots.add(panelist("bot0", { pin: "9000" }))).toBe(8);
    expect(slots.add(panelist("bot1", { pin: "9001" }))).toBe(7);
    expect(slots.add(panelist("bot2", { pin: "9002" }))).toBe(6);
  });

  it("keeps people out of the tail slots taken by bots", () => {
    const slots = makeSlots(4);
    slots.add(panelist("bot", { pin: "9000" }));
    expect(slots.add(panelist("person"))).toBe(1);
  });

  it("scans downward when the target tail slot is taken", () => {
    const slots = makeSlots(4);
    slots.add(panelist("bot0", { pin: "9000" }));
    expect(slots.add(panelist("bot0b", { pin: "9000" }))).toBe(3);
  });

  it("falls back to the first empty slot when the tail is exhausted", () => {
    const slots = makeSlots(2);
    slots.add(panelist("bot0", { pin: "9000" }));
    slots.add(panelist("bot1", { pin: "9001" }));
    slots.removeSlot(1);
    expect(slots.add(panelist("bot2", { pin: "9002" }))).toBe(1);
  });

  it("treats a non-numeric PIN as an ordinary panelist", () => {
    const slots = makeSlots(4);
    expect(slots.add(panelist("odd", { pin: "abcd" }))).toBe(1);
  });
});

describe("LiveSlots exclusive roles", () => {
  it("demotes a prior host when a new host is seated", () => {
    const slots = makeSlots();
    slots.add(panelist("a", { role: "host" }));
    slots.add(panelist("b", { role: "host" }));
    expect(slots.slots()[0]?.panelist?.role).toBe("panelist");
    expect(slots.slots()[1]?.panelist?.role).toBe("host");
  });

  it("demotes a prior reader on replace", () => {
    const slots = makeSlots();
    slots.add(panelist("a", { role: "reader" }));
    slots.add(panelist("b"));
    slots.replace(2, panelist("c", { role: "reader" }));
    expect(slots.slots()[0]?.panelist?.role).toBe("panelist");
    expect(slots.slots()[1]?.panelist?.role).toBe("reader");
  });

  it("leaves the host alone when a reader is seated", () => {
    const slots = makeSlots();
    slots.add(panelist("a", { role: "host" }));
    slots.add(panelist("b", { role: "reader" }));
    expect(slots.slots()[0]?.panelist?.role).toBe("host");
  });

  it("does not restrict non-exclusive roles", () => {
    const slots = makeSlots();
    slots.add(panelist("a", { role: "aslinterpreter" }));
    slots.add(panelist("b", { role: "aslinterpreter" }));
    expect(slots.slots().map((entry) => entry.panelist?.role)).toEqual([
      "aslinterpreter",
      "aslinterpreter",
      undefined,
      undefined
    ]);
  });
});

describe("LiveSlots rebuild and refresh", () => {
  it("rebuilds from a roster, seating bots in the tail", () => {
    const slots = makeSlots(4);
    slots.rebuild([
      panelist("a"),
      panelist("bot", { pin: "9000" }),
      panelist("b")
    ]);
    expect(slots.slots().map((entry) => entry.panelist?.participantId ?? null)).toEqual([
      "a",
      "b",
      null,
      "bot"
    ]);
  });

  it("clears previous occupants on rebuild", () => {
    const slots = makeSlots(4);
    slots.add(panelist("old"));
    slots.rebuild([panelist("new")]);
    expect(slots.slotOf("old")).toBeNull();
    expect(slots.slotOf("new")).toBe(1);
  });

  it("refreshes seated panelists in place without moving them", () => {
    const slots = makeSlots(4);
    slots.add(panelist("a"));
    slots.add(panelist("b"));
    slots.refresh(
      new Map([
        ["a", panelist("a", { displayName: "Renamed A", videoOn: false })],
        ["b", panelist("b")]
      ])
    );
    expect(slots.slotOf("a")).toBe(1);
    expect(slots.slots()[0]?.panelist).toMatchObject({
      displayName: "Renamed A",
      videoOn: false
    });
  });

  it("marks a vanished participant offline but keeps their seat", () => {
    const slots = makeSlots(4);
    slots.add(panelist("a"));
    slots.refresh(new Map());
    expect(slots.slotOf("a")).toBe(1);
    expect(slots.slots()[0]?.panelist).toMatchObject({ online: false, videoOn: false });
  });

  it("re-applies role uniqueness on refresh", () => {
    const slots = makeSlots(4);
    slots.add(panelist("a", { role: "host" }));
    slots.add(panelist("b"));
    slots.refresh(
      new Map([
        ["a", panelist("a", { role: "host" })],
        ["b", panelist("b", { role: "host" })]
      ])
    );
    const roles = slots.slots().map((entry) => entry.panelist?.role);
    expect(roles.filter((role) => role === "host")).toHaveLength(1);
  });
});

describe("LiveSlots persistence", () => {
  it("round-trips through JSON", () => {
    const slots = makeSlots(4);
    slots.add(panelist("a", { role: "host" }));
    slots.add(panelist("bot", { pin: "9000" }));
    slots.removeSlot(1);
    slots.add(panelist("c"));

    const restored = LiveSlots.fromJSON(slots.toJSON(), {
      capacity: 4,
      utilityPinBase: 9000
    });
    expect(restored.slots()).toEqual(slots.slots());
  });

  it("rejects a state whose capacity disagrees with the options", () => {
    const slots = makeSlots(4);
    expect(() =>
      LiveSlots.fromJSON(slots.toJSON(), { capacity: 8, utilityPinBase: 9000 })
    ).toThrow(/capacity/);
  });
});
