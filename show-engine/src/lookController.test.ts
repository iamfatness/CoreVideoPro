// show-engine/src/lookController.test.ts
import { describe, expect, it } from "vitest";
import { LookController } from "./lookController.js";
import { resolveLook, type ManualBoxAssignments } from "./lookDirector.js";
import { resolvePersonKey } from "./personKey.js";
import type { Capability, LookDefinition, Panelist, QueueState, Slot } from "./contracts.js";

/** A 2-box, queue-fill look — the default fixture for most tests below. */
function lookDef(overrides: Partial<LookDefinition> = {}): LookDefinition {
  return {
    id: "teatime",
    label: "Teatime",
    scenePreset: "scene-teatime",
    boxes: 2,
    includesHost: true,
    includesReader: false,
    plateTone: "neutral",
    tallySource: "boxes",
    boxFill: "queue",
    ...overrides
  };
}

const TEATIME = lookDef();
const BANTER = lookDef({ id: "banter", label: "Banter", scenePreset: "scene-banter", boxes: 1, boxFill: "manual" });

const AVAILABLE: Capability = { state: "available", detail: null };
const UNAVAILABLE: Capability = { state: "unavailable", detail: "hands endpoint down" };

/** `current` = the first pin, `upcoming` = the rest — matches `queueOrder`'s own concatenation. */
function queueWith(pins: string[]): QueueState {
  return { previous: [], current: pins[0] ?? null, upcoming: pins.slice(1) };
}

/** 5 PINs against a 2-box look spans exactly 3 pages (`ceil(5/2)`) — the same fixture shape `showEngine.test.ts`'s `engineWithFill` uses. */
const FIVE_PIN_QUEUE = queueWith(["1001", "1002", "1003", "1004", "1005"]);

function controller(looks: readonly LookDefinition[] = [TEATIME, BANTER]): LookController {
  return new LookController({ looks });
}

/** One seated panelist, enough for `resolveLook` to tell an occupied slot from an empty one. */
function panelist(participantId: string, pin: string): Panelist {
  const rawName = `Name ${participantId} | ${pin} | Somewhere`;
  return {
    participantId,
    rawName,
    online: true,
    videoOn: true,
    audioOn: true,
    handRaised: false,
    zoomRole: 0,
    displayName: `Name ${participantId}`,
    location: "Somewhere",
    pin,
    hasMukana: false,
    role: "panelist",
    personKey: resolvePersonKey({ participantId, rawName })
  };
}

/** Slots 1-3, with only slot 3 occupied — so "slot 3" resolves to a real seat and every other number does not. */
const SEATED_SLOTS: readonly Slot[] = [
  { slot: 1, panelist: null },
  { slot: 2, panelist: null },
  { slot: 3, panelist: panelist("p3", "1003") }
];

describe("LookController.select", () => {
  it("rejects an unknown look id", () => {
    expect(() => controller().select("nope")).toThrow(/nope/);
  });

  it("clears manual box assignments when switching to a different look", () => {
    const c = controller();
    c.select("teatime");
    c.assignBox(1, 3);
    expect(c.manualBoxes()).toEqual({ 1: 3 });
    c.select("banter");
    expect(c.manualBoxes()).toEqual({});
  });

  /**
   * Mutation target 1 (task-2 brief): making `select` clear manual boxes on
   * a same-look re-select must red this test. An idempotent re-select is a
   * real operator action (e.g. re-confirming a look from the UI) and must
   * never wipe work already in progress.
   */
  it("keeps manual box assignments when the same look is re-selected", () => {
    const c = controller();
    c.select("teatime");
    c.assignBox(1, 3);
    c.select("teatime");
    expect(c.manualBoxes()).toEqual({ 1: 3 });
  });

  /** Finding 4's other writer (see `PagingRefusalKind`'s doc comment): a look change must not leave a stale refusal attributed to the look it just left. */
  it("clears a recorded refusal on any select, even to a look that is also manual fill", () => {
    const c = controller();
    c.select("teatime");
    c.adjustPage(1, FIVE_PIN_QUEUE, UNAVAILABLE); // refused: fill
    expect(c.refusal()?.kind).toBe("fill");
    c.select("banter");
    expect(c.refusal()).toBeNull();
  });
});

describe("LookController.setPage", () => {
  /** Minor: `setPage` rejects a non-integer at the call site rather than letting it surface as a thrown error in a later tick. */
  it("rejects a non-integer page", () => {
    expect(() => controller().setPage(1.5)).toThrow(/integer/);
  });
});

describe("LookController.adjustPage", () => {
  it("refuses paging under manual fill instead of throwing", () => {
    const c = controller();
    c.select("banter"); // boxFill: manual
    c.adjustPage(1, FIVE_PIN_QUEUE, AVAILABLE);
    expect(c.page()).toBe(0);
    expect(c.refusal()?.kind).toBe("fill");
    expect(c.refusal()?.message).toMatch(/manual/i);
  });

  /**
   * Mutation target 2 (task-2 brief): making `adjustPage` unbounded must red
   * this test in both directions — `lookDirector.ts`'s own docs are explicit
   * that silently clamping an operator's direct "next"/"prev" is exactly the
   * silence an operator control must not produce, so an out-of-range move is
   * refused with the page left untouched, not walked past the end.
   */
  it("refuses to move past either end of the page range, leaving the page untouched", () => {
    const c = controller();
    c.select("teatime"); // pageCount 3 against FIVE_PIN_QUEUE: valid pages 0,1,2
    c.adjustPage(1, FIVE_PIN_QUEUE, AVAILABLE); // 0 -> 1
    c.adjustPage(1, FIVE_PIN_QUEUE, AVAILABLE); // 1 -> 2 (last valid page)
    c.adjustPage(1, FIVE_PIN_QUEUE, AVAILABLE); // would be 3: refuse
    expect(c.page()).toBe(2);
    expect(c.refusal()).toEqual({
      message: "paging refused: page 3 is out of range (this look has 3 page(s))",
      kind: "range"
    });

    c.setPage(0);
    c.adjustPage(-1, FIVE_PIN_QUEUE, AVAILABLE); // would be -1: refuse
    expect(c.page()).toBe(0);
    expect(c.refusal()).toEqual({
      message: "paging refused: page -1 is out of range (this look has 3 page(s))",
      kind: "range"
    });
  });
});

describe("LookController.clearStaleRefusal", () => {
  /**
   * Mutation target 3 (task-2 brief): auto-clearing a `"range"` refusal on a
   * fill change must red this test. `"range"` describes one specific
   * attempted move, not a standing condition — the ONLY fill strategy an
   * out-of-range move can even happen under is queue fill, so clearing it
   * merely because fill flipped back to queue would wipe it out on the very
   * next tick, before an operator polling on any normal cadence could ever
   * see it (`lookDirector.ts`'s own documented rationale, ported onto
   * `PagingRefusalKind`'s doc comment).
   */
  it("does NOT clear a still-valid 'range' refusal on a fill change", () => {
    const c = controller();
    c.select("teatime");
    c.adjustPage(1, FIVE_PIN_QUEUE, AVAILABLE); // 0 -> 1
    c.adjustPage(1, FIVE_PIN_QUEUE, AVAILABLE); // 1 -> 2
    c.adjustPage(1, FIVE_PIN_QUEUE, AVAILABLE); // refused: range
    expect(c.refusal()?.kind).toBe("range");

    c.clearStaleRefusal("manual");
    expect(c.refusal()?.kind).toBe("range");
    c.clearStaleRefusal("queue"); // the fill strategy that made the move possible at all
    expect(c.refusal()?.kind).toBe("range");
  });
});

describe("LookController.assignBox", () => {
  /** Minor: `assignBox` rejects a box number outside the active look's range instead of silently persisting garbage. */
  it("rejects a manual box assignment outside the active look's box range", () => {
    const c = controller();
    c.select("teatime"); // 2 boxes
    expect(() => c.assignBox(3, 1)).toThrow(/out of range/);
    expect(() => c.assignBox(0, 1)).toThrow();
  });
});

/**
 * The `slot` rule (Task 10 fix round 1). `assignBox` did not validate
 * `slot` at ALL before Task 10, and the fix's own stated invariant — that
 * `0` legally BLANKS a box, `GalleryDirector.assertSlot`'s rule verbatim —
 * was asserted by nothing: tightening the guard from `slot < 0` to
 * `slot < 1` left the entire suite green, because the only coverage was a
 * list of arguments expected to be REJECTED. These are the positive cases,
 * and they resolve the assignment through `resolveLook` so "blank" means
 * what it renders, not just what the map holds.
 */
describe("LookController.assignBox: the slot rule", () => {
  /** A one-box, manual-fill look resolved against a roster where slot 3 is really occupied. */
  function resolveBanterWith(manualBoxes: ManualBoxAssignments) {
    return resolveLook(BANTER, {
      queue: queueWith([]),
      slots: SEATED_SLOTS,
      page: 0,
      manualBoxes
    });
  }

  /** Non-vacuity for the two cases below: this fixture CAN render a filled box. */
  it("renders a filled box for a slot that is really occupied", () => {
    const c = controller();
    c.select("banter");
    c.assignBox(1, 3);
    expect(resolveBanterWith(c.manualBoxes()).boxes).toEqual([{ box: 1, slot: 3 }]);
  });

  it("accepts slot 0 and renders that box blank", () => {
    const c = controller();
    c.select("banter");
    expect(() => c.assignBox(1, 0)).not.toThrow();
    expect(c.manualBoxes()).toEqual({ 1: 0 });
    expect(resolveBanterWith(c.manualBoxes()).boxes).toEqual([{ box: 1, slot: null }]);
  });

  /**
   * Deliberately NO upper bound: this controller does not know the show's
   * capacity, and a too-high slot resolves safely to an empty box (the same
   * reasoning `parseProgramSource` applies to `slot:<n>`). Pinned so that
   * adding a capacity check here — which would reject a legitimate
   * assignment made before the roster grew — reds instead of shipping.
   */
  it("accepts a slot number above the current roster and resolves it to an empty box", () => {
    const c = controller();
    c.select("banter");
    expect(() => c.assignBox(1, 99)).not.toThrow();
    expect(c.manualBoxes()).toEqual({ 1: 99 });
    expect(resolveBanterWith(c.manualBoxes()).boxes).toEqual([{ box: 1, slot: null }]);
  });

  it("rejects a negative slot, which no configuration can ever mean", () => {
    const c = controller();
    c.select("banter");
    expect(() => c.assignBox(1, -1)).toThrow(/slot/);
    expect(c.manualBoxes()).toEqual({});
  });

  /**
   * A DIRECT-CALLER guard only: `coerceArg` rounds a fractional number for
   * an `int` param, so `2.5` reaches this method as `3` and can never
   * arrive through `ohg.look.box.assign`. Kept because this class is public
   * API and in-process callers do not go through that coercion.
   */
  it("rejects a fractional slot from a direct caller", () => {
    const c = controller();
    c.select("banter");
    expect(() => c.assignBox(1, 2.5)).toThrow(/slot/);
    expect(c.manualBoxes()).toEqual({});
  });
});

/**
 * `clearBox` validated NOTHING before Task 10 — `delete` on a key that
 * cannot exist is a silent no-op, so `ohg.look.box.clear 0` and
 * `ohg.look.box.clear 9` on a two-box look both answered `{kind:"ok"}` to a
 * Companion button while changing nothing. It now shares `assignBox`'s box
 * rule, so the two halves of one operator control cannot disagree about
 * what a box number is.
 */
describe("LookController.clearBox", () => {
  it("clears one assignment and leaves the rest untouched", () => {
    const c = controller();
    c.select("teatime"); // 2 boxes
    c.assignBox(1, 3);
    c.assignBox(2, 4);
    c.clearBox(1);
    expect(c.manualBoxes()).toEqual({ 2: 4 });
  });

  it("rejects a box number outside the active look's range, exactly as assignBox does", () => {
    const c = controller();
    c.select("teatime"); // 2 boxes
    expect(() => c.clearBox(3)).toThrow(/out of range/);
    expect(() => c.clearBox(0)).toThrow();
    expect(() => c.clearBox(1.5)).toThrow();
  });

  it("rejects a non-positive box number even with no look selected", () => {
    const c = controller();
    expect(() => c.clearBox(0)).toThrow(/positive integer/);
    // With no look there is no range, so a high box number is not an error.
    expect(() => c.clearBox(9)).not.toThrow();
  });
});
