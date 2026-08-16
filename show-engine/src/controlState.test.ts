import { describe, expect, it } from "vitest";
import { OHG_FIELD_TEMPLATES, projectControlFields, type ControlFieldValue } from "./controlState.js";
import type { Panelist, QueueState, Slot } from "./contracts.js";
import type { ShowSnapshot } from "./showSnapshot.js";

/**
 * The fixture rule (task-9 brief): no empty, null, or zero value anywhere it
 * can be avoided. Two occupied slots with DIFFERENT roles (host / reader),
 * one empty seat (a hole, for the holes test), a non-empty queue, a
 * non-`none` tally mode, and all three capabilities in DIFFERENT states. A
 * projection bug that drops a field is invisible against a fixture whose
 * value was already absent — this fixture is built so nothing is absent by
 * accident.
 */
function panelist(id: string, name: string, role: Panelist["role"], pin: string): Panelist {
  return {
    participantId: id,
    rawName: `${name} (${pin})`,
    online: true,
    videoOn: true,
    audioOn: true,
    handRaised: false,
    zoomRole: 0,
    displayName: name,
    location: "Nairobi",
    pin,
    hasMukana: true,
    role,
    personKey: `pin:${pin}`
  };
}

const HOST = panelist("p1", "Amaya", "host", "1001");
const READER = panelist("p2", "Kito", "reader", "1002");

const SLOTS: Slot[] = [
  { slot: 1, panelist: HOST },
  { slot: 2, panelist: READER },
  { slot: 3, panelist: null } // the hole
];

const QUEUE: QueueState = { previous: ["1003"], current: "1004", upcoming: ["1005"] };

function fixture(): ShowSnapshot {
  return {
    revision: 12,
    panelists: [HOST, READER],
    slots: SLOTS,
    gallery: [{ cell: 1, slot: 1 }],
    queue: QUEUE,
    program: {
      program: { kind: "slot", slot: 1 },
      preview: { kind: "look", lookId: "two-box" },
      activeSpeakerFollow: true,
      activeSpeakerId: "p1"
    },
    look: null,
    page: 0,
    manualBoxes: {},
    tally: {
      mode: "slot",
      onAirSlots: [1],
      onAirPins: ["1001"],
      onAirParticipantIds: ["p1"]
    },
    overlays: {
      nameplates: [],
      question: null,
      headline: null,
      headlineVisible: false
    },
    capabilities: {
      registry: { state: "available", detail: null },
      handsQueue: { state: "unavailable", detail: "hands feed unreachable" },
      questionFeed: { state: "disabled", detail: null }
    },
    health: {
      panelists: { state: "ok", consecutiveFailures: 0, detail: null },
      hands: { state: "failing", consecutiveFailures: 3, detail: "timed out" },
      question: { state: "dormant", consecutiveFailures: 0, detail: "outside show hours" }
    },
    unseated: [],
    pagingRefused: null,
    restoreWarnings: []
  };
}

describe("projectControlFields — per-slot fields", () => {
  it("publishes the name of an occupied slot", () => {
    const fields = projectControlFields(fixture());
    expect(fields["ohg/slot/1/name"]).toBe("Amaya");
    expect(fields["ohg/slot/2/name"]).toBe("Kito");
  });

  it("publishes the role of an occupied slot, distinguishing different roles", () => {
    const fields = projectControlFields(fixture());
    expect(fields["ohg/slot/1/role"]).toBe("host");
    expect(fields["ohg/slot/2/role"]).toBe("reader");
  });

  it("publishes true tally for a slot that is on air", () => {
    const fields = projectControlFields(fixture());
    expect(fields["ohg/slot/1/tally"]).toBe(true);
  });

  it("publishes false tally for an occupied slot that is not on air", () => {
    const fields = projectControlFields(fixture());
    expect(fields["ohg/slot/2/tally"]).toBe(false);
  });

  it("publishes no fields at all for an empty seat (a hole)", () => {
    const fields = projectControlFields(fixture());
    expect(fields["ohg/slot/3/name"]).toBeUndefined();
    expect(fields["ohg/slot/3/role"]).toBeUndefined();
    expect(fields["ohg/slot/3/tally"]).toBeUndefined();
    expect(Object.keys(fields).some((key) => key.startsWith("ohg/slot/3/"))).toBe(false);
  });
});

describe("projectControlFields — global fields", () => {
  it("publishes the program mode via formatProgramSource, round-trippable with the action layer", () => {
    const fields = projectControlFields(fixture());
    expect(fields["ohg/program/mode"]).toBe("slot:1");
  });

  it("publishes the current queue PIN", () => {
    const fields = projectControlFields(fixture());
    expect(fields["ohg/queue/current"]).toBe("1004");
  });

  it("publishes null for the current queue field when the queue is empty", () => {
    const snapshot = fixture();
    snapshot.queue = { previous: [], current: null, upcoming: [] };
    const fields = projectControlFields(snapshot);
    expect(fields["ohg/queue/current"]).toBeNull();
  });

  it("publishes ok for the Mukana health lamp when every endpoint is healthy", () => {
    const snapshot = fixture();
    snapshot.health = {
      panelists: { state: "ok", consecutiveFailures: 0, detail: null },
      hands: { state: "ok", consecutiveFailures: 0, detail: null },
      question: { state: "ok", consecutiveFailures: 0, detail: null }
    };
    const fields = projectControlFields(snapshot);
    expect(fields["ohg/health/mukana"]).toBe("ok");
  });

  /**
   * The load-bearing case (fix round 1 owner ruling): a single health lamp
   * must mean "is Mukana OK", not "is the panelist registry OK". The
   * panelist-registry endpoint is perfectly healthy here — only `hands` is
   * down — and the lamp must still read `failing`, because an operator
   * glancing at one indicator must never see green while the hands feed is
   * dead.
   */
  it("reads failing when only the hands endpoint is down, even though the panelist registry is ok", () => {
    const snapshot = fixture();
    snapshot.health = {
      panelists: { state: "ok", consecutiveFailures: 0, detail: null },
      hands: { state: "failing", consecutiveFailures: 4, detail: "connection refused" },
      question: { state: "ok", consecutiveFailures: 0, detail: null }
    };
    const fields = projectControlFields(snapshot);
    expect(fields["ohg/health/mukana"]).toBe("failing");
  });

  it("reads dormant when the worst endpoint is dormant and none are failing", () => {
    const snapshot = fixture();
    snapshot.health = {
      panelists: { state: "ok", consecutiveFailures: 0, detail: null },
      hands: { state: "ok", consecutiveFailures: 0, detail: null },
      question: { state: "dormant", consecutiveFailures: 0, detail: "outside show hours" }
    };
    const fields = projectControlFields(snapshot);
    expect(fields["ohg/health/mukana"]).toBe("dormant");
  });

  it("reads failing on the fixture's own mixed health (ok / failing / dormant) — worst wins over any single endpoint", () => {
    const fields = projectControlFields(fixture());
    expect(fields["ohg/health/mukana"]).toBe("failing");
  });

  it("publishes every capability's state under its own field, distinguishing all three states", () => {
    const fields = projectControlFields(fixture());
    expect(fields["ohg/capabilities/registry/state"]).toBe("available");
    expect(fields["ohg/capabilities/handsQueue/state"]).toBe("unavailable");
    expect(fields["ohg/capabilities/questionFeed/state"]).toBe("disabled");
  });
});

describe("projectControlFields — purity", () => {
  it("returns a structurally equal record for the same snapshot", () => {
    expect(projectControlFields(fixture())).toEqual(projectControlFields(fixture()));
  });

  it("does not mutate the snapshot passed in", () => {
    const snapshot = fixture();
    const before = JSON.parse(JSON.stringify(snapshot)) as unknown;
    projectControlFields(snapshot);
    expect(JSON.parse(JSON.stringify(snapshot))).toEqual(before);
  });
});

// ---------------------------------------------------------------------------
// Structural tests — templates and output must not drift. These DRIVE the
// projection (call projectControlFields and inspect its actual output)
// rather than describing it with a second hand-maintained list, because a
// prior task in this plan found that a hand-maintained list of names can
// verify names but cannot see whether the code still handles them.
// ---------------------------------------------------------------------------

const SLOT_FIELD_TEMPLATES = OHG_FIELD_TEMPLATES.filter((template) => template.includes("{slot}"));
const GLOBAL_FIELD_TEMPLATES = OHG_FIELD_TEMPLATES.filter((template) => !template.includes("{slot}"));

/**
 * Strip a produced per-slot key back to its `{slot}` template shape, or
 * null if it isn't one. Derives the legal suffix set from
 * `SLOT_FIELD_TEMPLATES` itself (fix round 1, Minor) rather than a
 * separately hardcoded `(name|role|tally)` alternation — the old version
 * needed a second edit site for every new `{slot}` field added to both the
 * templates and `projectSlot`, and would fail the forward test for an
 * otherwise-correct implementation until that second edit landed.
 */
function templateShapeOf(key: string): string | null {
  const match = /^ohg\/slot\/(\d+)\/(.+)$/.exec(key);
  if (match === null) return null;
  const shape = `ohg/slot/{slot}/${match[2]}`;
  return SLOT_FIELD_TEMPLATES.includes(shape) ? shape : null;
}

describe("OHG_FIELD_TEMPLATES / projectControlFields coverage", () => {
  it("produces only keys that match a declared template (forward direction)", () => {
    const fields = projectControlFields(fixture());
    for (const key of Object.keys(fields)) {
      const slotShape = templateShapeOf(key);
      if (slotShape !== null) {
        expect(OHG_FIELD_TEMPLATES).toContain(slotShape);
      } else {
        expect(GLOBAL_FIELD_TEMPLATES).toContain(key);
      }
    }
  });

  it("produces a key for every occupied slot's {slot} template (reverse direction)", () => {
    const snapshot = fixture();
    const fields = projectControlFields(snapshot);
    const occupiedSlots = snapshot.slots.filter((slot) => slot.panelist !== null).map((slot) => slot.slot);

    // Drives the fixture's own occupancy data, not a re-description of the
    // projection's logic: a slot is "expected" here because the fixture
    // says it's occupied, the same fact `projectControlFields` itself reads.
    for (const slotNumber of occupiedSlots) {
      for (const template of SLOT_FIELD_TEMPLATES) {
        const key = template.replace("{slot}", String(slotNumber));
        expect(Object.prototype.hasOwnProperty.call(fields, key)).toBe(true);
      }
    }
  });

  it("produces a key for every global (non-{slot}) template", () => {
    const fields = projectControlFields(fixture());
    for (const template of GLOBAL_FIELD_TEMPLATES) {
      expect(Object.prototype.hasOwnProperty.call(fields, template)).toBe(true);
    }
  });

  it("never produces a slot field for a slot number the fixture does not occupy (holes test)", () => {
    const snapshot = fixture();
    const fields = projectControlFields(snapshot);
    const emptySlots = snapshot.slots.filter((slot) => slot.panelist === null).map((slot) => slot.slot);

    for (const slotNumber of emptySlots) {
      for (const template of SLOT_FIELD_TEMPLATES) {
        const key = template.replace("{slot}", String(slotNumber));
        expect(Object.prototype.hasOwnProperty.call(fields, key)).toBe(false);
      }
    }
  });
});

describe("ControlFieldValue", () => {
  it("only ever holds a scalar or null — every produced value is one", () => {
    const fields = projectControlFields(fixture());
    for (const value of Object.values(fields)) {
      const isScalarOrNull =
        value === null || typeof value === "string" || typeof value === "number" || typeof value === "boolean";
      expect(isScalarOrNull).toBe(true);
    }
  });

  it("accepts every legal scalar variant at the type level", () => {
    const values: ControlFieldValue[] = ["a", 1, true, false, null];
    expect(values).toHaveLength(5);
  });

  /**
   * A compile-time pin, not a runtime one: `ControlFieldValue` must stay
   * ASSIGNABLE TO this narrow scalar-or-null shape. `value`'s type here is
   * the UNNARROWED parameter type (unlike `const x: ControlFieldValue =
   * null`, which TS's control-flow analysis narrows to the literal `null`
   * regardless of the annotation, defeating the check) — so `return value`
   * genuinely asks "is the full declared union assignable to the narrow
   * one?". If a future edit widens `ControlFieldValue` to admit an object
   * or array (the exact mistake this type exists to forbid), that
   * assignment stops compiling: a `typecheck:tests` failure, not a vitest
   * failure, since nothing here runs at runtime. Mutation target (task-9
   * brief): widen `ControlFieldValue` and this function stops compiling.
   */
  it("stays no wider than string | number | boolean | null at compile time", () => {
    function widthCheck(value: ControlFieldValue): string | number | boolean | null {
      return value;
    }
    expect(widthCheck("a")).toBe("a");
  });
});
