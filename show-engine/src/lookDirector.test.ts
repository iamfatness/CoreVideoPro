import { describe, expect, it } from "vitest";
import {
  clampPage,
  effectiveBoxFill,
  findChairSlots,
  pageCountFor,
  resolveLook
} from "./lookDirector.js";
import type { Capability, LookDefinition, Panelist, QueueState, Role, Slot } from "./contracts.js";

const available: Capability = { state: "available", detail: null };
const unavailable: Capability = { state: "unavailable", detail: "HTTP 503" };
const disabled: Capability = { state: "disabled", detail: null };

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
  plateTone: "accent",
  tallySource: "boxes",
  boxFill: "queue"
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
    const resolution = resolveLook(look, { queue, slots, page: 0, handsQueue: available });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: 3 },
      { box: 2, slot: 4 }
    ]);
  });

  it("carries the look's identity and layout through", () => {
    const resolution = resolveLook(look, { queue, slots, page: 0, handsQueue: available });
    expect(resolution.lookId).toBe("teatime");
    expect(resolution.scenePreset).toBe("scene-teatime");
    expect(resolution.plateTone).toBe("accent");
    expect(resolution.page).toBe(0);
    expect(resolution.pageCount).toBe(2);
  });

  it("pages forward through the queue", () => {
    const resolution = resolveLook(look, { queue, slots, page: 1, handsQueue: available });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: 5 },
      { box: 2, slot: null }
    ]);
  });

  it("seats the chairs when the look includes them", () => {
    const resolution = resolveLook(look, { queue, slots, page: 0, handsQueue: available });
    expect(resolution.hostSlot).toBe(1);
    expect(resolution.readerSlot).toBe(2);
  });

  it("omits the reader when the look excludes it", () => {
    const resolution = resolveLook(
      { ...look, includesReader: false },
      { queue, slots, page: 0, handsQueue: available }
    );
    expect(resolution.hostSlot).toBe(1);
    expect(resolution.readerSlot).toBeNull();
  });

  it("omits both chairs when the look excludes them", () => {
    const resolution = resolveLook(
      { ...look, includesHost: false, includesReader: false },
      { queue, slots, page: 0, handsQueue: available }
    );
    expect(resolution.hostSlot).toBeNull();
    expect(resolution.readerSlot).toBeNull();
  });

  it("leaves a box empty for a candidate who is not seated", () => {
    const resolution = resolveLook(look, {
      queue: { previous: [], current: "9999", upcoming: ["5555"] },
      slots,
      page: 0,
      handsQueue: available
    });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: null },
      { box: 2, slot: 4 }
    ]);
  });

  it("resolves to no boxes for a look with none", () => {
    const resolution = resolveLook(
      { ...look, boxes: 0 },
      { queue, slots, page: 0, handsQueue: available }
    );
    expect(resolution.boxes).toEqual([]);
  });

  it("blanks every box for an empty queue", () => {
    const resolution = resolveLook(look, {
      queue: { previous: [], current: null, upcoming: [] },
      slots,
      page: 0,
      handsQueue: available
    });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: null },
      { box: 2, slot: null }
    ]);
  });

  it("rejects a negative page", () => {
    expect(() =>
      resolveLook(look, { queue, slots, page: -1, handsQueue: available })
    ).toThrow(/page/);
  });

  it("rejects a page past the end", () => {
    expect(() =>
      resolveLook(look, { queue, slots, page: 2, handsQueue: available })
    ).toThrow(/page/);
  });

  it("emits one nameplate per occupied position, host and reader first", () => {
    const resolution = resolveLook(look, { queue, slots, page: 0, handsQueue: available });
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
      page: 0,
      handsQueue: available
    });
    expect(resolution.nameplates.map((plate) => plate.position)).toEqual([
      { kind: "host" },
      { kind: "reader" }
    ]);
  });

  it("emits no chair nameplates when the look excludes the chairs", () => {
    const resolution = resolveLook(
      { ...look, includesHost: false, includesReader: false },
      { queue, slots, page: 0, handsQueue: available }
    );
    expect(resolution.nameplates.every((plate) => plate.position.kind === "box")).toBe(true);
  });

  it("carries the look's tone onto every plate", () => {
    const resolution = resolveLook(
      { ...look, plateTone: "guest" },
      { queue, slots, page: 0, handsQueue: available }
    );
    expect(resolution.nameplates.every((plate) => plate.tone === "guest")).toBe(true);
  });

  it("does not mutate the roster or the queue", () => {
    const queueCopy: QueueState = {
      previous: [...queue.previous],
      current: queue.current,
      upcoming: [...queue.upcoming]
    };
    resolveLook(look, { queue: queueCopy, slots, page: 0, handsQueue: available });
    expect(queueCopy).toEqual(queue);
  });
});

describe("clampPage", () => {
  it("leaves a valid page alone", () => {
    expect(clampPage(look, queue, 0, available)).toBe(0);
    expect(clampPage(look, queue, 1, available)).toBe(1);
  });

  it("clamps a page past the end to the last valid page", () => {
    expect(clampPage(look, queue, 5, available)).toBe(1);
  });

  it("clamps a negative page to zero", () => {
    expect(clampPage(look, queue, -3, available)).toBe(0);
  });

  it("clamps to zero when the queue empties", () => {
    const empty: QueueState = { previous: [], current: null, upcoming: [] };
    expect(clampPage(look, empty, 3, available)).toBe(0);
  });

  it("clamps to zero for a look with no boxes", () => {
    expect(clampPage({ ...look, boxes: 0 }, queue, 2, available)).toBe(0);
  });

  it("throws on a non-integer page", () => {
    expect(() => clampPage(look, queue, 1.5, available)).toThrow(/page/);
    expect(() => clampPage(look, queue, Number.NaN, available)).toThrow(/page/);
  });

  it("clamps to zero when the hands feed is unusable, since manual fill has one page", () => {
    expect(clampPage(look, queue, 1, unavailable)).toBe(0);
    expect(clampPage(look, queue, 1, disabled)).toBe(0);
  });

  it("treats an omitted capability as unusable, exactly as resolveLook does", () => {
    expect(clampPage(look, queue, 1)).toBe(0);
  });

  /**
   * The load-bearing property, quantified over BOTH the page and the
   * capability. Restricting it to `page: 0` is what let a real bug through:
   * a hands feed dying while the operator sat on page 1 left a stale page
   * that a capability-blind clamp passed through unchanged, and resolveLook
   * then threw on it — a capability state blocking a tick, which the whole
   * capability model exists to prevent.
   */
  it("produces a page resolveLook accepts, for any integer page and any capability", () => {
    for (const handsQueue of [available, unavailable, disabled]) {
      for (const candidate of [-10, 0, 1, 2, 99]) {
        const page = clampPage(look, queue, candidate, handsQueue);
        expect(() =>
          resolveLook(look, { queue, slots, page, handsQueue })
        ).not.toThrow();
      }
    }
  });

  it("survives the hands feed dying while the operator is on a later page", () => {
    const page = clampPage(look, queue, 1, available);
    expect(page).toBe(1);

    const degraded = clampPage(look, queue, page, unavailable);
    const resolution = resolveLook(look, {
      queue,
      slots,
      page: degraded,
      handsQueue: unavailable,
      manualBoxes: { 1: 3, 2: 4 }
    });
    expect(resolution.boxFill).toBe("manual");
    expect(resolution.page).toBe(0);
    expect(resolution.pageCount).toBe(1);
    expect(resolution.boxes).toEqual([
      { box: 1, slot: 3 },
      { box: 2, slot: 4 }
    ]);
  });
});

describe("effectiveBoxFill", () => {
  it("keeps queue fill when the look asks for it and hands are available", () => {
    expect(effectiveBoxFill(look, available)).toBe("queue");
  });

  it("degrades to manual when hands are unavailable", () => {
    expect(effectiveBoxFill(look, unavailable)).toBe("manual");
  });

  it("degrades to manual when hands are disabled", () => {
    expect(effectiveBoxFill(look, disabled)).toBe("manual");
  });

  it("treats unavailable and disabled identically", () => {
    expect(effectiveBoxFill(look, unavailable)).toBe(effectiveBoxFill(look, disabled));
  });

  it("keeps manual fill regardless of hands", () => {
    const manual = { ...look, boxFill: "manual" as const };
    expect(effectiveBoxFill(manual, available)).toBe("manual");
  });
});

describe("resolveLook under manual fill", () => {
  const manualLook = { ...look, boxFill: "manual" as const };

  it("fills boxes from the manual assignments", () => {
    const resolution = resolveLook(manualLook, {
      queue,
      slots,
      page: 0,
      manualBoxes: { 1: 3, 2: 5 }
    });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: 3 },
      { box: 2, slot: 5 }
    ]);
  });

  it("reports the effective strategy", () => {
    const resolution = resolveLook(manualLook, { queue, slots, page: 0 });
    expect(resolution.boxFill).toBe("manual");
  });

  it("reports a single page", () => {
    const resolution = resolveLook(manualLook, { queue, slots, page: 0 });
    expect(resolution.pageCount).toBe(1);
    expect(resolution.page).toBe(0);
  });

  it("leaves an unassigned box empty", () => {
    const resolution = resolveLook(manualLook, { queue, slots, page: 0, manualBoxes: { 1: 3 } });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: 3 },
      { box: 2, slot: null }
    ]);
  });

  it("ignores an assignment naming an empty slot", () => {
    const withHole = [...slots, { slot: 6, panelist: null }];
    const resolution = resolveLook(manualLook, {
      queue,
      slots: withHole,
      page: 0,
      manualBoxes: { 1: 6 }
    });
    expect(resolution.boxes[0]).toEqual({ box: 1, slot: null });
  });

  it("ignores an assignment naming an out-of-range slot rather than throwing", () => {
    expect(() =>
      resolveLook(manualLook, { queue, slots, page: 0, manualBoxes: { 1: 99 } })
    ).not.toThrow();
    const resolution = resolveLook(manualLook, {
      queue,
      slots,
      page: 0,
      manualBoxes: { 1: 99 }
    });
    expect(resolution.boxes[0]).toEqual({ box: 1, slot: null });
  });

  it("throws on a non-zero page", () => {
    expect(() => resolveLook(manualLook, { queue, slots, page: 1 })).toThrow(/page/);
  });

  it("emits nameplates only for filled boxes", () => {
    const resolution = resolveLook(manualLook, { queue, slots, page: 0, manualBoxes: { 1: 3 } });
    const boxPlates = resolution.nameplates.filter((plate) => plate.position.kind === "box");
    expect(boxPlates).toHaveLength(1);
  });
});

describe("resolveLook degrading from queue to manual", () => {
  it("uses manual assignments when hands are unavailable", () => {
    const resolution = resolveLook(look, {
      queue,
      slots,
      page: 0,
      handsQueue: unavailable,
      manualBoxes: { 1: 5 }
    });
    expect(resolution.boxFill).toBe("manual");
    expect(resolution.boxes[0]).toEqual({ box: 1, slot: 5 });
  });

  it("does not empty the boxes when hands go away", () => {
    const resolution = resolveLook(look, {
      queue,
      slots,
      page: 0,
      handsQueue: unavailable,
      manualBoxes: { 1: 3, 2: 5 }
    });
    expect(resolution.boxes.every((box) => box.slot !== null)).toBe(true);
  });

  it("treats an omitted capability as unusable", () => {
    const resolution = resolveLook(look, { queue, slots, page: 0 });
    expect(resolution.boxFill).toBe("manual");
  });

  it("keeps queue fill when hands are available", () => {
    const resolution = resolveLook(look, { queue, slots, page: 0, handsQueue: available });
    expect(resolution.boxFill).toBe("queue");
    expect(resolution.boxes).toEqual([
      { box: 1, slot: 3 },
      { box: 2, slot: 4 }
    ]);
  });
});
