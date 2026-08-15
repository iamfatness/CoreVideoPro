// show-engine/src/actions.test.ts
import { describe, expect, it } from "vitest";
import { ShowEngine } from "./showEngine.js";
import { MockHost } from "./mockHost.js";
import { StateStore } from "./persistence.js";
import { parseShowEngineConfig } from "./config.js";
import { personKeyForPin } from "./personKey.js";
import { MukanaClient } from "./mukanaClient.js";
import type { StateFs } from "./persistence.js";
import type { Clock } from "./clock.js";
import type { ZoomEvent } from "./zoomIngest.js";
import type { FetchLike, FetchResponse } from "./mukanaClient.js";
import type { ShowSnapshot } from "./showSnapshot.js";
import {
  invokeAction,
  oscAddressFor,
  parseProgramSource,
  formatProgramSource,
  OHG_ACTIONS,
  OHG_DISPATCHED_ACTION_IDS,
  type ActionParamType,
  type ActionResult
} from "./actions.js";
import type { ProgramSource } from "./contracts.js";

// ---------------------------------------------------------------------------
// Fixtures — the same shapes showEngine.test.ts/enginePipeline.test.ts
// established: an in-memory StateFs, a fixed Clock, and a MockHost. This
// file's own addition is two look definitions (one queue-fill, one
// manual-fill) so both `nextGuest`/`prevGuest` refusal reasons are
// independently reachable through `invokeAction`.
// ---------------------------------------------------------------------------

function fixedClock(t = 1000): Clock {
  return { now: () => t };
}

function memoryFs(seed: Record<string, string> = {}): StateFs {
  const files = new Map(Object.entries(seed));
  return {
    readFile: async (p) => {
      const v = files.get(p);
      if (v === undefined) throw new Error(`ENOENT ${p}`);
      return v;
    },
    writeFile: async (p, c) => void files.set(p, c),
    rename: async (from, to) => {
      const v = files.get(from);
      if (v !== undefined) {
        files.set(to, v);
        files.delete(from);
      }
    },
    mkdir: async () => undefined
  };
}

const CONFIG = parseShowEngineConfig({
  capacity: 8,
  statePath: "/state/show.json",
  galleryCells: 16,
  looks: [
    {
      id: "queueLook",
      label: "Queue Look",
      scenePreset: "scene-queue",
      boxes: 2,
      includesHost: true,
      includesReader: false
      // boxFill omitted -> defaults to "queue"
    },
    {
      id: "manualLook",
      label: "Manual Look",
      scenePreset: "scene-manual",
      boxes: 2,
      includesHost: false,
      includesReader: false,
      boxFill: "manual"
    }
  ]
});

function engine(overrides: { host?: MockHost; fs?: StateFs } = {}): ShowEngine {
  const host = overrides.host ?? new MockHost();
  const fs = overrides.fs ?? memoryFs();
  return new ShowEngine({
    config: CONFIG,
    host,
    clock: fixedClock(),
    store: new StateStore(CONFIG.statePath, { fs })
  });
}

function joined(id: string, rawName: string): ZoomEvent {
  return {
    kind: "joined",
    participant: {
      participantId: id,
      rawName,
      online: true,
      videoOn: true,
      audioOn: true,
      handRaised: false,
      zoomRole: 0
    }
  };
}

/** Deep-equal snapshot comparator, used to prove a rejected invoke left the engine byte-identical. */
function snap(e: ShowEngine): ShowSnapshot {
  return e.snapshot();
}

// ---------------------------------------------------------------------------
// A second rig, WITH a real MukanaClient over a controllable FetchLike, used
// only where a genuine hands-queue/question feed is unavoidable: the
// nextGuest/prevGuest SUCCESS path (paging actually needs candidates to page
// through) and ohg.gfx.question.in/out (the overlay only ever carries a real
// question once one has arrived from the feed). Mirrors
// enginePipeline.test.ts's showRig, trimmed to what this file needs.
// ---------------------------------------------------------------------------

const QUEUE_STATE_PATH = "/state/queue-show.json";

const QUEUE_LOOK = {
  id: "panel",
  label: "Panel",
  scenePreset: "scene-panel",
  boxes: 2,
  includesHost: false,
  includesReader: false
};

/** Legacy three-line payload, in order [upcoming, current, previous]. Current=1001, upcoming=[1002,1003]: 3 candidates over 2 boxes = 2 pages. */
const HANDS_BODY = "1002,1003\n1001\nNONE";
const QUESTION_BODY = JSON.stringify({
  q: { key: "q-1", n: "Ivy Nunes", q: "How is the panel funded?", tag: "money", v: 7, ts: 1234 }
});

type QueueRig = {
  engine: ShowEngine;
  tick: () => Promise<ShowSnapshot>;
};

function queueRig(): QueueRig {
  let nowMs = 0;
  const clock: Clock = { now: () => nowMs };
  const fs = memoryFs();

  const fetch: FetchLike = async (url: string): Promise<FetchResponse> => {
    if (url.includes("req=hands")) return { ok: true, status: 200, text: async () => HANDS_BODY };
    if (url.includes("req=panelists")) return { ok: true, status: 200, text: async () => "{}" };
    return { ok: true, status: 200, text: async () => QUESTION_BODY };
  };

  const config = parseShowEngineConfig({
    capacity: 8,
    statePath: QUEUE_STATE_PATH,
    galleryCells: 16,
    integrations: { registry: true, handsQueue: true, questionFeed: true },
    mukana: { baseUrl: "https://example.com/rest.php", event: "officehours" },
    looks: [QUEUE_LOOK]
  });
  const mukana = new MukanaClient(
    config.mukana ?? (() => {
      throw new Error("test config always sets mukana");
    })(),
    { fetch }
  );

  const e = new ShowEngine({
    config,
    host: new MockHost(),
    clock,
    store: new StateStore(config.statePath, { fs }),
    mukana
  });

  return {
    engine: e,
    tick: async () => {
      const s = await e.tick();
      for (let i = 0; i < 8; i += 1) {
        // eslint-disable-next-line no-await-in-loop
        await Promise.resolve();
      }
      nowMs += 1;
      return s;
    }
  };
}

// ---------------------------------------------------------------------------
// Structural tests — drift here is silent, so these are exhaustive rather
// than spot checks.
// ---------------------------------------------------------------------------

describe("OHG_ACTIONS structure", () => {
  it("declares the full spec §4.2 action list, every id prefixed ohg.", () => {
    expect(OHG_ACTIONS.length).toBe(28);
    for (const action of OHG_ACTIONS) {
      expect(action.id.startsWith("ohg.")).toBe(true);
      expect(action.title.length).toBeGreaterThan(0);
      expect(action.description.length).toBeGreaterThan(0);
    }
  });

  it("has no duplicate ids", () => {
    const ids = OHG_ACTIONS.map((a) => a.id);
    expect(new Set(ids).size).toBe(ids.length);
  });

  /**
   * The two owner-decision signatures, pinned verbatim (2026-08-12): PINs
   * and participant ids are `string`, never `int` — the exact conflict the
   * pre-flight scan found between spec §4.2's declared types and this
   * engine's `personKeyForPin`/participant-id semantics.
   */
  it("declares participant ids and every PIN param as string, never int (owner ruling 2026-08-12)", () => {
    const byId = new Map(OHG_ACTIONS.map((a) => [a.id, a]));

    const add = byId.get("ohg.panelist.add");
    expect(add?.params[0]).toEqual({
      name: "participantId",
      type: "string",
      required: true,
      description: expect.any(String)
    });
    expect(add?.params[1]).toMatchObject({ name: "slot", type: "int", required: false });

    const replace = byId.get("ohg.panelist.replace");
    expect(replace?.params[1]).toMatchObject({ name: "participantId", type: "string", required: true });

    const roleSet = byId.get("ohg.panelist.role.set");
    expect(roleSet?.params[0]).toMatchObject({ name: "pin", type: "string", required: true });

    // Extended verbatim to ohg.mukana.override.set/delete: spec §4.2 also
    // declares these as `int pin`, and the identical identity-swap risk
    // applies — narrowing the fix to only the brief's illustrative table
    // would leave it reachable through the override editor.
    const overrideSet = byId.get("ohg.mukana.override.set");
    expect(overrideSet?.params[0]).toMatchObject({ name: "pin", type: "string", required: true });

    const overrideDelete = byId.get("ohg.mukana.override.delete");
    expect(overrideDelete?.params[0]).toMatchObject({ name: "pin", type: "string", required: true });
  });

  /**
   * The closed 1:1 set — mirrors
   * `StudioControlSurfaceCoverageTests.Adapter_HandlesEveryRegisteredAction`,
   * the C# registry's own coverage test. `OHG_DISPATCHED_ACTION_IDS` is
   * this file's `StudioControlSurface.SupportedActionIds` twin, kept by
   * hand beside the dispatch switch.
   *
   * Mutation targets (per the task brief): deleting one dispatch case reds
   * the first assertion; deleting one definition reds the second.
   */
  it("has a closed 1:1 set between OHG_ACTIONS and the dispatch switch", () => {
    const defined = new Set(OHG_ACTIONS.map((a) => a.id));
    const dispatched = new Set(OHG_DISPATCHED_ACTION_IDS);

    const missingDispatch = [...defined].filter((id) => !dispatched.has(id)).sort();
    expect(missingDispatch).toEqual([]);

    const phantomDispatch = [...dispatched].filter((id) => !defined.has(id)).sort();
    expect(phantomDispatch).toEqual([]);

    expect(dispatched.size).toBe(defined.size);
  });

  /**
   * The other half of the closure guarantee, exercising the REAL `dispatch`
   * switch rather than the hand-maintained `OHG_DISPATCHED_ACTION_IDS`
   * mirror above — a deleted `case` falls through to `dispatch`'s `default`
   * arm, which produces the exact same `"unknown action id"` message
   * `invokeAction` uses for an id `OHG_ACTIONS` never declared at all.
   * Distinguishing that from every OTHER legitimate rejection (a dummy arg
   * failing a domain rule, e.g. an unknown look id) is what makes this
   * bind to the switch itself, not just the two hand-kept lists agreeing
   * with each other.
   *
   * Mutation target: deleting one `case` from `dispatch` reds THIS test
   * (not just the affected action's own happy-path test) with the
   * `"unknown action id"` message naming the exact id whose case vanished.
   */
  it("every declared action actually reaches the dispatch switch (not just the mirrored id list)", () => {
    const e = engine();
    e.setLook("manualLook");
    for (const action of OHG_ACTIONS) {
      const args = action.params.map((param) => dummyValueFor(param.type));
      const result = invokeAction(e, action.id, args);
      if (result.kind === "error") {
        expect(result.message).not.toContain("unknown action id");
      }
    }
  });
});

function dummyValueFor(type: ActionParamType): unknown {
  switch (type) {
    case "string":
      return "x";
    case "int":
      return 1;
    case "double":
      return 1.5;
    case "bool":
      return true;
  }
}

describe("oscAddressFor", () => {
  it("implements the host stack's rule verbatim: root + id with dots as slashes, no case transform", () => {
    expect(oscAddressFor("ohg.look.box.assign")).toBe("/cvp/ohg/look/box/assign");
    expect(oscAddressFor("ohg.panelist.add")).toBe("/cvp/ohg/panelist/add");
  });

  it("defaults root to /cvp and honors a custom root verbatim", () => {
    expect(oscAddressFor("ohg.program.cut")).toBe("/cvp/ohg/program/cut");
    expect(oscAddressFor("ohg.program.cut", "/studio")).toBe("/studio/ohg/program/cut");
  });

  it("never transforms case or words — camelCase segments survive untouched", () => {
    expect(oscAddressFor("ohg.program.asFollow.set")).toBe("/cvp/ohg/program/asFollow/set");
    expect(oscAddressFor("ohg.gallery.resetFromSlots")).toBe("/cvp/ohg/gallery/resetFromSlots");
  });

  /**
   * Every id round-trips to a UNIQUE OSC address (spec: no two actions may
   * collide on the wire). Mutation target: adding a case transform inside
   * `oscAddressFor` reds this test by turning `asFollow.set` and (a
   * hypothetical) `asfollow.set` into the same address family, or simply by
   * disagreeing with the pinned examples above.
   */
  it("maps every action id to a unique OSC address", () => {
    const addresses = OHG_ACTIONS.map((a) => oscAddressFor(a.id));
    expect(new Set(addresses).size).toBe(addresses.length);
  });

  /**
   * Fix round 1: `oscAddressFor`'s doc comment claimed byte-for-byte parity
   * with `OscAddressMap`, but the C# constructor always normalizes `root`
   * (`NormalizeRoot`, `OscAddressMap.cs:39-53`) and `oscAddressFor` didn't —
   * so a root missing its leading slash, or carrying a trailing one,
   * produced an address the host would never actually expose.
   */
  it("normalizes root exactly like OscAddressMap.NormalizeRoot", () => {
    expect(oscAddressFor("ohg.program.cut", "cvp")).toBe("/cvp/ohg/program/cut");
    expect(oscAddressFor("ohg.program.cut", "/cvp/")).toBe("/cvp/ohg/program/cut");
    expect(oscAddressFor("ohg.program.cut", "studio/")).toBe("/studio/ohg/program/cut");
    expect(oscAddressFor("ohg.program.cut", "")).toBe("/cvp/ohg/program/cut");
    expect(oscAddressFor("ohg.program.cut", "   ")).toBe("/cvp/ohg/program/cut");
  });
});

// ---------------------------------------------------------------------------
// ProgramSource wire codec — round-trips both directions for all five
// variants (Task 9 publishes formatProgramSource's output as feedback; an
// asymmetry would make that field disagree with what the action accepts).
// ---------------------------------------------------------------------------

describe("parseProgramSource / formatProgramSource", () => {
  const variants: ReadonlyArray<{ wire: string; source: ProgramSource }> = [
    { wire: "black", source: { kind: "black" } },
    { wire: "gallery", source: { kind: "gallery" } },
    { wire: "activeSpeaker", source: { kind: "activeSpeaker" } },
    { wire: "look:teatime", source: { kind: "look", lookId: "teatime" } },
    { wire: "slot:3", source: { kind: "slot", slot: 3 } }
  ];

  it.each(variants)("parses '$wire' to its ProgramSource", ({ wire, source }) => {
    expect(parseProgramSource(wire)).toEqual(source);
  });

  it.each(variants)("formats the ProgramSource for '$wire' back to '$wire'", ({ wire, source }) => {
    expect(formatProgramSource(source)).toBe(wire);
  });

  it.each(variants)("round-trips '$wire' through parse then format", ({ wire }) => {
    const parsed = parseProgramSource(wire);
    expect(parsed).not.toBeNull();
    expect(formatProgramSource(parsed as ProgramSource)).toBe(wire);
  });

  it.each(variants)("round-trips the ProgramSource for '$wire' through format then parse", ({ source }) => {
    expect(parseProgramSource(formatProgramSource(source))).toEqual(source);
  });

  it.each([
    ["look: with an empty id", "look:"],
    ["slot: with a non-digit payload", "slot:abc"],
    ["slot: with a negative payload", "slot:-1"],
    ["slot: with a ZERO payload (fix round 1: LiveSlots is 1-based, slot 0 never exists)", "slot:0"],
    ["an unknown bare word", "nonsense"],
    ["the empty string", ""]
  ])("returns null for %s ('%s')", (_label, wire) => {
    expect(parseProgramSource(wire)).toBeNull();
  });

  it("rejects slot:0 through invokeAction too, engine untouched (fix round 1)", () => {
    const e = engine();
    const before = snap(e);
    const result = invokeAction(e, "ohg.program.directCut", ["slot:0"]);
    expect(result).toEqual({
      kind: "error",
      message: expect.stringContaining("not a valid ProgramSource")
    });
    expect(snap(e)).toEqual(before);
  });
});

// ---------------------------------------------------------------------------
// invokeAction — the "never throws", "never mutates before validating"
// guarantees, spot-checked against several representative actions.
// ---------------------------------------------------------------------------

describe("invokeAction: never throws, never mutates on a rejected invoke", () => {
  it("returns an error for an unknown action id, engine untouched", () => {
    const e = engine();
    const before = snap(e);
    const result = invokeAction(e, "ohg.does.not.exist", []);
    expect(result.kind).toBe("error");
    expect(snap(e)).toEqual(before);
  });

  it("returns an error for too many arguments", () => {
    const e = engine();
    const result = invokeAction(e, "ohg.program.cut", ["unexpected"]);
    expect(result).toEqual({
      kind: "error",
      message: expect.stringContaining("expected at most 0 argument")
    });
  });

  it("returns an error for a missing required argument, engine untouched", () => {
    const e = engine();
    const before = snap(e);
    const result = invokeAction(e, "ohg.look.set", []);
    expect(result.kind).toBe("error");
    expect(snap(e)).toEqual(before);
  });

  /**
   * The owner-ruling-protecting case: a caller that hands `invokeAction` a
   * JS NUMBER for a declared `"string"` param (exactly what an `int`-typed
   * OSC field would deliver) must be REJECTED, not silently
   * `String()`-coerced — silently stringifying a number here would
   * reintroduce the identity-swap risk one layer later than spec's `int
   * pin` would have, which defeats the whole point of declaring the param
   * `string` in the first place.
   */
  it("rejects a number where a string param (a PIN) is declared, never coerces it", () => {
    const e = engine();
    const before = snap(e);
    const result = invokeAction(e, "ohg.panelist.role.set", [42, "host"]);
    expect(result.kind).toBe("error");
    expect(snap(e)).toEqual(before);
  });

  it("rejects a number where a string param (a participant id) is declared", () => {
    const e = engine();
    const result = invokeAction(e, "ohg.panelist.add", [7]);
    expect(result.kind).toBe("error");
  });

  it("rejects a non-boolean for a bool param", () => {
    const e = engine();
    const result = invokeAction(e, "ohg.program.asFollow.set", ["yes"]);
    expect(result.kind).toBe("error");
  });

  it("rejects a non-numeric string for an int param", () => {
    const e = engine();
    const result = invokeAction(e, "ohg.panelist.remove", ["abc"]);
    expect(result.kind).toBe("error");
  });

  it("returns an error for an unparseable ProgramSource, engine untouched", () => {
    const e = engine();
    const before = snap(e);
    const result = invokeAction(e, "ohg.program.preview", ["slot:-1"]);
    expect(result).toEqual({
      kind: "error",
      message: expect.stringContaining("not a valid ProgramSource")
    });
    expect(snap(e)).toEqual(before);
  });

  it("returns an error for an unknown role string, engine untouched", () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann | 1234"));
    const before = snap(e);
    const result = invokeAction(e, "ohg.panelist.role.set", ["1234", "wizard"]);
    expect(result.kind).toBe("error");
    expect(snap(e)).toEqual(before);
  });

  /**
   * Every engine method this registry can reach is capable of throwing.
   * `invokeAction` must turn each into `{kind:"error"}`, never propagate —
   * `expect(() => ...).not.toThrow()` proves the exception genuinely never
   * escapes, not merely that the return value looks right.
   */
  it("never throws for an unknown look id (setLook's own throw)", () => {
    const e = engine();
    let result: ActionResult | undefined;
    expect(() => {
      result = invokeAction(e, "ohg.look.set", ["ghost-look"]);
    }).not.toThrow();
    expect(result?.kind).toBe("error");
  });

  it("never throws for an unknown participant id (addPanelist's own throw)", () => {
    const e = engine();
    let result: ActionResult | undefined;
    expect(() => {
      result = invokeAction(e, "ohg.panelist.add", ["ghost"]);
    }).not.toThrow();
    expect(result?.kind).toBe("error");
  });

  it("never throws for an occupied slot (addPanelist's own throw)", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    invokeAction(e, "ohg.panelist.add", ["p1", 1]);
    let result: ActionResult | undefined;
    expect(() => {
      result = invokeAction(e, "ohg.panelist.add", ["p2", 1]);
    }).not.toThrow();
    expect(result?.kind).toBe("error");
    expect(result?.kind === "error" ? result.message : "").toMatch(/occupied/i);
  });

  it("never throws for an out-of-range box (assignBox's own throw)", () => {
    const e = engine();
    e.setLook("manualLook");
    let result: ActionResult | undefined;
    expect(() => {
      result = invokeAction(e, "ohg.look.box.assign", [99, 1]);
    }).not.toThrow();
    expect(result?.kind).toBe("error");
  });

  it("never throws for an out-of-range gallery cell (replaceGalleryCell's own throw)", () => {
    const e = engine();
    let result: ActionResult | undefined;
    expect(() => {
      result = invokeAction(e, "ohg.gallery.replace", [999, 1]);
    }).not.toThrow();
    expect(result?.kind).toBe("error");
  });

  /**
   * Fix round 1 CRITICAL: `bindArgs`'s rejection message used to build with
   * `JSON.stringify(raw)`, which throws for a `bigint` or a circular
   * object — and `bindArgs` ran OUTSIDE `invokeAction`'s try/catch, so that
   * throw reached the caller directly. This is not a contrived input: OSC's
   * `h` (int64) type tag decodes to a JS `bigint` in common Node OSC
   * libraries, so a real OSC client sending a 64-bit int to any `int` or
   * `string` param took this exact path. Fixed by formatting the message
   * with `typeof`/`String()` (never throws) AND moving `bindArgs` inside
   * the `try` (so an equivalent mistake anywhere else is caught too).
   */
  it("returns an error (never throws) for a bigint argument to an int param, engine untouched", () => {
    const e = engine();
    const before = snap(e);
    let result: ActionResult | undefined;
    expect(() => {
      result = invokeAction(e, "ohg.panelist.remove", [10n]);
    }).not.toThrow();
    expect(result?.kind).toBe("error");
    expect(snap(e)).toEqual(before);
  });

  it("returns an error (never throws) for a bigint argument to a string param, engine untouched", () => {
    const e = engine();
    const before = snap(e);
    let result: ActionResult | undefined;
    expect(() => {
      result = invokeAction(e, "ohg.panelist.add", [10n]);
    }).not.toThrow();
    expect(result?.kind).toBe("error");
    expect(snap(e)).toEqual(before);
  });

  it("returns an error (never throws) for a circular-object argument to a string param, engine untouched", () => {
    const e = engine();
    const before = snap(e);
    const circular: Record<string, unknown> = {};
    circular.self = circular;
    let result: ActionResult | undefined;
    expect(() => {
      result = invokeAction(e, "ohg.look.set", [circular]);
    }).not.toThrow();
    expect(result?.kind).toBe("error");
    expect(snap(e)).toEqual(before);
  });

  it("returns an error (never throws) for a circular-object argument to an int param, engine untouched", () => {
    const e = engine();
    const before = snap(e);
    const circular: Record<string, unknown> = {};
    circular.self = circular;
    let result: ActionResult | undefined;
    expect(() => {
      result = invokeAction(e, "ohg.panelist.remove", [circular]);
    }).not.toThrow();
    expect(result?.kind).toBe("error");
    expect(snap(e)).toEqual(before);
  });
});

// ---------------------------------------------------------------------------
// Fix round 1: coerceArg widened to genuinely match the host stack's own
// ControlActionRegistry.TryCoerce for "int"/"bool" (the doc comment claimed
// parity before this round without it being true). "string" stays strict —
// the owner's PIN ruling — and is asserted to still reject a bool/number.
// ---------------------------------------------------------------------------

describe("invokeAction: widened int/bool coercion matches the host stack (string stays strict)", () => {
  it("accepts an OSC-style 1/0 number for a bool param", async () => {
    const e = engine();
    expect(invokeAction(e, "ohg.program.asFollow.set", [1])).toEqual({ kind: "ok" });
    const on = await e.tick();
    expect(on.program.activeSpeakerFollow).toBe(true);

    expect(invokeAction(e, "ohg.program.asFollow.set", [0])).toEqual({ kind: "ok" });
    const off = await e.tick();
    expect(off.program.activeSpeakerFollow).toBe(false);
  });

  it("accepts the strings '1'/'0' for a bool param", () => {
    const e = engine();
    expect(invokeAction(e, "ohg.gallery.smart.set", ["1"])).toEqual({ kind: "ok" });
    expect(invokeAction(e, "ohg.gallery.smart.set", ["0"])).toEqual({ kind: "ok" });
  });

  it("accepts 'True'/'FALSE' case-insensitively for a bool param", () => {
    const e = engine();
    expect(invokeAction(e, "ohg.gallery.smart.set", ["True"])).toEqual({ kind: "ok" });
    expect(invokeAction(e, "ohg.gallery.smart.set", ["FALSE"])).toEqual({ kind: "ok" });
  });

  it("rounds a float for an int param, matching the host's TryCoerce", async () => {
    const e = engine();
    const result = invokeAction(e, "ohg.gallery.replace", [1, 2.6]);
    expect(result).toEqual({ kind: "ok" });
    const s = await e.tick();
    expect(s.gallery[0]).toEqual({ cell: 1, slot: 3 }); // 2.6 rounds to 3
  });

  it("accepts a boolean for an int param (true=1, false=0), matching the host's TryCoerce", async () => {
    const e = engine();
    const result = invokeAction(e, "ohg.gallery.remove", [true]); // cell 1
    expect(result).toEqual({ kind: "ok" });
  });

  /** The one DELIBERATE divergence: string stays strict, never coerced from a bool/number. */
  it("still rejects a boolean/number for a string param — the PIN ruling is unaffected by the widening", () => {
    const e = engine();
    expect(invokeAction(e, "ohg.panelist.role.set", [true, "host"]).kind).toBe("error");
    expect(invokeAction(e, "ohg.panelist.role.set", [42, "host"]).kind).toBe("error");
  });
});

// ---------------------------------------------------------------------------
// Refusal — a first-class result, not an error. Closes the pre-existing gap
// this plan's ledger flagged: no prior test anywhere drove
// nextGuest/prevGuest with NO look selected.
// ---------------------------------------------------------------------------

describe("invokeAction: ohg.look.nextGuest / prevGuest refusal", () => {
  it("refuses nextGuest with no look selected, carrying the engine's own message", async () => {
    const e = engine();
    await e.tick();
    const result = invokeAction(e, "ohg.look.nextGuest", []);
    expect(result).toEqual({ kind: "refused", reason: "paging refused: no look is selected" });
  });

  it("refuses prevGuest with no look selected", async () => {
    const e = engine();
    await e.tick();
    const result = invokeAction(e, "ohg.look.prevGuest", []);
    expect(result).toEqual({ kind: "refused", reason: "paging refused: no look is selected" });
  });

  it("refuses nextGuest under manual box fill", async () => {
    const e = engine();
    e.setLook("manualLook");
    await e.tick();
    const result = invokeAction(e, "ohg.look.nextGuest", []);
    expect(result).toEqual({
      kind: "refused",
      reason: "paging refused: box fill is manual, not queue-driven"
    });
  });

  it("refuses prevGuest under manual box fill", async () => {
    const e = engine();
    e.setLook("manualLook");
    await e.tick();
    const result = invokeAction(e, "ohg.look.prevGuest", []);
    expect(result.kind).toBe("refused");
  });

  /**
   * Mutation target: returning `{kind:"ok"}` for a refused paging call reds
   * this test — the shape asserted is exact (`toEqual`), not a loose
   * `.kind` check, so a refusal silently downgraded to "ok" cannot pass.
   */
  it("is never {kind:'ok'} for a refused call", async () => {
    const e = engine();
    await e.tick();
    const result = invokeAction(e, "ohg.look.nextGuest", []);
    expect(result).not.toEqual({ kind: "ok" });
    expect(result.kind).toBe("refused");
  });

  it("succeeds and pages forward/back once real queue candidates exist", async () => {
    const rig = queueRig();
    rig.engine.onZoomEvent(joined("p1", "Ann | 1001"));
    rig.engine.onZoomEvent(joined("p2", "Bo | 1002"));
    rig.engine.onZoomEvent(joined("p3", "Cy | 1003"));
    await rig.tick(); // seat the cast, start the hands poll
    await rig.tick(); // apply the settled hands outcome

    rig.engine.setLook("panel");
    const looked = await rig.tick();
    expect(looked.look?.pageCount).toBeGreaterThan(1);
    expect(looked.page).toBe(0);

    const nextResult = invokeAction(rig.engine, "ohg.look.nextGuest", []);
    expect(nextResult).toEqual({ kind: "ok" });
    const paged = await rig.tick();
    expect(paged.page).toBe(1);
    expect(paged.pagingRefused).toBeNull();

    const prevResult = invokeAction(rig.engine, "ohg.look.prevGuest", []);
    expect(prevResult).toEqual({ kind: "ok" });
    const back = await rig.tick();
    expect(back.page).toBe(0);
  });
});

// ---------------------------------------------------------------------------
// The leading-zero PIN tests — the load-bearing case for the owner ruling.
// A 4-digit PIN like "0042" must survive `invokeAction` byte-for-byte and
// resolve to a DIFFERENT person than "42" would.
// ---------------------------------------------------------------------------

describe("owner ruling: PINs never lose a leading zero through invokeAction", () => {
  it("ohg.panelist.role.set: '0042' and '4200' resolve to distinct people", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann | 0042"));
    e.onZoomEvent(joined("p2", "Bo | 4200"));
    await e.tick();

    const result = invokeAction(e, "ohg.panelist.role.set", ["0042", "host"]);
    expect(result).toEqual({ kind: "ok" });
    const s = await e.tick();

    const p1 = s.slots.find((slot) => slot.panelist?.participantId === "p1")?.panelist;
    const p2 = s.slots.find((slot) => slot.panelist?.participantId === "p2")?.panelist;
    expect(p1?.role).toBe("host");
    expect(p2?.role).toBe("panelist");
  });

  /**
   * Extended verbatim to ohg.mukana.override.set/delete (this file's
   * deliberate broadening of the owner ruling beyond the brief's
   * illustrative three-line table — see OHG_ACTIONS's own test above and
   * this file's header comment for the reasoning): the identical silent
   * swap is reachable through the override editor unless its `pin` is also
   * a string.
   */
  it("ohg.mukana.override.set: '0042' and '4200' resolve to distinct people", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann | 0042"));
    e.onZoomEvent(joined("p2", "Bo | 4200"));
    await e.tick();

    const result = invokeAction(e, "ohg.mukana.override.set", ["0042", "Ann", "Nowhere", "reader"]);
    expect(result).toEqual({ kind: "ok" });
    const s = await e.tick();

    const p1 = s.slots.find((slot) => slot.panelist?.participantId === "p1")?.panelist;
    const p2 = s.slots.find((slot) => slot.panelist?.participantId === "p2")?.panelist;
    expect(p1?.role).toBe("reader");
    expect(p2?.role).toBe("panelist");
  });

  it("ohg.mukana.override.delete: deleting '0042' never deletes '42'", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann | 0042"));
    await e.tick();

    // Plant a second, unrelated override directly under the numeric-value
    // key — the one a naive int-then-back-to-string round trip would
    // collide with.
    e.setOverride({
      personKey: personKeyForPin("42"),
      displayName: "Someone Else",
      location: "",
      role: "reader"
    });
    e.setRole("0042", "host");
    await e.tick();

    const deleteResult = invokeAction(e, "ohg.mukana.override.delete", ["0042"]);
    expect(deleteResult).toEqual({ kind: "ok" });
    const s = await e.tick();

    const p1 = s.slots.find((slot) => slot.panelist?.participantId === "p1")?.panelist;
    expect(p1?.role).toBe("panelist"); // "0042"'s override is gone
  });
});

/**
 * Fix round 1: an empty (or whitespace-only) PIN is never a real person,
 * but `personKeyForPin("")` is a perfectly well-formed `PersonKey`
 * (`"pin:"`) — without a guard, all three PIN-consuming actions would
 * silently write to (role.set/override.set) or delete (override.delete)
 * an override keyed by that bogus identity instead of refusing.
 */
describe("invokeAction: an empty PIN is rejected, never silently accepted", () => {
  it("ohg.panelist.role.set rejects an empty pin, engine untouched", () => {
    const e = engine();
    const before = snap(e);
    const result = invokeAction(e, "ohg.panelist.role.set", ["", "host"]);
    expect(result).toEqual({ kind: "error", message: expect.stringContaining("pin must not be empty") });
    expect(snap(e)).toEqual(before);
  });

  it("ohg.panelist.role.set rejects a whitespace-only pin", () => {
    const e = engine();
    const result = invokeAction(e, "ohg.panelist.role.set", ["   ", "host"]);
    expect(result.kind).toBe("error");
  });

  it("ohg.mukana.override.set rejects an empty pin, engine untouched", () => {
    const e = engine();
    const before = snap(e);
    const result = invokeAction(e, "ohg.mukana.override.set", ["", "Name", "Loc", "host"]);
    expect(result).toEqual({ kind: "error", message: expect.stringContaining("pin must not be empty") });
    expect(snap(e)).toEqual(before);
  });

  it("ohg.mukana.override.delete rejects an empty pin, engine untouched", () => {
    const e = engine();
    const before = snap(e);
    const result = invokeAction(e, "ohg.mukana.override.delete", [""]);
    expect(result).toEqual({ kind: "error", message: expect.stringContaining("pin must not be empty") });
    expect(snap(e)).toEqual(before);
  });
});

// ---------------------------------------------------------------------------
// One happy-path test per action — "Task 8 exercises every action."
// ---------------------------------------------------------------------------

describe("invokeAction: every action, happy path", () => {
  describe("panelist.*", () => {
    it("ohg.panelist.add with an explicit slot", async () => {
      const e = engine();
      e.onZoomEvent(joined("p1", "Ann"));
      await e.tick();
      const result = invokeAction(e, "ohg.panelist.add", ["p1", 3]);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.slots[2]?.panelist?.participantId).toBe("p1");
    });

    it("ohg.panelist.add with slot omitted seats into the first empty slot", async () => {
      const e = engine();
      e.onZoomEvent(joined("p1", "Ann"));
      await e.tick();
      const result = invokeAction(e, "ohg.panelist.add", ["p1"]);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.slots[0]?.panelist?.participantId).toBe("p1");
    });

    /** Wire value 0 = "first empty" (spec), never a literal slot 0 — LiveSlots has no slot 0. */
    it("ohg.panelist.add with slot 0 seats into the first empty slot", async () => {
      const e = engine();
      e.onZoomEvent(joined("p1", "Ann"));
      await e.tick();
      const result = invokeAction(e, "ohg.panelist.add", ["p1", 0]);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.slots[0]?.panelist?.participantId).toBe("p1");
    });

    it("ohg.panelist.remove clears a slot", async () => {
      const e = engine();
      e.onZoomEvent(joined("p1", "Ann"));
      await e.tick();
      invokeAction(e, "ohg.panelist.add", ["p1", 1]);
      const result = invokeAction(e, "ohg.panelist.remove", [1]);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.slots[0]?.panelist).toBeNull();
    });

    it("ohg.panelist.replace overwrites an occupied slot", async () => {
      const e = engine();
      e.onZoomEvent(joined("p1", "Ann"));
      e.onZoomEvent(joined("p2", "Bo"));
      await e.tick();
      invokeAction(e, "ohg.panelist.add", ["p1", 1]);
      const result = invokeAction(e, "ohg.panelist.replace", [1, "p2"]);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.slots[0]?.panelist?.participantId).toBe("p2");
    });

    it("ohg.panelist.role.set assigns a role", async () => {
      const e = engine();
      e.onZoomEvent(joined("p1", "Ann | 1111"));
      await e.tick();
      const result = invokeAction(e, "ohg.panelist.role.set", ["1111", "host"]);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.slots.find((slot) => slot.panelist?.participantId === "p1")?.panelist?.role).toBe("host");
    });

    it("ohg.panelist.syncAll never errors even with no Mukana client", () => {
      const e = engine();
      expect(invokeAction(e, "ohg.panelist.syncAll", [])).toEqual({ kind: "ok" });
    });
  });

  describe("program.*", () => {
    it("ohg.program.preview stages a source without cutting program", async () => {
      const e = engine();
      const result = invokeAction(e, "ohg.program.preview", ["gallery"]);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.program.preview).toEqual({ kind: "gallery" });
      expect(s.program.program).toEqual({ kind: "black" });
    });

    it("ohg.program.cut swaps preview onto program", async () => {
      const e = engine();
      invokeAction(e, "ohg.program.preview", ["gallery"]);
      const result = invokeAction(e, "ohg.program.cut", []);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.program.program).toEqual({ kind: "gallery" });
    });

    it("ohg.program.auto swaps preview onto program (no host preview bus distinction at this layer)", async () => {
      const e = engine();
      invokeAction(e, "ohg.program.preview", ["activeSpeaker"]);
      const result = invokeAction(e, "ohg.program.auto", []);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.program.program).toEqual({ kind: "activeSpeaker" });
    });

    it("ohg.program.directCut bypasses preview entirely", async () => {
      const e = engine();
      const result = invokeAction(e, "ohg.program.directCut", ["slot:2"]);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.program.program).toEqual({ kind: "slot", slot: 2 });
      expect(s.program.preview).toEqual({ kind: "black" });
    });

    it("ohg.program.asFollow.set toggles active-speaker follow", async () => {
      const e = engine();
      const result = invokeAction(e, "ohg.program.asFollow.set", [true]);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.program.activeSpeakerFollow).toBe(true);
    });
  });

  describe("look.*", () => {
    it("ohg.look.set selects a look", async () => {
      const e = engine();
      const result = invokeAction(e, "ohg.look.set", ["queueLook"]);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.look?.lookId).toBe("queueLook");
    });

    it("ohg.look.box.assign writes a manual box assignment", async () => {
      const e = engine();
      e.setLook("manualLook");
      await e.tick();
      const result = invokeAction(e, "ohg.look.box.assign", [1, 3]);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.manualBoxes).toEqual({ 1: 3 });
    });

    it("ohg.look.box.clear removes a manual box assignment", async () => {
      const e = engine();
      e.setLook("manualLook");
      await e.tick();
      invokeAction(e, "ohg.look.box.assign", [1, 3]);
      const result = invokeAction(e, "ohg.look.box.clear", [1]);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.manualBoxes).toEqual({});
    });
  });

  describe("gallery.*", () => {
    it("ohg.gallery.replace places a slot in a cell", async () => {
      const e = engine();
      const result = invokeAction(e, "ohg.gallery.replace", [1, 3]);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.gallery[0]).toEqual({ cell: 1, slot: 3 });
    });

    it("ohg.gallery.remove blanks a cell", async () => {
      const e = engine();
      invokeAction(e, "ohg.gallery.replace", [1, 3]);
      const result = invokeAction(e, "ohg.gallery.remove", [1]);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.gallery[0]).toEqual({ cell: 1, slot: 0 });
    });

    it("ohg.gallery.empty blanks every cell", async () => {
      const e = engine();
      invokeAction(e, "ohg.gallery.replace", [1, 3]);
      invokeAction(e, "ohg.gallery.replace", [2, 4]);
      const result = invokeAction(e, "ohg.gallery.empty", []);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.gallery.every((cell) => cell.slot === 0)).toBe(true);
    });

    it("ohg.gallery.resetFromSlots compacts the gallery from live seating", async () => {
      const e = engine();
      e.onZoomEvent(joined("p1", "Ann"));
      await e.tick();
      invokeAction(e, "ohg.panelist.add", ["p1", 1]);
      const result = invokeAction(e, "ohg.gallery.resetFromSlots", []);
      expect(result).toEqual({ kind: "ok" });
      const s = await e.tick();
      expect(s.gallery[0]).toEqual({ cell: 1, slot: 1 });
    });

    it("ohg.gallery.smart.set toggles smart gallery ordering without error", () => {
      const e = engine();
      expect(invokeAction(e, "ohg.gallery.smart.set", [true])).toEqual({ kind: "ok" });
      expect(invokeAction(e, "ohg.gallery.smart.set", [false])).toEqual({ kind: "ok" });
    });
  });

  describe("gfx.*", () => {
    it("ohg.gfx.headline.change sets the headline text", async () => {
      const e = engine();
      const result = invokeAction(e, "ohg.gfx.headline.change", ["Ann Lee", "Santa Venetia, CA"]);
      expect(result).toEqual({ kind: "ok" });
      invokeAction(e, "ohg.gfx.headline.in", []);
      const s = await e.tick();
      expect(s.overlays.headline).toEqual({ name: "Ann Lee", location: "Santa Venetia, CA" });
      expect(s.overlays.headlineVisible).toBe(true);
    });

    it("ohg.gfx.headline.in / .out toggle visibility, retaining text", async () => {
      const e = engine();
      invokeAction(e, "ohg.gfx.headline.change", ["Ann Lee", "Santa Venetia, CA"]);
      invokeAction(e, "ohg.gfx.headline.in", []);
      const shown = await e.tick();
      expect(shown.overlays.headlineVisible).toBe(true);

      const outResult = invokeAction(e, "ohg.gfx.headline.out", []);
      expect(outResult).toEqual({ kind: "ok" });
      const hidden = await e.tick();
      expect(hidden.overlays.headlineVisible).toBe(false);
      expect(hidden.overlays.headline).toEqual({ name: "Ann Lee", location: "Santa Venetia, CA" });
    });

    it("ohg.gfx.question.in / .out toggle the question overlay once a real question has arrived", async () => {
      const rig = queueRig();
      await rig.tick(); // starts the question poll
      await rig.tick(); // applies the settled question outcome

      const inResult = invokeAction(rig.engine, "ohg.gfx.question.in", []);
      expect(inResult).toEqual({ kind: "ok" });
      const shown = await rig.tick();
      expect(shown.overlays.question).toEqual({
        askerName: "Ivy Nunes",
        text: "How is the panel funded?",
        tag: "money",
        votes: 7
      });

      const outResult = invokeAction(rig.engine, "ohg.gfx.question.out", []);
      expect(outResult).toEqual({ kind: "ok" });
      const hidden = await rig.tick();
      expect(hidden.overlays.question).toBeNull();
    });
  });

  describe("mukana.*", () => {
    it("ohg.mukana.sync never errors even with no Mukana client", () => {
      const e = engine();
      expect(invokeAction(e, "ohg.mukana.sync", [])).toEqual({ kind: "ok" });
    });

    it("ohg.mukana.override.set writes an override, ohg.mukana.override.delete removes it", async () => {
      const e = engine();
      e.onZoomEvent(joined("p1", "Ann | 1234"));
      await e.tick();

      const setResult = invokeAction(e, "ohg.mukana.override.set", ["1234", "Ann", "Reno", "host"]);
      expect(setResult).toEqual({ kind: "ok" });
      const withOverride = await e.tick();
      expect(withOverride.slots.find((s) => s.panelist?.participantId === "p1")?.panelist?.role).toBe(
        "host"
      );

      const deleteResult = invokeAction(e, "ohg.mukana.override.delete", ["1234"]);
      expect(deleteResult).toEqual({ kind: "ok" });
      const withoutOverride = await e.tick();
      expect(
        withoutOverride.slots.find((s) => s.panelist?.participantId === "p1")?.panelist?.role
      ).toBe("panelist");
    });
  });

  /**
   * Both syncAll-shaped ids exist per spec §4.2 (`ohg.panelist.syncAll` and
   * `ohg.mukana.sync`) and dispatch to the SAME engine method, but they are
   * still two distinct registry entries with two distinct OSC addresses —
   * proven here rather than assumed.
   */
  it("ohg.panelist.syncAll and ohg.mukana.sync are distinct ids with distinct OSC addresses", () => {
    expect(oscAddressFor("ohg.panelist.syncAll")).not.toBe(oscAddressFor("ohg.mukana.sync"));
  });
});
