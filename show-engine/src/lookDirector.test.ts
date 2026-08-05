import { describe, expect, it } from "vitest";
import { clampPage, findChairSlots, pageCountFor, resolveLook } from "./lookDirector.js";
import type { LookDefinition, Panelist, QueueState, Role, Slot } from "./contracts.js";

function panelist(pin: string | null, role: Role = "panelist"): Panelist {
  return {
    participantId: `p-${pin ?? "none"}`,
    rawName: `Name ${pin ?? ""}`,
    online: true,
    videoOn: true,
    audioOn: false,
    handRaised: false,
    zoomRole: 3,
    displayName: `Name ${pin ?? ""}`,
    location: "",
    pin,
    hasMukana: pin !== null,
    role
  };
}

function roster(entries: (Panelist | null)[]): Slot[] {
  return entries.map((p, index) => ({ slot: index + 1, panelist: p }));
}

const look: LookDefinition = {
  id: "teatime",
  label: "Teatime",
  scenePreset: "scene-teatime",
  boxes: 2,
  includesHost: true,
  includesReader: true,
  plateTone: "accent"
};

const queue: QueueState = {
  previous: [],
  current: "4242",
  upcoming: ["5555", "7777", "8888"]
};

const slots = roster([
  panelist("1383", "host"),
  panelist("2001", "reader"),
  panelist("4242"),
  panelist("5555"),
  panelist("7777")
]);

describe("findChairSlots", () => {
  it("finds the host and reader slots", () => {
    expect(findChairSlots(slots)).toEqual({ hostSlot: 1, readerSlot: 2 });
  });

  it("returns null for an unseated chair", () => {
    expect(findChairSlots(roster([panelist("4242")]))).toEqual({
      hostSlot: null,
      readerSlot: null
    });
  });

  it("ignores empty slots", () => {
    expect(findChairSlots(roster([null, panelist("1383", "host")]))).toEqual({
      hostSlot: 2,
      readerSlot: null
    });
  });
});

describe("pageCountFor", () => {
  it("counts pages across the candidate list", () => {
    expect(pageCountFor(look, queue)).toBe(2);
  });

  it("returns one page for an empty queue", () => {
    expect(pageCountFor(look, { previous: [], current: null, upcoming: [] })).toBe(1);
  });

  it("returns one page for a look with no boxes", () => {
    expect(pageCountFor({ ...look, boxes: 0 }, queue)).toBe(1);
  });
});

describe("resolveLook", () => {
  it("fills boxes from the front of the queue", () => {
    const resolution = resolveLook(look, { queue, slots, page: 0 });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: 3 },
      { box: 2, slot: 4 }
    ]);
  });

  it("carries the look's identity and layout through", () => {
    const resolution = resolveLook(look, { queue, slots, page: 0 });
    expect(resolution.lookId).toBe("teatime");
    expect(resolution.scenePreset).toBe("scene-teatime");
    expect(resolution.plateTone).toBe("accent");
    expect(resolution.page).toBe(0);
    expect(resolution.pageCount).toBe(2);
  });

  it("pages forward through the queue", () => {
    const resolution = resolveLook(look, { queue, slots, page: 1 });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: 5 },
      { box: 2, slot: null }
    ]);
  });

  it("seats the chairs when the look includes them", () => {
    const resolution = resolveLook(look, { queue, slots, page: 0 });
    expect(resolution.hostSlot).toBe(1);
    expect(resolution.readerSlot).toBe(2);
  });

  it("omits the reader when the look excludes it", () => {
    const resolution = resolveLook(
      { ...look, includesReader: false },
      { queue, slots, page: 0 }
    );
    expect(resolution.hostSlot).toBe(1);
    expect(resolution.readerSlot).toBeNull();
  });

  it("omits both chairs when the look excludes them", () => {
    const resolution = resolveLook(
      { ...look, includesHost: false, includesReader: false },
      { queue, slots, page: 0 }
    );
    expect(resolution.hostSlot).toBeNull();
    expect(resolution.readerSlot).toBeNull();
  });

  it("leaves a box empty for a candidate who is not seated", () => {
    const resolution = resolveLook(look, {
      queue: { previous: [], current: "9999", upcoming: ["5555"] },
      slots,
      page: 0
    });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: null },
      { box: 2, slot: 4 }
    ]);
  });

  it("resolves to no boxes for a look with none", () => {
    const resolution = resolveLook({ ...look, boxes: 0 }, { queue, slots, page: 0 });
    expect(resolution.boxes).toEqual([]);
  });

  it("blanks every box for an empty queue", () => {
    const resolution = resolveLook(look, {
      queue: { previous: [], current: null, upcoming: [] },
      slots,
      page: 0
    });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: null },
      { box: 2, slot: null }
    ]);
  });

  it("rejects a negative page", () => {
    expect(() => resolveLook(look, { queue, slots, page: -1 })).toThrow(/page/);
  });

  it("rejects a page past the end", () => {
    expect(() => resolveLook(look, { queue, slots, page: 2 })).toThrow(/page/);
  });

  it("emits one nameplate per occupied position, host and reader first", () => {
    const resolution = resolveLook(look, { queue, slots, page: 0 });
    expect(resolution.nameplates).toEqual([
      {
        position: { kind: "host" },
        slot: 1,
        name: "Name 1383",
        location: "",
        tone: "accent"
      },
      {
        position: { kind: "reader" },
        slot: 2,
        name: "Name 2001",
        location: "",
        tone: "accent"
      },
      {
        position: { kind: "box", box: 1 },
        slot: 3,
        name: "Name 4242",
        location: "",
        tone: "accent"
      },
      {
        position: { kind: "box", box: 2 },
        slot: 4,
        name: "Name 5555",
        location: "",
        tone: "accent"
      }
    ]);
  });

  it("emits no nameplate for an empty box", () => {
    const resolution = resolveLook(look, {
      queue: { previous: [], current: "9999", upcoming: [] },
      slots,
      page: 0
    });
    expect(resolution.nameplates.map((plate) => plate.position)).toEqual([
      { kind: "host" },
      { kind: "reader" }
    ]);
  });

  it("emits no chair nameplates when the look excludes the chairs", () => {
    const resolution = resolveLook(
      { ...look, includesHost: false, includesReader: false },
      { queue, slots, page: 0 }
    );
    expect(resolution.nameplates.every((plate) => plate.position.kind === "box")).toBe(true);
  });

  it("carries the look's tone onto every plate", () => {
    const resolution = resolveLook({ ...look, plateTone: "guest" }, { queue, slots, page: 0 });
    expect(resolution.nameplates.every((plate) => plate.tone === "guest")).toBe(true);
  });

  it("does not mutate the roster or the queue", () => {
    const queueCopy: QueueState = {
      previous: [...queue.previous],
      current: queue.current,
      upcoming: [...queue.upcoming]
    };
    resolveLook(look, { queue: queueCopy, slots, page: 0 });
    expect(queueCopy).toEqual(queue);
  });
});

describe("clampPage", () => {
  it("leaves a valid page alone", () => {
    expect(clampPage(look, queue, 0)).toBe(0);
    expect(clampPage(look, queue, 1)).toBe(1);
  });

  it("clamps a page past the end to the last valid page", () => {
    expect(clampPage(look, queue, 5)).toBe(1);
  });

  it("clamps a negative page to zero", () => {
    expect(clampPage(look, queue, -3)).toBe(0);
  });

  it("clamps to zero when the queue empties", () => {
    const empty: QueueState = { previous: [], current: null, upcoming: [] };
    expect(clampPage(look, empty, 3)).toBe(0);
  });

  it("clamps to zero for a look with no boxes", () => {
    expect(clampPage({ ...look, boxes: 0 }, queue, 2)).toBe(0);
  });

  it("throws on a non-integer page", () => {
    expect(() => clampPage(look, queue, 1.5)).toThrow(/page/);
    expect(() => clampPage(look, queue, Number.NaN)).toThrow(/page/);
  });

  it("produces a page resolveLook accepts, for any integer input", () => {
    for (const candidate of [-10, 0, 1, 2, 99]) {
      const page = clampPage(look, queue, candidate);
      expect(() => resolveLook(look, { queue, slots, page })).not.toThrow();
    }
  });
});
