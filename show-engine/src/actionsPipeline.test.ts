/**
 * The composed action surface (Task 10) — the `ohg.*` registry, the engine
 * it drives, the host it emits to, and the feedback projection an operator
 * reads back, exercised together rather than one module at a time. Plus the
 * exported host conformance suite, RUN here against `MockHost` so this
 * package is the first consumer of the mechanism Plans 7-9 will run against
 * their real adapters.
 *
 * This file adds no production code beyond `conformance.ts` and the barrel
 * lines it needs. Everything it imports comes through `./index.js` on
 * purpose (the same discipline `enginePipeline.test.ts` set): a name that
 * exists in `src/` but is unreachable from the barrel is not exported as
 * far as a host adapter is concerned.
 *
 * Async discipline: `rig.tick()` drains 8 microtask turns after every
 * engine tick, and every polling scenario below goes through it. Measured
 * honestly, as in `enginePipeline.test.ts`: setting that bound to 0 does
 * not red anything in this file either (the engine's own `await
 * store.save(...)` yields enough turns for the rig's synchronous-resolve
 * `FetchLike` to settle). It stays because it is the rig discipline, not
 * because these scenarios are known to be drain-sensitive.
 *
 * The four scenarios and the invariant each must break on are stated on
 * their own `describe` blocks.
 */

import { describe, expect, it } from "vitest";
import {
  formatProgramSource,
  invokeAction,
  MockHost,
  MukanaClient,
  oscAddressFor,
  parseShowEngineConfig,
  projectControlFields,
  ShowEngine,
  StateStore,
  CONFORMANCE_CONFIG,
  HOST_CONFORMANCE_CASES,
  OHG_ACTIONS,
  OHG_FIELD_TEMPLATES,
  type ActionDefinition,
  type ActionParamType,
  type ActionResult,
  type Clock,
  type ConformanceCase,
  type ConformanceHost,
  type ControlFieldValue,
  type FetchLike,
  type FetchResponse,
  type Headline,
  type HostCall,
  type LookPlacement,
  type MukanaHealth,
  type ShowEngineConfig,
  type ShowSnapshot,
  type StateFs,
  type ZoomEvent
} from "./index.js";

// ---------------------------------------------------------------------------
// Fixtures — the shapes `showEngine.test.ts` / `enginePipeline.test.ts` /
// `actions.test.ts` established, so a reader who knows those knows this one.
// ---------------------------------------------------------------------------

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

/** The 8-turn microtask drain every scenario uses after every tick (see the file header). */
const FLUSH_TURNS = 8;

async function flush(): Promise<void> {
  for (let i = 0; i < FLUSH_TURNS; i += 1) {
    // eslint-disable-next-line no-await-in-loop
    await Promise.resolve();
  }
}

const STATE_PATH = "/state/actions-pipeline.json";

/** Five panelists in the OHG "Name | PIN | Location" convention; ids sort to seating order p1..p5. */
const CAST: ReadonlyArray<{ id: string; rawName: string; pin: string }> = [
  { id: "p1", rawName: "Ann Reed | 1001 | Oslo", pin: "1001" },
  { id: "p2", rawName: "Bo Vance | 1002 | Reno", pin: "1002" },
  { id: "p3", rawName: "Cy Diaz | 1003 | Lima", pin: "1003" },
  { id: "p4", rawName: "Dee Ekko | 1004 | Perth", pin: "1004" },
  { id: "p5", rawName: "Eli Fox | 1005 | Cairo", pin: "1005" }
];

const PANELISTS_BODY = JSON.stringify(
  Object.fromEntries(
    CAST.map(({ rawName, pin }) => [
      pin,
      {
        displayName: rawName.split("|")[0]?.trim() ?? "",
        loc: rawName.split("|")[2]?.trim() ?? "",
        pin: Number(pin),
        role: "panelist",
        online: true
      }
    ])
  )
);

/**
 * The legacy three-line hands payload: upcoming, current, previous.
 * Current 1003 plus upcoming 1004/1005 = three candidates over two boxes,
 * which is what makes `ohg.look.nextGuest` have somewhere to go AND
 * `ohg.look.prevGuest` somewhere to come back from.
 */
const HANDS_BODY = "1004,1005\n1003\nNONE";

const QUESTION_BODY = JSON.stringify({
  q: { key: "q-1", n: "Ivy Nunes", q: "How is the panel funded?", tag: "money", v: 7, ts: 1234 }
});

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

const QUEUE_LOOK = {
  id: "panel",
  label: "Panel",
  scenePreset: "scene-panel",
  boxes: 2,
  includesHost: true,
  includesReader: true
  // boxFill omitted -> "queue"
};

const MANUAL_LOOK = {
  id: "manual",
  label: "Manual",
  scenePreset: "scene-manual",
  boxes: 2,
  includesHost: false,
  includesReader: false,
  boxFill: "manual"
};

function pipelineConfig(): ShowEngineConfig {
  return parseShowEngineConfig({
    capacity: 8,
    statePath: STATE_PATH,
    galleryCells: 16,
    integrations: { registry: true, handsQueue: true, questionFeed: true },
    mukana: { baseUrl: "https://example.com/rest.php", event: "officehours" },
    looks: [QUEUE_LOOK, MANUAL_LOOK]
  });
}

type ShowRig = {
  engine: ShowEngine;
  host: MockHost;
  /** One engine tick followed by the microtask drain. */
  tick: () => Promise<ShowSnapshot>;
  /** Move the injected clock forward — the only way time passes here. */
  advance: (ms: number) => void;
  /** Every URL the Mukana client actually fetched, recorded at fetch-start. */
  fetches: readonly string[];
};

function showRig(): ShowRig {
  let nowMs = 0;
  const clock: Clock = { now: () => nowMs };
  const host = new MockHost();
  const fetches: string[] = [];

  const fetch: FetchLike = async (url: string): Promise<FetchResponse> => {
    fetches.push(url);
    if (url.includes("req=hands")) return { ok: true, status: 200, text: async () => HANDS_BODY };
    if (url.includes("req=panelists")) {
      return { ok: true, status: 200, text: async () => PANELISTS_BODY };
    }
    return { ok: true, status: 200, text: async () => QUESTION_BODY };
  };

  const config = pipelineConfig();
  const mukanaConfig = config.mukana;
  if (mukanaConfig === null) throw new Error("pipelineConfig always configures mukana");

  const engine = new ShowEngine({
    config,
    host,
    clock,
    store: new StateStore(config.statePath, { fs: memoryFs() }),
    mukana: new MukanaClient(mukanaConfig, { fetch })
  });

  return {
    engine,
    host,
    fetches,
    advance: (ms: number) => {
      nowMs += ms;
    },
    tick: async () => {
      const snapshot = await engine.tick();
      await flush();
      return snapshot;
    }
  };
}

/**
 * The show every action case starts from: five seated panelists, all three
 * Mukana feeds landed, and the two chairs assigned (slot 1 host, slot 2
 * reader) so the queue has guests to page through that are not chairs.
 */
async function baseShow(rig: ShowRig): Promise<void> {
  for (const member of CAST) {
    rig.engine.onZoomEvent(joined(member.id, member.rawName));
  }
  await rig.tick(); // seats the cast, starts every poll
  await rig.tick(); // applies what settled
  rig.engine.setRole("1001", "host");
  rig.engine.setRole("1002", "reader");
  await rig.tick();
}

function roleOf(snapshot: ShowSnapshot, participantId: string): string | null {
  return snapshot.slots.find((slot) => slot.panelist?.participantId === participantId)?.panelist?.role ?? null;
}

function slotOccupant(snapshot: ShowSnapshot, slot: number): string | null {
  return snapshot.slots.find((entry) => entry.slot === slot)?.panelist?.participantId ?? null;
}

function galleryCellSlot(snapshot: ShowSnapshot, cell: number): number | null {
  return snapshot.gallery.find((entry) => entry.cell === cell)?.slot ?? null;
}

// ---------------------------------------------------------------------------
// The exported conformance suite, run against this package's own MockHost.
// ---------------------------------------------------------------------------

function conformanceEngine(host: MockHost): ShowEngine {
  return new ShowEngine({
    config: CONFORMANCE_CONFIG,
    host,
    clock: { now: () => 1000 },
    store: new StateStore(CONFORMANCE_CONFIG.statePath, { fs: memoryFs() })
  });
}

describe("HOST_CONFORMANCE_CASES", () => {
  /**
   * The suite is the deliverable, and this is its first execution. Each
   * case gets a FRESH engine + host per the contract in `conformance.ts`'s
   * header — a case that leaked state into the next would pass here and
   * fail in a host's runner, which enumerates them in its own order.
   */
  for (const conformanceCase of HOST_CONFORMANCE_CASES) {
    it(`passes against a host with a preview bus: ${conformanceCase.name}`, async () => {
      const host = new MockHost();
      await conformanceCase.run(conformanceEngine(host), host, flush);
    });

    /**
     * The same cases against a host that declares NO preview bus and half
     * the gallery cells — the degradation shape spec §4.1 requires every
     * host to be conformant under. A case that only worked for the richer
     * host would red here rather than silently never being run that way.
     */
    it(`passes against a preview-less, 8-cell host: ${conformanceCase.name}`, async () => {
      const host = new MockHost({ hasPreviewBus: false, maxGalleryCells: 8, transitions: [] });
      await conformanceCase.run(conformanceEngine(host), host, flush);
    });
  }

  it("is exported as data a foreign runner can enumerate, not as a describe block", () => {
    expect(Array.isArray(HOST_CONFORMANCE_CASES)).toBe(true);
    expect(HOST_CONFORMANCE_CASES.length).toBeGreaterThanOrEqual(6);
    for (const entry of HOST_CONFORMANCE_CASES) {
      const typed: ConformanceCase = entry;
      expect(typed.name).toBeTypeOf("string");
      expect(typed.name.length).toBeGreaterThan(0);
      expect(typed.run).toBeTypeOf("function");
    }
    expect(new Set(HOST_CONFORMANCE_CASES.map((entry) => entry.name)).size).toBe(
      HOST_CONFORMANCE_CASES.length
    );
  });

  /**
   * The cases must FAIL against a host that does the wrong thing, or they
   * are decoration. A `MockHost` whose `assignSlot` records nothing is a
   * host adapter that dropped the call on the floor — the exact defect this
   * suite exists to catch in a real bridge — and at least the slot case
   * must reject it. (Every case is run, and the count of failures asserted
   * to be non-zero, rather than pinning WHICH cases fail: that is a
   * property of this deliberately-broken host, not a contract.)
   */
  it("rejects a host that silently drops a call", async () => {
    class DroppingHost extends MockHost {
      override assignSlot(): void {
        // Dropped on the floor, exactly as a mis-wired bridge would.
      }
    }

    const failures: string[] = [];
    for (const entry of HOST_CONFORMANCE_CASES) {
      const host = new DroppingHost();
      try {
        await entry.run(conformanceEngine(host), host, flush);
      } catch (error) {
        failures.push(error instanceof Error ? error.message : String(error));
      }
    }

    expect(failures.length).toBeGreaterThan(0);
    for (const message of failures) {
      expect(message).toMatch(/^host conformance \[/);
    }
  });

  it("types its host parameter structurally, so a real adapter's recorder can be passed", () => {
    const host: ConformanceHost = new MockHost();
    expect(host.capabilities().hasPreviewBus).toBe(true);
    expect(host.calls()).toEqual([]);
  });
});

// ---------------------------------------------------------------------------
// Scenario 1 — every action drives the engine.
// ---------------------------------------------------------------------------

type DriveContext = {
  rig: ShowRig;
  result: ActionResult;
  before: ShowSnapshot;
  after: ShowSnapshot;
  calls: readonly HostCall[];
  /** `rig.fetches.length` captured the instant before the invoke. */
  fetchesBefore: number;
};

type ActionCase = {
  /** Extra setup beyond `baseShow`, run before the "before" snapshot is taken. */
  arrange?: (rig: ShowRig) => Promise<void>;
  /** A valid positional argument list for this action. */
  args: readonly unknown[];
  /** What this action PROMISES: the change it must have made, stated distinctively enough that another engine method would not satisfy it. */
  drives: (ctx: DriveContext) => void;
  /**
   * Type-correct but semantically invalid arguments this action must
   * reject. These are the only inputs that reach `dispatch` at all (arity
   * and type failures are rejected before it), so they are what makes
   * scenario 2 able to catch a case that mutates before validating.
   */
  invalidArgs?: readonly (readonly unknown[])[];
};

/** Stage a look and land it, so a case that needs a resolved look starts from one. */
async function withLook(rig: ShowRig, lookId: string): Promise<void> {
  rig.engine.setLook(lookId);
  await rig.tick();
}

/** Blank every gallery cell, so a case asserting a cell became N starts from 0. */
async function withEmptyGallery(rig: ShowRig): Promise<void> {
  rig.engine.emptyGallery();
  await rig.tick();
}

const ACTION_CASES: Record<string, ActionCase> = {
  "ohg.panelist.add": {
    // Slot 5 is vacated first: the base show seats all five, and a seat the
    // engine already filled cannot demonstrate that `add` is what filled it.
    arrange: async (rig) => {
      rig.engine.removePanelist(5);
      await rig.tick();
    },
    args: ["p5", 5],
    drives: ({ before, after }) => {
      expect(slotOccupant(before, 5)).toBeNull();
      expect(slotOccupant(after, 5)).toBe("p5");
    },
    // Slot 1 is occupied (`add` refuses rather than overwriting), and "p9"
    // is nobody this show has published.
    invalidArgs: [
      ["p5", 1],
      ["p9", 6],
      ["p5", -1]
    ]
  },
  "ohg.panelist.remove": {
    args: [4],
    drives: ({ before, after }) => {
      expect(slotOccupant(before, 4)).toBe("p4");
      expect(slotOccupant(after, 4)).toBeNull();
      // Only slot 4 moved — a `remove` wired to a rebuild would clear more.
      expect(slotOccupant(after, 3)).toBe("p3");
      expect(slotOccupant(after, 5)).toBe("p5");
    },
    invalidArgs: [[0], [99]]
  },
  "ohg.panelist.replace": {
    args: [1, "p3"],
    drives: ({ before, after }) => {
      expect(slotOccupant(before, 1)).toBe("p1");
      expect(slotOccupant(after, 1)).toBe("p3");
    },
    invalidArgs: [
      [99, "p3"],
      [1, "p9"]
    ]
  },
  "ohg.panelist.role.set": {
    args: ["1003", "aslinterpreter"],
    drives: ({ before, after }) => {
      expect(roleOf(before, "p3")).toBe("panelist");
      expect(roleOf(after, "p3")).toBe("aslinterpreter");
      // The chairs the base show assigned are untouched: `role.set` writes
      // one person's role, it does not re-derive the room.
      expect(roleOf(after, "p1")).toBe("host");
      expect(roleOf(after, "p2")).toBe("reader");
    },
    invalidArgs: [
      ["1003", "not-a-role"],
      ["", "host"],
      ["   ", "host"]
    ]
  },
  "ohg.panelist.syncAll": {
    // Prove the show is QUIET first: without a forced sync, a tick at the
    // same clock instant polls nothing, so the fetches counted below are
    // this action's doing and nothing else's.
    arrange: async (rig) => {
      const before = rig.fetches.length;
      await rig.tick();
      expect(rig.fetches.length).toBe(before);
    },
    args: [],
    drives: ({ rig, fetchesBefore }) => {
      expect(rig.fetches.length - fetchesBefore).toBe(3);
    }
  },
  "ohg.program.preview": {
    args: ["gallery"],
    drives: ({ before, after, calls }) => {
      expect(before.program.preview).not.toEqual({ kind: "gallery" });
      expect(after.program.preview).toEqual({ kind: "gallery" });
      // Staging is not cutting.
      expect(after.program.program).toEqual(before.program.program);
      expect(calls.filter((call) => call.kind === "setPreview")).toEqual([
        { kind: "setPreview", source: { kind: "gallery" } }
      ]);
      expect(calls.some((call) => call.kind === "cut" || call.kind === "auto")).toBe(false);
    },
    invalidArgs: [["slot:0"], ["look:"], ["nonsense"]]
  },
  "ohg.program.cut": {
    arrange: async (rig) => {
      rig.engine.setPreview({ kind: "gallery" });
      await rig.tick();
    },
    args: [],
    drives: ({ before, after, calls }) => {
      expect(before.program.program).not.toEqual({ kind: "gallery" });
      expect(after.program.program).toEqual({ kind: "gallery" });
      // `cut`, not `auto` — the two differ only in which host method they
      // call, which is exactly the copy-paste this asserts against.
      expect(calls.filter((call) => call.kind === "cut")).toHaveLength(1);
      expect(calls.some((call) => call.kind === "auto")).toBe(false);
    }
  },
  "ohg.program.auto": {
    arrange: async (rig) => {
      rig.engine.setPreview({ kind: "slot", slot: 3 });
      await rig.tick();
    },
    args: [],
    drives: ({ before, after, calls }) => {
      expect(before.program.program).not.toEqual({ kind: "slot", slot: 3 });
      expect(after.program.program).toEqual({ kind: "slot", slot: 3 });
      expect(calls.filter((call) => call.kind === "auto")).toHaveLength(1);
      expect(calls.some((call) => call.kind === "cut")).toBe(false);
    }
  },
  "ohg.program.directCut": {
    args: ["slot:2"],
    drives: ({ before, after, calls }) => {
      expect(after.program.program).toEqual({ kind: "slot", slot: 2 });
      // Bypassing preview means preview did NOT move and no transport
      // command was sent — the whole difference from `cut`.
      expect(after.program.preview).toEqual(before.program.preview);
      expect(
        calls.some((call) => call.kind === "cut" || call.kind === "auto" || call.kind === "setPreview")
      ).toBe(false);
    },
    invalidArgs: [["slot:0"], ["slot:-1"], ["look:"], ["gallery "]]
  },
  "ohg.program.asFollow.set": {
    args: [true],
    drives: ({ before, after }) => {
      expect(before.program.activeSpeakerFollow).toBe(false);
      expect(after.program.activeSpeakerFollow).toBe(true);
    }
  },
  "ohg.look.set": {
    args: ["panel"],
    drives: ({ before, after, calls }) => {
      expect(before.look).toBeNull();
      expect(after.look?.lookId).toBe("panel");
      expect(after.look?.scenePreset).toBe("scene-panel");
      const applied = calls.filter((call): call is Extract<HostCall, { kind: "applyLook" }> =>
        call.kind === "applyLook"
      );
      expect(applied).toHaveLength(1);
      expect(applied[0]?.lookId).toBe("panel");
    },
    invalidArgs: [["no-such-look"], [""]]
  },
  "ohg.look.nextGuest": {
    arrange: async (rig) => {
      await withLook(rig, "panel");
    },
    args: [],
    drives: ({ before, after }) => {
      expect(before.page).toBe(0);
      expect(after.page).toBe(1);
      expect(after.pagingRefused).toBeNull();
      expect(after.look?.boxes).not.toEqual(before.look?.boxes);
    }
  },
  "ohg.look.prevGuest": {
    arrange: async (rig) => {
      await withLook(rig, "panel");
      rig.engine.nextGuest();
      await rig.tick();
    },
    args: [],
    drives: ({ before, after }) => {
      expect(before.page).toBe(1);
      expect(after.page).toBe(0);
      expect(after.pagingRefused).toBeNull();
    }
  },
  "ohg.look.box.assign": {
    arrange: async (rig) => {
      await withLook(rig, "manual");
    },
    args: [1, 4],
    drives: ({ before, after }) => {
      expect(before.manualBoxes[1]).toBeUndefined();
      expect(after.manualBoxes[1]).toBe(4);
      expect(after.look?.boxes[0]).toEqual({ box: 1, slot: 4 });
    },
    // `[1, 0]` is deliberately NOT here: `0` legally blanks a box, the same
    // convention a blank gallery cell uses. A negative slot never can.
    invalidArgs: [
      [9, 4],
      [0, 4],
      [1, -1]
    ]
  },
  "ohg.look.box.clear": {
    arrange: async (rig) => {
      await withLook(rig, "manual");
      rig.engine.assignBox(1, 4);
      rig.engine.assignBox(2, 5);
      await rig.tick();
    },
    args: [1],
    drives: ({ before, after }) => {
      expect(before.manualBoxes[1]).toBe(4);
      expect(after.manualBoxes[1]).toBeUndefined();
      // The other assignment survives — `clear` removes one box, not the table.
      expect(after.manualBoxes[2]).toBe(5);
    },
    invalidArgs: [[0], [9]]
  },
  "ohg.gallery.resetFromSlots": {
    arrange: withEmptyGallery,
    args: [],
    drives: ({ before, after, calls }) => {
      expect(galleryCellSlot(before, 1)).toBe(0);
      expect([1, 2, 3, 4, 5].map((cell) => galleryCellSlot(after, cell))).toEqual([1, 2, 3, 4, 5]);
      expect(calls.some((call) => call.kind === "setGallery")).toBe(true);
    }
  },
  "ohg.gallery.replace": {
    arrange: withEmptyGallery,
    args: [2, 4],
    drives: ({ before, after }) => {
      expect(galleryCellSlot(before, 2)).toBe(0);
      expect(galleryCellSlot(after, 2)).toBe(4);
      expect(galleryCellSlot(after, 1)).toBe(0);
    },
    invalidArgs: [
      [0, 4],
      [99, 4],
      [2, -1]
    ]
  },
  "ohg.gallery.remove": {
    arrange: async (rig) => {
      rig.engine.replaceGalleryCell(2, 4);
      rig.engine.replaceGalleryCell(3, 5);
      await rig.tick();
    },
    args: [2],
    drives: ({ before, after }) => {
      expect(galleryCellSlot(before, 2)).toBe(4);
      expect(galleryCellSlot(after, 2)).toBe(0);
      // Only that cell blanked.
      expect(galleryCellSlot(after, 3)).toBe(5);
    },
    invalidArgs: [[0], [99]]
  },
  "ohg.gallery.empty": {
    arrange: async (rig) => {
      rig.engine.resetGalleryFromSlots();
      await rig.tick();
    },
    args: [],
    drives: ({ before, after }) => {
      expect(before.gallery.some((cell) => cell.slot !== 0)).toBe(true);
      expect(after.gallery.every((cell) => cell.slot === 0)).toBe(true);
    }
  },
  "ohg.gallery.smart.set": {
    // Smart gallery reorders OCCUPIED cells by speaker recency, so the
    // observable needs a gallery to reorder and a recent speaker to reorder
    // it by. p5 (slot 5, gallery cell 5) speaks; with the toggle off,
    // nothing moves.
    arrange: async (rig) => {
      rig.engine.resetGalleryFromSlots();
      rig.engine.onActiveSpeaker("p5");
      await rig.tick();
      expect(galleryCellSlot(rig.engine.snapshot(), 1)).toBe(1);
    },
    args: [true],
    drives: ({ before, after }) => {
      expect(galleryCellSlot(before, 1)).toBe(1);
      expect(galleryCellSlot(after, 1)).toBe(5);
    }
  },
  "ohg.gfx.headline.in": {
    arrange: async (rig) => {
      rig.engine.setHeadline({ name: "Ann Lee", location: "Santa Venetia" });
      await rig.tick();
    },
    args: [],
    drives: ({ before, after }) => {
      expect(before.overlays.headlineVisible).toBe(false);
      expect(after.overlays.headlineVisible).toBe(true);
      // Visibility only — the text is `headline.change`'s job.
      expect(after.overlays.headline).toEqual({ name: "Ann Lee", location: "Santa Venetia" });
    }
  },
  "ohg.gfx.headline.out": {
    arrange: async (rig) => {
      rig.engine.setHeadline({ name: "Ann Lee", location: "Santa Venetia" });
      rig.engine.setHeadlineVisible(true);
      await rig.tick();
    },
    args: [],
    drives: ({ before, after }) => {
      expect(before.overlays.headlineVisible).toBe(true);
      expect(after.overlays.headlineVisible).toBe(false);
      // Hiding retains the text (spec: "out" is not "clear").
      expect(after.overlays.headline).toEqual({ name: "Ann Lee", location: "Santa Venetia" });
    }
  },
  "ohg.gfx.headline.change": {
    arrange: async (rig) => {
      rig.engine.setHeadline({ name: "Old Name", location: "Old City" });
      await rig.tick();
    },
    args: ["New Name", "New City"],
    drives: ({ before, after }) => {
      expect(before.overlays.headline).toEqual({ name: "Old Name", location: "Old City" });
      expect(after.overlays.headline).toEqual({ name: "New Name", location: "New City" });
      // Changing the text does not put it on screen.
      expect(after.overlays.headlineVisible).toBe(false);
    }
  },
  "ohg.gfx.question.in": {
    args: [],
    drives: ({ before, after, calls }) => {
      expect(before.overlays.question).toBeNull();
      expect(after.overlays.question).toEqual({
        askerName: "Ivy Nunes",
        text: "How is the panel funded?",
        tag: "money",
        votes: 7
      });
      expect(
        calls.some((call) => call.kind === "setQuestion" && call.question !== null)
      ).toBe(true);
    }
  },
  "ohg.gfx.question.out": {
    arrange: async (rig) => {
      rig.engine.setQuestionVisible(true);
      await rig.tick();
    },
    args: [],
    drives: ({ before, after }) => {
      expect(before.overlays.question).not.toBeNull();
      expect(after.overlays.question).toBeNull();
    }
  },
  "ohg.mukana.sync": {
    arrange: async (rig) => {
      const before = rig.fetches.length;
      await rig.tick();
      expect(rig.fetches.length).toBe(before);
    },
    args: [],
    drives: ({ rig, fetchesBefore }) => {
      expect(rig.fetches.length - fetchesBefore).toBe(3);
    }
  },
  "ohg.mukana.override.set": {
    args: ["1003", "Cy Diaz", "Lima", "host"],
    drives: ({ before, after }) => {
      expect(roleOf(before, "p3")).toBe("panelist");
      expect(roleOf(after, "p3")).toBe("host");
      // An exclusive role demotes the prior holder — the behavior that
      // distinguishes this from a bare override write.
      expect(roleOf(before, "p1")).toBe("host");
      expect(roleOf(after, "p1")).toBe("panelist");
    },
    invalidArgs: [
      ["1003", "Cy", "Lima", "not-a-role"],
      ["", "Cy", "Lima", "host"]
    ]
  },
  "ohg.mukana.override.delete": {
    arrange: async (rig) => {
      rig.engine.setRole("1003", "aslinterpreter");
      await rig.tick();
    },
    args: ["1003"],
    drives: ({ before, after }) => {
      expect(roleOf(before, "p3")).toBe("aslinterpreter");
      expect(roleOf(after, "p3")).toBe("panelist");
    },
    invalidArgs: [[""], ["  "]]
  }
};

/**
 * Drive one action end to end: base show, the case's own arrangement, a
 * "before" snapshot, the invoke, one tick, and the case's assertions.
 */
async function driveAction(id: string, actionCase: ActionCase): Promise<void> {
  const rig = showRig();
  await baseShow(rig);
  if (actionCase.arrange !== undefined) await actionCase.arrange(rig);

  const before = rig.engine.snapshot();
  const fetchesBefore = rig.fetches.length;
  rig.host.clear();

  const result = invokeAction(rig.engine, id, actionCase.args);
  expect(result).toEqual({ kind: "ok" });

  const after = await rig.tick();
  actionCase.drives({ rig, result, before, after, calls: rig.host.calls(), fetchesBefore });
}

describe("scenario 1: every action drives the engine", () => {
  /**
   * For every id in `OHG_ACTIONS`, invoke it with valid args and assert the
   * snapshot (or the host calls) changed the way that action PROMISES.
   *
   * The invariant this must break on: a dispatch case wired to the WRONG
   * engine method. `actions.test.ts`'s 1:1 coverage test cannot see that —
   * it only checks a case EXISTS for every id — so `ohg.program.cut`
   * calling `engine.auto()`, or `ohg.gfx.headline.in` calling
   * `setQuestionVisible`, passes there and reds here.
   *
   * The loop is over `OHG_ACTIONS` itself, not a hand-kept list: an action
   * added to the registry with no case here fails immediately (there is
   * nothing to look up), so the coverage cannot rot. The reverse direction
   * is asserted below.
   */
  for (const definition of OHG_ACTIONS) {
    it(`${definition.id} drives the engine`, async () => {
      const actionCase = ACTION_CASES[definition.id];
      expect(actionCase, `no drive case for ${definition.id}`).toBeDefined();
      if (actionCase === undefined) return;
      await driveAction(definition.id, actionCase);
    });
  }

  it("has a drive case for every registered action and no orphans", () => {
    const registered = OHG_ACTIONS.map((definition) => definition.id).sort();
    expect(Object.keys(ACTION_CASES).sort()).toEqual(registered);
  });

  /**
   * The assertions above must be per-action, not a shared "something
   * changed" check: a distinct OSC address per action means an operator
   * bound one button to one behavior, and the drive cases are what pins
   * each behavior to its own id.
   */
  it("every action reaches a distinct OSC address", () => {
    const addresses = OHG_ACTIONS.map((definition) => oscAddressFor(definition.id));
    expect(new Set(addresses).size).toBe(addresses.length);
  });
});

// ---------------------------------------------------------------------------
// Scenario 2 — a malformed invoke never throws and never mutates.
// ---------------------------------------------------------------------------

/** A value that can NEVER coerce to the given param type (see `coerceArg`). */
const WRONG_VALUE_FOR: Record<ActionParamType, unknown> = {
  // Strings are strict on purpose (the PIN ruling): a number never coerces.
  string: 42,
  int: "not-a-number",
  double: "not-a-number",
  bool: { not: "a bool" }
};

/** The last index in `params` that is required, or -1 when the action has no required params. */
function lastRequiredIndex(definition: ActionDefinition): number {
  let last = -1;
  definition.params.forEach((param, index) => {
    if (param.required) last = index;
  });
  return last;
}

type MalformedInvoke = { label: string; args: readonly unknown[] };

/** Every malformed argument list for one action: too few, too many, and one wrong type per param. */
function malformedInvokesFor(definition: ActionDefinition, validArgs: readonly unknown[]): MalformedInvoke[] {
  const invokes: MalformedInvoke[] = [{ label: "too many args", args: [...validArgs, "extra"] }];

  const required = lastRequiredIndex(definition);
  if (required >= 0) {
    invokes.push({ label: "too few args", args: validArgs.slice(0, required) });
  }

  definition.params.forEach((param, index) => {
    if (index >= validArgs.length) return;
    const args = [...validArgs];
    args[index] = WRONG_VALUE_FOR[param.type];
    invokes.push({ label: `wrong type for '${param.name}' (${param.type})`, args });
  });

  return invokes;
}

/**
 * Move the show OFF every default value before the malformed invokes run.
 *
 * This is load-bearing, and it was measured (Task 10, mutation 2): with the
 * show left at its defaults, a `dispatch` case mutated to
 * `engine.directCut(parseProgramSource(raw) ?? {kind:"black"})` BEFORE
 * validating — mutate-then-validate, the exact defect scenario 2 exists to
 * catch — was invisible, because program was ALREADY `{kind:"black"}` and
 * the bogus write wrote back the value that was there. Same shape as the
 * `DISTINCTIVE_SNAPSHOT` lesson in `enginePipeline.test.ts`: a value-based
 * guard only binds where the fixture differs from what the defect writes.
 *
 * So program, preview, the headline, and the question are all put on
 * distinctive values no plausible "fall back to a default" mutation would
 * coincide with. Applied BEFORE each case's own `arrange`, so a case that
 * needs a specific starting state (an empty gallery, a selected look) still
 * gets it.
 */
async function makeDistinctive(rig: ShowRig): Promise<void> {
  rig.engine.directCut({ kind: "slot", slot: 3 });
  rig.engine.setPreview({ kind: "slot", slot: 4 });
  rig.engine.setHeadline({ name: "Standing headline", location: "Distinctive City" });
  rig.engine.setHeadlineVisible(true);
  rig.engine.setQuestionVisible(true);
  rig.engine.resetGalleryFromSlots();
  await rig.tick();

  const snapshot = rig.engine.snapshot();
  expect(snapshot.program.program).toEqual({ kind: "slot", slot: 3 });
  expect(snapshot.program.preview).toEqual({ kind: "slot", slot: 4 });
  expect(snapshot.overlays.headline).not.toBeNull();
  expect(snapshot.overlays.question).not.toBeNull();
  expect(snapshot.gallery.some((cell) => cell.slot !== 0)).toBe(true);
}

describe("scenario 2: a malformed invoke never throws and never mutates", () => {
  /**
   * For every id: too few args, too many args, and a wrong type in each
   * position — plus, where the action has one, an argument list that is
   * type-correct and semantically invalid (an unparseable `ProgramSource`,
   * an unknown role, an empty PIN, an out-of-range slot/box/cell). That
   * last family is the one that actually REACHES `dispatch`; arity and type
   * failures are rejected before it.
   *
   * The invariant this must break on: any dispatch path that mutates before
   * validating. The snapshot is compared as a serialized string before and
   * after — byte-identical, not "close enough" — and the host must have
   * received nothing at all, which also covers the one action that emits to
   * the host synchronously on invoke (`ohg.program.preview`).
   */
  for (const definition of OHG_ACTIONS) {
    it(`${definition.id} rejects every malformed invoke without mutating`, async () => {
      const actionCase = ACTION_CASES[definition.id];
      expect(actionCase, `no drive case for ${definition.id}`).toBeDefined();
      if (actionCase === undefined) return;

      const rig = showRig();
      await baseShow(rig);
      await makeDistinctive(rig);
      if (actionCase.arrange !== undefined) await actionCase.arrange(rig);

      const invokes: MalformedInvoke[] = [
        ...malformedInvokesFor(definition, actionCase.args),
        ...(actionCase.invalidArgs ?? []).map((args, index) => ({
          label: `semantically invalid args #${index}`,
          args
        }))
      ];
      // Not vacuous: every action has at least the too-many-args case, and
      // an action with params has one per param.
      expect(invokes.length).toBeGreaterThanOrEqual(1);

      for (const invoke of invokes) {
        const beforeJson = JSON.stringify(rig.engine.snapshot());
        rig.host.clear();

        const result = invokeAction(rig.engine, definition.id, invoke.args);

        expect(result.kind, `${definition.id} / ${invoke.label}`).toBe("error");
        if (result.kind === "error") {
          expect(result.message.length).toBeGreaterThan(0);
        }
        expect(JSON.stringify(rig.engine.snapshot()), `${definition.id} / ${invoke.label}`).toBe(
          beforeJson
        );
        expect(rig.host.calls(), `${definition.id} / ${invoke.label}`).toEqual([]);
      }
    });
  }

  /**
   * The same guarantee for the shapes a hand-wired OSC client actually
   * sends when it is broken: an unknown id, a bigint (OSC's `h` tag), and a
   * circular object. These are `actions.test.ts`'s cases too, re-run here
   * against a LIVE show rather than a fresh engine — a mutation guard that
   * only holds on an empty engine proves nothing about a running one.
   */
  it("survives an unknown id, a bigint, and a circular object mid-show", async () => {
    const rig = showRig();
    await baseShow(rig);
    await makeDistinctive(rig);
    await withLook(rig, "panel");

    const circular: Record<string, unknown> = {};
    circular.self = circular;

    const malformed: ReadonlyArray<{ id: string; args: readonly unknown[] }> = [
      { id: "ohg.not.an.action", args: [] },
      { id: "ohg.panelist.add", args: [10n, 1] },
      { id: "ohg.panelist.remove", args: [10n] },
      { id: "ohg.panelist.add", args: [circular, 1] },
      { id: "ohg.gallery.replace", args: [circular, 1] }
    ];

    for (const invoke of malformed) {
      const beforeJson = JSON.stringify(rig.engine.snapshot());
      rig.host.clear();
      const result = invokeAction(rig.engine, invoke.id, invoke.args);
      expect(result.kind).toBe("error");
      expect(JSON.stringify(rig.engine.snapshot())).toBe(beforeJson);
      expect(rig.host.calls()).toEqual([]);
    }
  });
});

// ---------------------------------------------------------------------------
// Scenario 3 — a refused action reports why.
// ---------------------------------------------------------------------------

describe("scenario 3: a refused action reports why", () => {
  /**
   * The invariant this must break on: a refusal degrading into `{kind:
   * "ok"}` (the operator's "next" silently did nothing) or into a throw
   * (the bridge sees a fault where the engine merely said no). Each case
   * below asserts the exact kind, a non-empty reason carrying the engine's
   * own words, and that the page did not move.
   */
  it("refuses paging with no look selected, and says so", async () => {
    const rig = showRig();
    await baseShow(rig);

    const result = invokeAction(rig.engine, "ohg.look.nextGuest", []);
    expect(result.kind).toBe("refused");
    if (result.kind !== "refused") return;
    expect(result.reason).toMatch(/no look is selected/i);

    const after = await rig.tick();
    expect(after.page).toBe(0);
    expect(after.pagingRefused).toBe(result.reason);
  });

  it("refuses paging under manual box fill, naming the fill strategy", async () => {
    const rig = showRig();
    await baseShow(rig);
    await withLook(rig, "manual");

    for (const id of ["ohg.look.nextGuest", "ohg.look.prevGuest"]) {
      const result = invokeAction(rig.engine, id, []);
      expect(result.kind, id).toBe("refused");
      if (result.kind !== "refused") continue;
      expect(result.reason).toMatch(/box fill is manual/i);
      expect(result.reason).not.toEqual("");
    }

    const after = await rig.tick();
    expect(after.page).toBe(0);
    expect(after.pagingRefused).toMatch(/box fill is manual/i);
  });

  it("refuses paging off the end of the range, naming the range", async () => {
    const rig = showRig();
    await baseShow(rig);
    await withLook(rig, "panel");

    // Two pages of guests: page forward to the last page, then off the end.
    expect(invokeAction(rig.engine, "ohg.look.nextGuest", [])).toEqual({ kind: "ok" });
    const lastPage = await rig.tick();
    expect(lastPage.page).toBe(1);
    expect(lastPage.look?.pageCount).toBe(2);

    const result = invokeAction(rig.engine, "ohg.look.nextGuest", []);
    expect(result.kind).toBe("refused");
    if (result.kind !== "refused") return;
    expect(result.reason).toMatch(/out of range/i);

    const after = await rig.tick();
    expect(after.page).toBe(1);
    expect(after.pagingRefused).toBe(result.reason);
  });

  it("is never ok and never an error — a refusal is its own outcome", async () => {
    const rig = showRig();
    await baseShow(rig);
    await withLook(rig, "manual");

    const results: ActionResult[] = [
      invokeAction(rig.engine, "ohg.look.nextGuest", []),
      invokeAction(rig.engine, "ohg.look.prevGuest", [])
    ];
    for (const result of results) {
      expect(result.kind).not.toBe("ok");
      expect(result.kind).not.toBe("error");
      expect(result.kind).toBe("refused");
    }
  });
});

// ---------------------------------------------------------------------------
// Scenario 4 — the projection tracks the engine.
// ---------------------------------------------------------------------------

/**
 * The expected flattened fields for `snapshot`, re-derived here
 * INDEPENDENTLY of `controlState.ts` — its own formatter, its own
 * worst-of-three health rank, its own slot loop. A projection that read
 * anything other than the snapshot it was handed (a cached roster, a live
 * health getter, the previous tick's program) disagrees with this.
 */
function expectedFields(snapshot: ShowSnapshot): Record<string, ControlFieldValue> {
  const fields: Record<string, ControlFieldValue> = {};
  const onAir = new Set(snapshot.tally.onAirSlots);

  for (const slot of snapshot.slots) {
    const panelist = slot.panelist;
    if (panelist === null) continue;
    fields[`ohg/slot/${slot.slot}/name`] = panelist.displayName;
    fields[`ohg/slot/${slot.slot}/role`] = panelist.role;
    fields[`ohg/slot/${slot.slot}/tally`] = onAir.has(slot.slot);
  }

  const program = snapshot.program.program;
  fields["ohg/program/mode"] =
    program.kind === "look"
      ? `look:${program.lookId}`
      : program.kind === "slot"
        ? `slot:${program.slot}`
        : program.kind;
  fields["ohg/queue/current"] = snapshot.queue.current;

  const rank: Record<MukanaHealth["state"], number> = { ok: 0, dormant: 1, failing: 2 };
  const states = [snapshot.health.panelists, snapshot.health.hands, snapshot.health.question];
  let worst: MukanaHealth["state"] = "ok";
  for (const health of states) {
    if (rank[health.state] > rank[worst]) worst = health.state;
  }
  fields["ohg/health/mukana"] = worst;

  fields["ohg/capabilities/registry/state"] = snapshot.capabilities.registry.state;
  fields["ohg/capabilities/handsQueue/state"] = snapshot.capabilities.handsQueue.state;
  fields["ohg/capabilities/questionFeed/state"] = snapshot.capabilities.questionFeed.state;

  return fields;
}

/** Every key the projection produced must match a declared template once `{slot}` is expanded. */
function matchesATemplate(key: string): boolean {
  return OHG_FIELD_TEMPLATES.some((template) => {
    const pattern = new RegExp(`^${template.replace("{slot}", "\\d+").replace(/\//g, "\\/")}$`);
    return pattern.test(key);
  });
}

describe("scenario 4: the projection tracks the engine", () => {
  /**
   * Drive a whole show and, after EVERY tick, assert
   * `projectControlFields` agrees with the snapshot it was built from.
   *
   * The invariant this must break on: the projection reading stale state —
   * a field sourced from anywhere but its argument (a cached roster, the
   * live Mukana health getter, last tick's program). The comparison is
   * against an independent re-derivation in this file, so the two can only
   * agree by actually agreeing.
   */
  it("agrees with the snapshot it was built from, after every tick of a real show", async () => {
    const rig = showRig();
    const seen: Array<Record<string, ControlFieldValue>> = [];

    const step = async (label: string): Promise<ShowSnapshot> => {
      const snapshot = await rig.tick();
      const fields = projectControlFields(snapshot);
      expect(fields, label).toEqual(expectedFields(snapshot));
      for (const key of Object.keys(fields)) {
        expect(matchesATemplate(key), `${label}: ${key} matches no declared template`).toBe(true);
      }
      // The projection is a pure function of its argument — projecting the
      // same snapshot twice cannot disagree with itself.
      expect(projectControlFields(snapshot)).toEqual(fields);
      seen.push(fields);
      return snapshot;
    };

    for (const member of CAST) {
      rig.engine.onZoomEvent(joined(member.id, member.rawName));
    }
    await step("the cast seats");
    await step("the feeds land");

    rig.engine.setRole("1001", "host");
    rig.engine.setRole("1002", "reader");
    await step("the chairs are assigned");

    expect(invokeAction(rig.engine, "ohg.look.set", ["panel"])).toEqual({ kind: "ok" });
    await step("a look is staged");

    expect(invokeAction(rig.engine, "ohg.program.cut", [])).toEqual({ kind: "ok" });
    const onAir = await step("the look is on air");
    expect(onAir.tally.onAirSlots.length).toBeGreaterThan(0);

    expect(invokeAction(rig.engine, "ohg.look.nextGuest", [])).toEqual({ kind: "ok" });
    await step("the operator pages");

    expect(invokeAction(rig.engine, "ohg.program.asFollow.set", [true])).toEqual({ kind: "ok" });
    rig.engine.onActiveSpeaker("p5");
    await step("a guest speaks");

    expect(invokeAction(rig.engine, "ohg.panelist.remove", [4])).toEqual({ kind: "ok" });
    await step("a seat is vacated");

    // Not vacuous: the show really moved, so the per-tick agreement above
    // was asserted against changing values rather than one static map.
    const distinct = new Set(seen.map((fields) => JSON.stringify(fields)));
    expect(distinct.size).toBeGreaterThan(4);
    const last = seen[seen.length - 1] ?? {};
    expect(last["ohg/program/mode"]).toBe("activeSpeaker");
    expect(last["ohg/slot/1/role"]).toBe("host");
    expect(last["ohg/slot/4/name"]).toBeUndefined();
  });

  /**
   * The other direction of "tracks": a projection of an OLD snapshot must
   * NOT equal the projection of the current one once the show has moved. If
   * these agreed, the per-tick check above could be passing on a constant.
   */
  it("differs between two snapshots the show moved between", async () => {
    const rig = showRig();
    for (const member of CAST) {
      rig.engine.onZoomEvent(joined(member.id, member.rawName));
    }
    const seated = await rig.tick();
    await rig.tick();

    expect(invokeAction(rig.engine, "ohg.program.directCut", ["gallery"])).toEqual({ kind: "ok" });
    const cut = await rig.tick();

    expect(projectControlFields(seated)["ohg/program/mode"]).toBe("black");
    expect(projectControlFields(cut)["ohg/program/mode"]).toBe("gallery");
    expect(projectControlFields(seated)).not.toEqual(projectControlFields(cut));
    // And the wire codec both sides share round-trips the same value.
    expect(formatProgramSource(cut.program.program)).toBe(
      projectControlFields(cut)["ohg/program/mode"]
    );
  });
});

// ---------------------------------------------------------------------------
// The barrel — the names Task 10 owes, resolved through `./index.js`.
// ---------------------------------------------------------------------------

describe("package barrel: the control surface", () => {
  it("exports every runtime value a host bridge needs", () => {
    expect(OHG_ACTIONS.length).toBeGreaterThan(0);
    expect(invokeAction).toBeTypeOf("function");
    expect(oscAddressFor).toBeTypeOf("function");
    expect(projectControlFields).toBeTypeOf("function");
    expect(OHG_FIELD_TEMPLATES.length).toBeGreaterThan(0);
    expect(HOST_CONFORMANCE_CASES.length).toBeGreaterThan(0);
    expect(CONFORMANCE_CONFIG.looks.length).toBeGreaterThan(0);
  });

  /**
   * The type-only half. A type-only export cannot be asserted at runtime,
   * so each one is used in a type position and carried by
   * `npm run typecheck:tests` — a file that fails to typecheck is a failing
   * test.
   */
  it("exports every type a host bridge needs to name", () => {
    const definition: ActionDefinition | undefined = OHG_ACTIONS[0];
    const paramType: ActionParamType = "string";
    const result: ActionResult = { kind: "refused", reason: "because" };
    const value: ControlFieldValue = null;
    const headline: Headline = { name: "Ann Lee", location: "Santa Venetia" };
    const placement: LookPlacement = {
      lookId: "panel",
      scenePreset: "scene-panel",
      hostSlot: 1,
      readerSlot: 2,
      boxes: new Map<number, number | null>([[1, 3]])
    };
    const conformance: ConformanceCase | undefined = HOST_CONFORMANCE_CASES[0];
    const host: ConformanceHost = new MockHost();

    expect(definition?.id).toBeTypeOf("string");
    expect(paramType).toBe("string");
    expect(result.kind).toBe("refused");
    expect(value).toBeNull();
    expect(headline.name).toBe("Ann Lee");
    expect(placement.boxes.get(1)).toBe(3);
    expect(conformance?.name).toBeTypeOf("string");
    expect(host.calls()).toEqual([]);
  });
});
