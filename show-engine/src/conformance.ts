/**
 * The host conformance suite (Task 10) — spec §6's "mechanism that keeps
 * CVP-Windows, CVP-Mac, and OBS semantically identical".
 *
 * These cases are DATA, not a `describe` block: `HOST_CONFORMANCE_CASES` is
 * a plain array a host's own test runner (xUnit, XCTest, googletest driving
 * a Node child, or this package's own vitest) can enumerate and execute.
 * That is why this module lives in `src/` rather than in a `.test.ts` file
 * and why it imports NO test framework — an adapter author gets the cases
 * from `dist/index.js` like any other export, and a failed expectation is a
 * thrown `Error` with an operator-readable message, which every runner in
 * every one of those languages already knows how to report.
 *
 * **WHAT THIS SUITE DOES NOT PROVE — read this before trusting it (final
 * fix round).** Spec §6 calls it "the mechanism that keeps CVP-Windows,
 * CVP-Mac, and OBS semantically identical", and its NAME is wider than its
 * REACH. What it genuinely binds is the **engine → host CALL contract**: it
 * asserts on what a recording facade RECEIVED, and it fails a host that
 * drops a call. That is real and it is worth running. But:
 *
 * 1. **A wrong adapter can pass.** Nothing here verifies the facade
 *    forwards, or that the real implementation honours what it forwarded.
 *    An adapter that receives `scenePreset` and ignores it, or binds
 *    `hostSlot` to the wrong window, is conformant by these cases. The
 *    suite keeps the three shells RECEIVING identical instructions; it does
 *    not keep them RENDERING identically. Adapter-behaviour conformance is
 *    per-shell work each of Plans 7-9 still owes.
 * 2. **`capabilities()` is taken on trust.** A host declaring
 *    `hasPreviewBus: true` with no preview bus passes the transport case's
 *    rich branch.
 * 3. **The control surface itself has ZERO coverage here.**
 *    `projectControlFields`, `OHG_FIELD_TEMPLATES`, `oscAddressFor`, and
 *    the action manifest — the artifacts a bridge is actually built from —
 *    appear in no case, and only 9 of the 28 `ohg.*` actions are ever
 *    invoked (`panelist.role.set`, `panelist.remove`, `program.preview`,
 *    `program.cut`, `look.set`, `look.box.assign`, `look.box.clear`,
 *    `gallery.resetFromSlots`, `gallery.remove`). Those are covered by this
 *    package's own tests, not by anything a host runner executes.
 * 4. **`flush` is structurally unexercised** — see the runner contract's
 *    point 3 below.
 * 5. **`ConformanceHost` does not extend `HostAdapter`**, so nothing
 *    type-enforces that the recorder passed as `host` is the same object
 *    the engine was constructed with. Case 1 would catch a disconnected
 *    recorder; case 6 ("a tick that changes nothing sends nothing") would
 *    pass vacuously against one.
 *
 * **What a case may touch.** Each case drives the engine through public
 * actions only — `invokeAction` (the `ohg.*` registry), the handful of
 * public `ShowEngine` intake methods a host process must call anyway
 * (`onZoomEvent`, `onActiveSpeaker`, `tick`, `snapshot`) — and asserts on
 * what the `HostAdapter` RECEIVED. Nothing here reaches into engine
 * internals, so a case cannot pass because of an implementation detail an
 * adapter does not share.
 *
 * **What the runner owes each case** (the contract, since these three
 * arguments are the whole interface):
 *
 * 1. A **fresh** `ShowEngine` that has never been ticked, constructed with
 *    `CONFORMANCE_CONFIG` (below) and with `host` as its `HostAdapter`.
 *    Cases assert on first-tick emission, so a pre-ticked engine fails them
 *    for the wrong reason. **Give it an in-memory `StateFs`.**
 *    `CONFORMANCE_CONFIG.statePath` is `/conformance/show-state.json` — a
 *    path at the filesystem ROOT, deliberately not anywhere real. There is
 *    no silent default (`StateStore` requires an injected `StateFs`), but a
 *    runner that wires a real filesystem will try to `mkdir /conformance`
 *    and fail on a permission error that has nothing to do with its
 *    adapter. Nothing in this suite asserts on persistence.
 * 2. A `host` that records every call it receives. `MockHost` is that
 *    recorder for this package; a real adapter passes a thin recording
 *    facade that forwards to its own implementation and appends the same
 *    `HostCall` shapes — which is why `ConformanceHost` below is the
 *    structural subset `MockHost` satisfies rather than `MockHost` itself.
 *    The suite reads `capabilities()` and BRANCHES on it (see the transport
 *    case), so a host that declares no preview bus is conformant by a
 *    different assertion, never by a skipped one.
 * 3. A `flush` that drains pending microtasks. Every case awaits it after
 *    every `tick()` — the discipline this package adopted after a retention
 *    test passed vacuously by reading before an outcome had settled.
 *    **Currently unexercised, and say so rather than implying otherwise
 *    (fix round 1):** these cases configure no integrations, so there is
 *    nothing asynchronous in flight for a drain to wait on, and the suite
 *    passes 6/6 from `dist` with a no-op `flush`. It is in the signature
 *    because an adapter whose own emission path is async, or a future case
 *    with a live feed, will need it — not because any case here depends on
 *    it today. A runner may pass `async () => {}` and be conformant.
 *
 * **`CONFORMANCE_CONFIG` configures NO integrations on purpose.** A host
 * adapter author must be able to run this suite with no Mukana server, no
 * network, and no fixtures of their own; every case's state therefore comes
 * from operator actions and synthetic roster events. The cost is stated
 * honestly where it bites: with no hands queue the active look fills its
 * boxes MANUALLY (`effectiveBoxFill` degrades a queue look whose feed is
 * unusable), so the look case assigns its boxes explicitly, and the
 * question overlay is always `null` (nothing publishes a question without
 * the feed), so the overlay case proves change detection on the nameplate
 * half and asserts only the null-emission half for the question.
 */

import { invokeAction } from "./actions.js";
import { parseShowEngineConfig, type ShowEngineConfig } from "./config.js";
import type { MockHost } from "./mockHost.js";
import type { ShowEngine } from "./showEngine.js";
import type { ShowSnapshot } from "./showSnapshot.js";
import type { ZoomEvent } from "./zoomIngest.js";

/**
 * The recording-host surface a conformance case needs. `MockHost` satisfies
 * it structurally, so `case.run(engine, new MockHost(), flush)` typechecks
 * exactly as the brief's signature does; a Plan 7-9 adapter satisfies it
 * with a facade that records the calls it forwards to the real host. Kept
 * as a `Pick` of `MockHost` rather than a hand-written interface so the two
 * can never drift: adding a recording method to `MockHost` is the only way
 * to widen what a case may read.
 */
export type ConformanceHost = Pick<MockHost, "capabilities" | "calls" | "callsOfKind" | "clear">;

export type ConformanceCase = {
  name: string;
  /** Drive the engine, then assert against what the adapter received. */
  run(engine: ShowEngine, host: ConformanceHost, flush: () => Promise<void>): Promise<void>;
};

/**
 * The show every conformance case assumes. A runner MUST construct its
 * engine with this exact config (or one that differs only in `statePath`):
 * the cases name `CONFORMANCE_LOOK_ID`, expect capacity 8, and expect a
 * look with both chairs and two boxes.
 */
export const CONFORMANCE_LOOK_ID = "conformance.panel";

/** The scene preset `CONFORMANCE_LOOK_ID` renders through — asserted verbatim on `applyLook`. */
export const CONFORMANCE_SCENE_PRESET = "conformance-scene";

export const CONFORMANCE_CONFIG: ShowEngineConfig = parseShowEngineConfig({
  capacity: 8,
  statePath: "/conformance/show-state.json",
  galleryCells: 16,
  // No integrations, no `mukana` block: the suite must run with no network.
  integrations: { registry: false, handsQueue: false, questionFeed: false },
  looks: [
    {
      id: CONFORMANCE_LOOK_ID,
      label: "Conformance panel",
      scenePreset: CONFORMANCE_SCENE_PRESET,
      boxes: 2,
      includesHost: true,
      includesReader: true,
      // Manual fill, stated rather than degraded into: with no hands queue a
      // "queue" look would fill manually ANYWAY (`effectiveBoxFill`), and a
      // case that relied on that degradation would be asserting the
      // degradation rather than the placement.
      boxFill: "manual"
    }
  ]
});

/** The three people every case seats, in participant-id order (which is also seating order). */
const CAST: ReadonlyArray<{ participantId: string; rawName: string }> = [
  { participantId: "c1", rawName: "Cara Ames | 1001 | Oslo" },
  { participantId: "c2", rawName: "Dev Blake | 1002 | Reno" },
  { participantId: "c3", rawName: "Eve Cole | 1003 | Lima" }
];

function joinedEvent(participantId: string, rawName: string): ZoomEvent {
  return {
    kind: "joined",
    participant: {
      participantId,
      rawName,
      online: true,
      videoOn: true,
      audioOn: true,
      handRaised: false,
      zoomRole: 0
    }
  };
}

/**
 * Everything a case body is handed. `act` is the only way a case changes
 * the show: it goes through `invokeAction`, so a case can never reach an
 * engine method the `ohg.*` registry does not expose, and it asserts the
 * result is `{kind:"ok"}` so a case can never silently proceed on a
 * rejected action.
 */
type CaseContext = {
  engine: ShowEngine;
  host: ConformanceHost;
  /** One engine tick followed by the runner's microtask drain. */
  tick(): Promise<ShowSnapshot>;
  /** Invoke one `ohg.*` action and require it to succeed. */
  act(id: string, args?: readonly unknown[]): void;
  assert(condition: boolean, message: string): void;
  /**
   * Structural equality, **independent of object key order** — enough for
   * the scalar/array/record values a `HostCall` carries. See
   * `canonicalize` for why the key-order part is load-bearing.
   */
  assertEqual(actual: unknown, expected: unknown, message: string): void;
};

/**
 * Recursively sort object keys so two records with the same fields compare
 * equal regardless of the order they were WRITTEN in.
 *
 * This is not tidiness — it is the difference between the suite serving its
 * consumers and lying to them (fix round 1). `assertEqual` compares
 * serialized values, and `JSON.stringify` preserves insertion order, so a
 * Plan 7-9 recording facade that appends `{lookId, kind, …}` where
 * `MockHost` appends `{kind, lookId, …}` FAILED the look case with every
 * single field equal:
 *
 *   expected {"kind":…,"lookId":…}, got {"lookId":…,"kind":…}
 *
 * A facade over a real adapter is exactly what `ConformanceHost` was
 * widened to allow, so an order-sensitive comparison hands a spurious
 * failure to the one consumer this suite exists for. Arrays keep their
 * order (`assignSlot` emission order and gallery cell order are both real
 * assertions); only object KEYS are normalized.
 *
 * **`ancestors` tracks the PATH being walked, not every value ever seen
 * (final fix round, I2).** It started as a set that only ever grew, which
 * detects *repeated references* rather than cycles: a diamond — one object
 * reached twice by two different paths, with no cycle anywhere — came out
 * as `{"left":{"a":1},"right":"[circular]"}` while the structurally
 * identical version built from two separate literals came out in full, so
 * the two compared UNEQUAL. That is the key-order bug's exact class, biting
 * its exact victim: `ConformanceHost` is a structural `Pick` so a Plan 7-9
 * adapter can pass a recording facade, and a facade that interns or
 * memoises any repeated value (a shared empty-box tuple, one cached
 * capabilities record folded into every call) got a spurious failure whose
 * message says `[circular]` about a structure with no cycle. Removing each
 * value on the way back out means a reference is only "circular" when it is
 * genuinely an ancestor of itself.
 *
 * Exported (deliberately NOT re-exported from the package barrel, which
 * lists its names one by one) so this property has a direct regression test
 * in `conformance.test.ts`: no shipped case compares a value that contains
 * a shared sub-object, so a test driven only through `HOST_CONFORMANCE_CASES`
 * would pass either way.
 */
export function canonicalize(value: unknown, ancestors: WeakSet<object> = new WeakSet()): unknown {
  if (value === null || typeof value !== "object") return value;
  // A cycle would recurse forever; report it rather than blowing the stack.
  if (ancestors.has(value)) return "[circular]";
  ancestors.add(value);

  const canonical = canonicalizeChildren(value, ancestors);

  // Off the path again: a LATER sibling that happens to reference this same
  // object is a diamond, not a cycle, and must serialize in full.
  ancestors.delete(value);
  return canonical;
}

function canonicalizeChildren(value: object, ancestors: WeakSet<object>): unknown {
  if (Array.isArray(value)) return value.map((entry) => canonicalize(entry, ancestors));
  if (value instanceof Map) {
    return [...value.entries()].map((entry) => canonicalize(entry, ancestors));
  }

  const record = value as Record<string, unknown>;
  const sorted: Record<string, unknown> = {};
  for (const key of Object.keys(record).sort()) {
    sorted[key] = canonicalize(record[key], ancestors);
  }
  return sorted;
}

function describeValue(value: unknown): string {
  try {
    return JSON.stringify(canonicalize(value)) ?? String(value);
  } catch {
    // A circular structure (or anything else `JSON.stringify` refuses) still
    // has to produce a message rather than a second failure.
    return String(value);
  }
}

function conformanceCase(name: string, body: (ctx: CaseContext) => Promise<void>): ConformanceCase {
  return {
    name,
    run: async (engine, host, flush) => {
      const fail = (message: string): never => {
        throw new Error(`host conformance [${name}]: ${message}`);
      };
      const ctx: CaseContext = {
        engine,
        host,
        tick: async () => {
          const snapshot = await engine.tick();
          await flush();
          return snapshot;
        },
        act: (id, args = []) => {
          const result = invokeAction(engine, id, args);
          if (result.kind !== "ok") {
            fail(`${id}(${describeValue(args)}) was expected to succeed but returned ${describeValue(result)}`);
          }
        },
        assert: (condition, message) => {
          if (!condition) fail(message);
        },
        assertEqual: (actual, expected, message) => {
          if (describeValue(actual) !== describeValue(expected)) {
            fail(`${message} — expected ${describeValue(expected)}, got ${describeValue(actual)}`);
          }
        }
      };
      await body(ctx);
    }
  };
}

/** Seat the standard cast and run the first tick (which is also the first emission). */
async function seatCast(ctx: CaseContext): Promise<void> {
  for (const member of CAST) {
    ctx.engine.onZoomEvent(joinedEvent(member.participantId, member.rawName));
  }
  await ctx.tick();
}

/** Give slot 1 the host chair and slot 2 the reader chair, by PIN, through the registry's own action. */
function assignChairs(ctx: CaseContext): void {
  ctx.act("ohg.panelist.role.set", ["1001", "host"]);
  ctx.act("ohg.panelist.role.set", ["1002", "reader"]);
}

export const HOST_CONFORMANCE_CASES: readonly ConformanceCase[] = [
  /**
   * Slot binding. The engine's whole roster contract with a host is
   * `assignSlot(slot, participantId)` — one call per slot, emitted when
   * that slot's binding CHANGES and never as a full sweep. A host that
   * ignores the participant id, or that only ever sees the first tick's
   * calls, fails here.
   */
  conformanceCase("a seated participant binds its slot on the host", async (ctx) => {
    await seatCast(ctx);

    const firstPass = ctx.host.callsOfKind("assignSlot");
    ctx.assertEqual(
      firstPass.length,
      CONFORMANCE_CONFIG.capacity,
      "the first tick binds every slot, occupied or empty"
    );
    ctx.assertEqual(
      firstPass.slice(0, 3).map((call) => [call.slot, call.participantId]),
      [
        [1, "c1"],
        [2, "c2"],
        [3, "c3"]
      ],
      "the cast seats in participant-id order, slot 1 first"
    );
    ctx.assertEqual(
      [firstPass[3]?.slot, firstPass[3]?.participantId],
      [4, null],
      "an empty seat binds to null rather than being skipped"
    );

    // A vacated seat re-binds to null, and NOTHING else re-emits: the
    // engine diffs per slot, so a host is never asked to re-bind a slot
    // that did not move.
    ctx.host.clear();
    ctx.act("ohg.panelist.remove", [2]);
    await ctx.tick();

    ctx.assertEqual(
      ctx.host.callsOfKind("assignSlot").map((call) => [call.slot, call.participantId]),
      [[2, null]],
      "clearing slot 2 re-binds slot 2 alone"
    );
  }),

  /**
   * Look application. `applyLook` is ONE call carrying the whole placement
   * — the look id, the scene preset it renders through, both chairs, and
   * the guest boxes — so a host never re-derives `lookId -> scenePreset`
   * or hunts the chairs out of `setNameplates`. Every field is asserted
   * here because every field is load-bearing for a host that must swap a
   * scene and rebind five windows from this single call.
   */
  conformanceCase("selecting a look applies its preset and both chairs", async (ctx) => {
    await seatCast(ctx);
    assignChairs(ctx);
    await ctx.tick();

    ctx.host.clear();
    ctx.act("ohg.look.set", [CONFORMANCE_LOOK_ID]);
    ctx.act("ohg.look.box.assign", [1, 3]);
    await ctx.tick();

    const applied = ctx.host.callsOfKind("applyLook");
    ctx.assertEqual(applied.length, 1, "selecting a look applies it exactly once");
    ctx.assertEqual(
      applied[0],
      {
        kind: "applyLook",
        lookId: CONFORMANCE_LOOK_ID,
        scenePreset: CONFORMANCE_SCENE_PRESET,
        hostSlot: 1,
        readerSlot: 2,
        boxes: [
          [1, 3],
          [2, null]
        ]
      },
      "applyLook carries the preset, both chairs, and every box (filled or empty)"
    );

    // Re-applying the same placement says nothing: `applyLook` costs a
    // scene swap on a real host.
    ctx.host.clear();
    await ctx.tick();
    ctx.assertEqual(ctx.host.callsOfKind("applyLook").length, 0, "an unchanged placement is not re-applied");
  }),

  /**
   * Transport degradation. A host that declares `hasPreviewBus: false` must
   * never receive `setPreview`/`cut`/`auto` — it has no such concept — and
   * a cut must still put the staged source on program, by cutting straight
   * to it. This case BRANCHES on the adapter's own declaration rather than
   * assuming either shape, so it is a real assertion for both kinds of host
   * instead of a skip for one of them.
   */
  conformanceCase("transport honors the host's declared preview bus", async (ctx) => {
    await seatCast(ctx);
    ctx.host.clear();

    const hasPreviewBus = ctx.host.capabilities().hasPreviewBus;
    ctx.act("ohg.program.preview", ["gallery"]);
    ctx.act("ohg.program.cut", []);
    const snapshot = await ctx.tick();

    ctx.assertEqual(
      snapshot.program.program,
      { kind: "gallery" },
      "the staged source reaches program on every host, preview bus or not"
    );

    if (hasPreviewBus) {
      ctx.assertEqual(
        ctx.host.callsOfKind("setPreview").map((call) => call.source),
        [{ kind: "gallery" }],
        "a host with a preview bus is told what is staged"
      );
      ctx.assertEqual(ctx.host.callsOfKind("cut").length, 1, "a host with a preview bus is told to cut");
    } else {
      ctx.assertEqual(
        ctx.host.callsOfKind("setPreview").length,
        0,
        "a host with no preview bus is never told to stage anything"
      );
      ctx.assertEqual(
        ctx.host.callsOfKind("cut").length + ctx.host.callsOfKind("auto").length,
        0,
        "a host with no preview bus is never told to cut or auto"
      );
    }
  }),

  /**
   * Gallery composition. `setGallery` carries the WHOLE cell map in one
   * call (there is no per-cell host command), capped at the host's declared
   * `maxGalleryCells`, with `0` meaning a blank cell.
   */
  conformanceCase("the gallery composes from seating and respects the host's cell cap", async (ctx) => {
    await seatCast(ctx);

    const cap = Math.min(CONFORMANCE_CONFIG.galleryCells, ctx.host.capabilities().maxGalleryCells);
    ctx.host.clear();
    ctx.act("ohg.gallery.resetFromSlots", []);
    await ctx.tick();

    const emitted = ctx.host.callsOfKind("setGallery");
    ctx.assertEqual(emitted.length, 1, "composing the gallery emits exactly one whole-map call");
    const cells = emitted[0]?.cells ?? [];
    ctx.assertEqual(cells.length, cap, "the gallery never exceeds the host's declared cell count");
    ctx.assertEqual(
      cells.slice(0, 3),
      [
        [1, 1],
        [2, 2],
        [3, 3]
      ],
      "cell 1 holds the first occupied seat, compacted from seating"
    );
    ctx.assert(
      cells.slice(3).every(([, slot]) => slot === 0),
      "cells past the seated cast are blank (slot 0), never absent"
    );

    // One cell moving re-sends the whole map — the only shape the host
    // interface offers — and nothing moving sends nothing.
    ctx.host.clear();
    ctx.act("ohg.gallery.remove", [1]);
    await ctx.tick();
    ctx.assertEqual(
      ctx.host.callsOfKind("setGallery")[0]?.cells[0],
      [1, 0],
      "blanking a cell re-sends the map with that cell at 0"
    );
  }),

  /**
   * Overlay emission with change detection. Re-rendering an identical
   * lower third restarts its on-air animation, so `setNameplates`/
   * `setQuestion` must go out on a real change and stay silent otherwise.
   *
   * Honest limit: with no question feed configured (see this file's header)
   * the question is always `null`, so the question half of this case proves
   * the null emission and its silence, not a question's content. A host
   * adapter with a live feed gets the content half from the engine's own
   * `overlayDirector` tests.
   */
  conformanceCase("nameplates and question emit on change and stay silent otherwise", async (ctx) => {
    await seatCast(ctx);
    assignChairs(ctx);
    await ctx.tick();

    ctx.host.clear();
    ctx.act("ohg.look.set", [CONFORMANCE_LOOK_ID]);
    ctx.act("ohg.look.box.assign", [1, 3]);
    await ctx.tick();

    const plates = ctx.host.callsOfKind("setNameplates");
    ctx.assertEqual(plates.length, 1, "a resolved look writes its nameplates once");
    ctx.assertEqual(
      plates[0]?.plates.map((plate) => plate.name),
      ["Cara Ames", "Dev Blake", "Eve Cole"],
      "the plates name the host chair, the reader chair, and the filled box"
    );
    ctx.assertEqual(
      ctx.host.callsOfKind("setQuestion").map((call) => call.question),
      [null],
      "the question overlay is published alongside the plates, null when nothing is on screen"
    );

    ctx.host.clear();
    await ctx.tick();
    await ctx.tick();
    ctx.assertEqual(
      ctx.host.callsOfKind("setNameplates").length + ctx.host.callsOfKind("setQuestion").length,
      0,
      "unchanged overlays are never re-rendered"
    );

    // A real change — the box now holds nobody — emits again.
    ctx.act("ohg.look.box.clear", [1]);
    await ctx.tick();
    ctx.assertEqual(
      ctx.host.callsOfKind("setNameplates")[0]?.plates.map((plate) => plate.name),
      ["Cara Ames", "Dev Blake"],
      "clearing the box re-renders the plates without its occupant"
    );
  }),

  /**
   * A quiet tick says nothing. The engine ticks continuously; a host that
   * received work every tick would be rebinding inputs and re-rastering
   * overlays at tick rate, which is the churn class this repo's own shell
   * has a documented fail-fast from.
   */
  conformanceCase("a tick that changes nothing sends nothing", async (ctx) => {
    await seatCast(ctx);
    assignChairs(ctx);
    ctx.act("ohg.look.set", [CONFORMANCE_LOOK_ID]);
    ctx.act("ohg.gallery.resetFromSlots", []);
    await ctx.tick();
    await ctx.tick();

    ctx.host.clear();
    await ctx.tick();
    await ctx.tick();
    await ctx.tick();

    ctx.assertEqual(
      ctx.host.calls().map((call) => call.kind),
      [],
      "three ticks over unchanged state produce no host calls at all"
    );
  })
];
