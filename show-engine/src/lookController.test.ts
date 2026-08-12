// show-engine/src/lookController.test.ts
import { describe, expect, it } from "vitest";
import { LookController } from "./lookController.js";
import type { Capability, LookDefinition, QueueState } from "./contracts.js";

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
