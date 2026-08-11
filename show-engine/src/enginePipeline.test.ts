/**
 * The composed pipeline — three scenarios that drive the WHOLE engine
 * rather than one module of it, and the one outage scenario Plan 4's
 * outcomes doc recorded as owed but untestable until an orchestrator
 * existed.
 *
 * This file adds no production code. Everything it imports comes through
 * `./index.js` on purpose: Plans 6–9 build three host adapters against this
 * package's public surface, so "is the name exported" is asserted the only
 * way that actually proves anything — by resolving it through the barrel
 * and using it.
 *
 * Async discipline (the rule this package learned the hard way, Task 9 fix
 * round 1): polling is asynchronous, and a rig that does not drain
 * microtasks makes every poll-dependent assertion pass VACUOUSLY — a
 * retention test read at ticks 2 and 4 while the outcome first landed at
 * tick 5, which left the entire outcome-apply path deletable with the full
 * suite green. `tick()` below therefore drains 8 microtask turns after
 * every engine tick, the same `flush()` shape `showEngine.test.ts` uses.
 */

import { describe, expect, it } from "vitest";
import {
  buildSnapshot,
  MockHost,
  MukanaClient,
  parseShowEngineConfig,
  resolvePersonKey,
  ShowEngine,
  StateStore,
  STATE_VERSION,
  systemClock,
  type Capability,
  type Clock,
  type FetchLike,
  type FetchResponse,
  type HostAdapter,
  type HostCall,
  type HostCapabilities,
  type MukanaEndpoint,
  type MukanaHealth,
  type PersistedShowState,
  type ShowEngineConfig,
  type ShowEngineDeps,
  type ShowSnapshot,
  type StateFs,
  type ZoomEvent
} from "./index.js";

// ---------------------------------------------------------------------------
// Fixtures. Deliberately the same shapes `showEngine.test.ts` established —
// an in-memory `StateFs`, a mutable injected `Clock`, a `MockHost`, and a
// real `MukanaClient` over a controllable `FetchLike` — so a reader who
// knows that file knows this one.
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

const STATE_PATH = "/state/show.json";

/**
 * The show's one look: host chair, reader chair, two guest boxes filled
 * from the hands queue. `boxFill` is left at its `"queue"` default, which
 * is what makes the degradation story observable at all — a look that
 * declared `"manual"` would behave identically in every scenario here and
 * prove nothing.
 */
const PANEL_LOOK = {
  id: "panel",
  label: "Panel",
  scenePreset: "scene-panel",
  boxes: 2,
  includesHost: true,
  includesReader: true
};

/** Seven panelists, named in the OHG "Name | PIN | Location" convention (`identity.ts`). */
const CAST: ReadonlyArray<{ id: string; rawName: string }> = [
  { id: "p1", rawName: "Ann Reed | 1001 | Oslo" },
  { id: "p2", rawName: "Bo Vance | 1002 | Reno" },
  { id: "p3", rawName: "Cy Diaz | 1003 | Lima" },
  { id: "p4", rawName: "Dee Ekko | 1004 | Perth" },
  { id: "p5", rawName: "Eli Fox | 1005 | Cairo" },
  { id: "p6", rawName: "Fay Gunn | 1006 | Doha" },
  { id: "p7", rawName: "Gil Hale | 1007 | Ghent" }
];

const PANELISTS_BODY = JSON.stringify(
  Object.fromEntries(
    CAST.map(({ rawName }, index) => {
      const pin = 1001 + index;
      const name = rawName.split("|")[0]?.trim() ?? "";
      return [
        String(pin),
        { displayName: name, loc: `${rawName.split("|")[2]?.trim() ?? ""}, registered`, pin, role: "panelist", online: true }
      ];
    })
  )
);

/**
 * The legacy three-line hands payload: upcoming, current, previous.
 * `1001` is the HOST's pin and is in the queue on purpose — a seated chair
 * must be stripped out of the guest queue (`stripChairs`) rather than
 * double-booked into a guest box.
 */
const HANDS_BODY = "1001,1003,1004,1005,1006\n1007\nNONE";

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

type MukanaMode = "live" | "failing" | "none";

type ShowRig = {
  engine: ShowEngine;
  host: MockHost;
  fs: StateFs;
  /** Move the injected clock forward — the only way time passes in this file. */
  advance: (ms: number) => void;
  /** One engine tick followed by an 8-turn microtask drain (see the file header). */
  tick: () => Promise<ShowSnapshot>;
  /** Every Mukana endpoint starts answering 503 from here on. */
  killMukana: () => void;
  /** Every URL the client actually fetched, recorded at fetch-START time. */
  fetches: readonly string[];
};

function buildConfig(mode: MukanaMode): ShowEngineConfig {
  const integrated = mode !== "none";
  return parseShowEngineConfig({
    capacity: 8,
    statePath: STATE_PATH,
    galleryCells: 16,
    integrations: { registry: integrated, handsQueue: integrated, questionFeed: integrated },
    ...(integrated
      ? { mukana: { baseUrl: "https://example.com/rest.php", event: "officehours" } }
      : {}),
    looks: [PANEL_LOOK]
  });
}

/**
 * A whole show on a bench: engine, host, clock, state file, and (unless
 * `mode: "none"`) a real `MukanaClient` over a `FetchLike` this rig
 * controls. `mode: "failing"` is a show configured for all three
 * integrations whose backend has never once answered; `killMukana()` turns
 * a `"live"` rig into that mid-show.
 */
function showRig(options: { mode?: MukanaMode; seedState?: PersistedShowState } = {}): ShowRig {
  const mode = options.mode ?? "live";
  let nowMs = 0;
  let dead = mode === "failing";
  const clock: Clock = { now: () => nowMs };
  const host = new MockHost();
  const fetches: string[] = [];

  const fs = memoryFs(
    options.seedState === undefined ? {} : { [STATE_PATH]: JSON.stringify(options.seedState) }
  );

  const fetch: FetchLike = async (url: string): Promise<FetchResponse> => {
    fetches.push(url);
    if (dead) {
      return { ok: false, status: 503, text: async () => "" };
    }
    if (url.includes("req=hands")) {
      return { ok: true, status: 200, text: async () => HANDS_BODY };
    }
    if (url.includes("req=panelists")) {
      return { ok: true, status: 200, text: async () => PANELISTS_BODY };
    }
    return { ok: true, status: 200, text: async () => QUESTION_BODY };
  };

  const config = buildConfig(mode);
  const mukana =
    config.mukana === null ? undefined : new MukanaClient(config.mukana, { fetch });

  const deps: ShowEngineDeps = {
    config,
    host,
    clock,
    store: new StateStore(config.statePath, { fs }),
    ...(mukana === undefined ? {} : { mukana })
  };
  const engine = new ShowEngine(deps);

  return {
    engine,
    host,
    fs,
    fetches,
    advance: (ms: number) => {
      nowMs += ms;
    },
    killMukana: () => {
      dead = true;
    },
    tick: async (): Promise<ShowSnapshot> => {
      const snapshot = await engine.tick();
      for (let i = 0; i < 8; i += 1) {
        // eslint-disable-next-line no-await-in-loop
        await Promise.resolve();
      }
      return snapshot;
    }
  };
}

/** Seat the whole cast and run the first tick, which is also what starts the first poll of every endpoint. */
async function seatTheCast(rig: ShowRig): Promise<ShowSnapshot> {
  for (const member of CAST) {
    rig.engine.onZoomEvent(joined(member.id, member.rawName));
  }
  return rig.tick();
}

/** Assign the two exclusive chairs the way an operator does: by override, leaving name/location to the identity parse and the registry. */
function assignChairs(rig: ShowRig): void {
  rig.engine.setOverride({
    personKey: resolvePersonKey({ participantId: "p1", rawName: "Ann Reed | 1001 | Oslo" }),
    displayName: "",
    location: "",
    role: "host"
  });
  rig.engine.setOverride({
    personKey: resolvePersonKey({ participantId: "p2", rawName: "Bo Vance | 1002 | Reno" }),
    displayName: "",
    location: "",
    role: "reader"
  });
}

/** The slot numbers a look claims are on air, derived from the SAME snapshot's look node. */
function onAirSlotsOfLook(snapshot: ShowSnapshot): number[] {
  const look = snapshot.look;
  if (look === null) return [];
  const slots: number[] = [];
  if (look.hostSlot !== null) slots.push(look.hostSlot);
  if (look.readerSlot !== null) slots.push(look.readerSlot);
  for (const box of look.boxes) {
    if (box.slot !== null) slots.push(box.slot);
  }
  return Array.from(new Set(slots)).sort((a, b) => a - b);
}

function callKinds(calls: readonly HostCall[]): string[] {
  return calls.map((call) => call.kind);
}

function plateNames(calls: readonly HostCall[]): string[] {
  const last = [...calls].reverse().find((call) => call.kind === "setNameplates");
  if (last === undefined || last.kind !== "setNameplates") return [];
  return last.plates.map((plate) => plate.name);
}

// ---------------------------------------------------------------------------
// Scenario 1 — a full show, end to end.
// ---------------------------------------------------------------------------

describe("scenario 1: a full show, end to end", () => {
  /**
   * Seven join, the operator assigns a host and a reader, selects a look,
   * cuts it to air, a guest speaks, and the operator pages the queue.
   *
   * The invariant this must break on is TICK ORDER. Two specific
   * inversions:
   *
   *  - Deriving tally before the look resolves. Every step below compares
   *    `snapshot.tally.onAirSlots` against the slots computed from
   *    `snapshot.look` — the SAME snapshot, but a different producer inside
   *    the tick. A tally derived from the previous tick's resolution
   *    disagrees with the look published beside it the moment the look
   *    changes, which is exactly what the paging step forces.
   *  - Emitting nameplates from a stale resolution. The paging step asserts
   *    the plates the host actually received name the NEW page's guests,
   *    and that `applyLook` precedes `setNameplates` within the tick.
   */
  it("drives seating, chairs, a look, program, a speaker, and paging coherently", async () => {
    const rig = showRig();

    // --- The room fills up. -------------------------------------------------
    const seated = await seatTheCast(rig);
    expect(seated.revision).toBe(1);
    expect(seated.panelists).toHaveLength(7);
    expect(seated.slots.slice(0, 7).map((slot) => slot.panelist?.participantId)).toEqual([
      "p1",
      "p2",
      "p3",
      "p4",
      "p5",
      "p6",
      "p7"
    ]);
    expect(seated.slots[7]?.panelist).toBeNull();
    expect(seated.unseated).toEqual([]);
    expect(seated.look).toBeNull();
    expect(seated.tally.mode).toBe("none");
    expect(seated.program.program).toEqual({ kind: "black" });
    // The full picture goes out on the first tick, and nothing look-shaped
    // does: there is no look yet to apply or write plates for.
    expect(rig.host.callsOfKind("assignSlot")).toHaveLength(8);
    expect(rig.host.callsOfKind("setGallery")).toHaveLength(1);
    expect(rig.host.callsOfKind("applyLook")).toEqual([]);
    expect(rig.host.callsOfKind("setNameplates")).toEqual([]);

    // --- The three integrations answer. -------------------------------------
    // The first tick STARTED all three fetches; this one applies what
    // settled during that tick's drain.
    const integrated = await rig.tick();
    expect(integrated.capabilities.registry.state).toBe("available");
    expect(integrated.capabilities.handsQueue.state).toBe("available");
    expect(integrated.capabilities.questionFeed.state).toBe("available");
    expect(integrated.queue).toEqual({
      previous: [],
      current: "1007",
      upcoming: ["1001", "1003", "1004", "1005", "1006"]
    });
    // The registry actually reached a seated panelist rather than merging
    // into a map nothing reads.
    const ann = integrated.panelists.find((panelist) => panelist.participantId === "p1");
    expect(ann?.hasMukana).toBe(true);
    expect(ann?.location).toBe("Oslo, registered");

    // --- The operator assigns the two chairs. -------------------------------
    assignChairs(rig);
    const chaired = await rig.tick();
    expect(chaired.slots[0]?.panelist?.role).toBe("host");
    expect(chaired.slots[1]?.panelist?.role).toBe("reader");
    expect(chaired.slots[2]?.panelist?.role).toBe("panelist");

    // --- The operator selects a look. ---------------------------------------
    rig.host.clear();
    rig.engine.setLook("panel");
    const looked = await rig.tick();
    expect(looked.look?.lookId).toBe("panel");
    expect(looked.look?.scenePreset).toBe("scene-panel");
    expect(looked.look?.boxFill).toBe("queue");
    expect(looked.look?.hostSlot).toBe(1);
    expect(looked.look?.readerSlot).toBe(2);
    // Five queue candidates over two boxes: 1007 (current) then 1003..1006.
    // The host's own pin (1001) was in the raw queue and is NOT here — a
    // seated chair is stripped rather than double-booked into a guest box.
    expect(looked.look?.pageCount).toBe(3);
    expect(looked.page).toBe(0);
    expect(looked.look?.boxes).toEqual([
      { box: 1, slot: 7 },
      { box: 2, slot: 3 }
    ]);
    // Selecting a look STAGES it. It must not put itself on air.
    expect(looked.program.program).toEqual({ kind: "black" });
    expect(looked.program.preview).toEqual({ kind: "look", lookId: "panel" });
    expect(looked.tally.mode).toBe("none");
    expect(looked.tally.onAirSlots).toEqual([]);
    // The command sequence for this tick, in order: stage the look, apply
    // it, then write the overlays derived FROM it. Nameplates after the
    // look, never before.
    expect(callKinds(rig.host.calls())).toEqual([
      "setPreview",
      "applyLook",
      "setNameplates",
      "setQuestion"
    ]);
    expect(rig.host.callsOfKind("applyLook")[0]?.boxes).toEqual([
      [1, 7],
      [2, 3]
    ]);
    expect(plateNames(rig.host.calls())).toEqual(["Ann Reed", "Bo Vance", "Gil Hale", "Cy Diaz"]);

    // --- Program cuts to the look. ------------------------------------------
    rig.host.clear();
    rig.engine.cut();
    const onAir = await rig.tick();
    expect(onAir.program.program).toEqual({ kind: "look", lookId: "panel" });
    expect(onAir.tally.mode).toBe("look");
    expect(onAir.tally.onAirSlots).toEqual([1, 2, 3, 7]);
    expect(onAir.tally.onAirParticipantIds).toEqual(["p1", "p2", "p3", "p7"]);
    // Tally and the look it was derived from must agree ON THE SAME
    // SNAPSHOT — see this test's doc comment.
    expect(onAir.tally.onAirSlots).toEqual(onAirSlotsOfLook(onAir));
    expect(callKinds(rig.host.calls())).toEqual(["cut"]);

    // --- The operator pages the queue. --------------------------------------
    rig.host.clear();
    rig.engine.nextGuest();
    const paged = await rig.tick();
    expect(paged.page).toBe(1);
    expect(paged.pagingRefused).toBeNull();
    expect(paged.look?.boxes).toEqual([
      { box: 1, slot: 4 },
      { box: 2, slot: 5 }
    ]);
    // The anti-staleness pair: this tick's tally follows this tick's page,
    // and the plates the host received name this tick's guests.
    expect(paged.tally.onAirSlots).toEqual([1, 2, 4, 5]);
    expect(paged.tally.onAirSlots).toEqual(onAirSlotsOfLook(paged));
    expect(callKinds(rig.host.calls())).toEqual(["applyLook", "setNameplates", "setQuestion"]);
    expect(rig.host.callsOfKind("applyLook")[0]?.boxes).toEqual([
      [1, 4],
      [2, 5]
    ]);
    expect(plateNames(rig.host.calls())).toEqual(["Ann Reed", "Bo Vance", "Dee Ekko", "Eli Fox"]);

    // --- A guest speaks. -----------------------------------------------------
    rig.engine.setActiveSpeakerFollow(true);
    rig.engine.onActiveSpeaker("p5");
    const speaking = await rig.tick();
    expect(speaking.program.activeSpeakerId).toBe("p5");
    expect(speaking.program.program).toEqual({ kind: "activeSpeaker" });
    expect(speaking.tally.mode).toBe("activeSpeaker");
    expect(speaking.tally.onAirSlots).toEqual([5]);

    // --- The ASL interpreter speaks, and must not steal the shot. -----------
    rig.engine.setOverride({
      personKey: resolvePersonKey({ participantId: "p6", rawName: "Fay Gunn | 1006 | Doha" }),
      displayName: "",
      location: "",
      role: "aslinterpreter"
    });
    rig.engine.onActiveSpeaker("p6");
    const stillEli = await rig.tick();
    expect(stillEli.program.activeSpeakerId).toBe("p5");
    expect(stillEli.tally.onAirSlots).toEqual([5]);

    // --- A tick that changes nothing says nothing. ---------------------------
    rig.host.clear();
    await rig.tick();
    await rig.tick();
    expect(rig.host.calls()).toEqual([]);
  });
});

// ---------------------------------------------------------------------------
// Scenario 2 — the registry dies mid-show.
// ---------------------------------------------------------------------------

/** A state file carrying a real gallery arrangement, so "the gallery survived" is not a statement about sixteen zeros. */
function seedStateWithGallery(): PersistedShowState {
  return {
    version: STATE_VERSION,
    slots: { version: 1, capacity: 8, seats: new Array(8).fill(null) },
    overrides: {},
    gallery: {
      version: 1,
      cells: 16,
      assignments: Array.from({ length: 16 }, (_, index) => ({
        cell: index + 1,
        slot: index < 7 ? index + 1 : 0
      }))
    },
    manualBoxes: {},
    lookId: null
  };
}

describe("scenario 2: the registry dies mid-show", () => {
  /**
   * The scenario Plan 4's outcomes doc recorded as owed: a show running
   * healthy on all three integrations, then every Mukana endpoint starts
   * failing.
   *
   * The invariant this must break on: any consumer treating a lost
   * integration as a reason to CLEAR state rather than to DEGRADE. Nothing
   * the outage touches may empty — not the roster, not the seating, not the
   * gallery, not the last-good queue, not the question already on screen,
   * not the chairs, and not the guest boxes, which fall back to the
   * operator's manual assignments.
   */
  it("degrades to the operator's manual assignments and clears nothing", async () => {
    const rig = showRig({ seedState: seedStateWithGallery() });
    expect(await rig.engine.restore()).toBe(true);

    await seatTheCast(rig); // tick 1: seats the cast, starts the first polls
    rig.engine.setQuestionVisible(true);
    await rig.tick(); // tick 2: the polls land

    assignChairs(rig);
    await rig.tick();

    rig.engine.setLook("panel");
    const healthyLook = await rig.tick();
    expect(healthyLook.look?.boxFill).toBe("queue");
    expect(healthyLook.look?.boxes).toEqual([
      { box: 1, slot: 7 },
      { box: 2, slot: 3 }
    ]);

    // The operator's manual fallback, staged while the queue is still
    // driving the boxes (`resolveLook` ignores it under queue fill). Set to
    // the same two people the queue currently has on screen — an operator
    // pinning what is already right, which is what makes "the boxes did not
    // change" a meaningful assertion rather than a coincidence.
    rig.engine.assignBox(1, 7);
    rig.engine.assignBox(2, 3);

    rig.engine.cut();
    const healthy = await rig.tick();

    expect(healthy.capabilities.registry.state).toBe("available");
    expect(healthy.capabilities.handsQueue.state).toBe("available");
    expect(healthy.capabilities.questionFeed.state).toBe("available");
    expect(healthy.program.program).toEqual({ kind: "look", lookId: "panel" });
    expect(healthy.tally.onAirSlots).toEqual([1, 2, 3, 7]);
    expect(healthy.overlays.question).toEqual({
      askerName: "Ivy Nunes",
      text: "How is the panel funded?",
      tag: "money",
      votes: 7
    });
    // The gallery is a real arrangement restored from disk, not an empty grid.
    expect(healthy.gallery.some((cell) => cell.slot !== 0)).toBe(true);

    // --- Mukana dies. Every endpoint, all at once. ---------------------------
    rig.host.clear();
    rig.killMukana();
    rig.advance(60_000);
    await rig.tick(); // the failing fetches start and settle
    await rig.tick(); // their outcomes apply
    rig.advance(60_000);
    const degraded = await rig.tick();

    // Every capability reports the outage, honestly and with a reason.
    for (const capability of [
      degraded.capabilities.registry,
      degraded.capabilities.handsQueue,
      degraded.capabilities.questionFeed
    ] satisfies Capability[]) {
      expect(capability.state).toBe("unavailable");
      expect(capability.detail).not.toBeNull();
    }
    for (const endpoint of ["panelists", "hands", "question"] satisfies MukanaEndpoint[]) {
      const health: MukanaHealth = degraded.health[endpoint];
      expect(health.state).toBe("failing");
      expect(health.consecutiveFailures).toBeGreaterThan(0);
    }

    // Nothing else moved.
    expect(degraded.panelists).toEqual(healthy.panelists);
    expect(degraded.slots).toEqual(healthy.slots);
    expect(degraded.gallery).toEqual(healthy.gallery);
    expect(degraded.queue).toEqual(healthy.queue);
    expect(degraded.program).toEqual(healthy.program);
    expect(degraded.overlays).toEqual(healthy.overlays);
    expect(degraded.tally).toEqual(healthy.tally);

    // The resolved look survives too, in every respect except the two
    // fields whose whole job is to describe the degradation: `boxFill`
    // flips to manual, and `pageCount` collapses to the single page manual
    // fill has. Compared as WHOLE nodes minus those two, so a field added
    // to `LookResolution` later is covered without anyone remembering to
    // add it here.
    const { boxFill: healthyFill, pageCount: healthyPages, ...healthyRest } = healthyLook.look ?? {
      boxFill: null,
      pageCount: null
    };
    const { boxFill: degradedFill, pageCount: degradedPages, ...degradedRest } = degraded.look ?? {
      boxFill: null,
      pageCount: null
    };
    expect(healthyFill).toBe("queue");
    expect(degradedFill).toBe("manual");
    expect(healthyPages).toBe(3);
    expect(degradedPages).toBe(1);
    expect(degradedRest).toEqual(healthyRest);

    // The boxes did not empty: they are still filled, by the operator's
    // manual assignments now that the queue is gone.
    expect(degraded.look?.boxes).toEqual([
      { box: 1, slot: 7 },
      { box: 2, slot: 3 }
    ]);

    // The whole outage was invisible to the host — no re-bind, no re-apply,
    // no plate re-raster. Degrading is not the same as churning.
    expect(rig.host.calls()).toEqual([]);

    // And the manual table really is what drives the boxes now: an operator
    // move that queue fill would have ignored takes effect immediately.
    rig.engine.assignBox(2, 6);
    const remanaged = await rig.tick();
    expect(remanaged.look?.boxes).toEqual([
      { box: 1, slot: 7 },
      { box: 2, slot: 6 }
    ]);
    expect(remanaged.tally.onAirSlots).toEqual([1, 2, 6, 7]);
  });
});

// ---------------------------------------------------------------------------
// Scenario 3 — degradation equivalence, at the engine level.
// ---------------------------------------------------------------------------

/**
 * The one canonical value every capability node is rewritten to before two
 * engines' snapshots are compared, and the one canonical health record.
 *
 * WHY BOTH NODES, when the brief asked only for `detail` to be stripped:
 * `capabilities[*].state` is itself `"unavailable"` for a configured-but-
 * failing integration and `"disabled"` for one that was never configured —
 * by construction, in `resolveCapability`. Those two strings are the
 * DECLARATION of the difference, not a behavior branching on it, and
 * `canUse` is what collapses them for every consumer. `health` is the same
 * kind of node one level down: it is the raw operator-facing diagnostic
 * (`showEngine.test.ts` pins its content as a contract in its own right),
 * and a failing backend must be distinguishable there from one nobody
 * configured, or an operator cannot tell an outage from a setting.
 *
 * So the normalizer erases exactly the two nodes whose JOB is to report the
 * difference, and leaves every other field of the snapshot — revision,
 * panelists, slots, gallery, queue, program, look, page, manualBoxes,
 * tally, overlays, unseated, pagingRefused — compared verbatim. The point
 * of the property is that nobody hand-picked that list; the test below
 * ("the normalizer is not a blanket") proves the residue still has teeth.
 */
const NORMALIZED_CAPABILITY: Capability = { state: "disabled", detail: null };
const NORMALIZED_HEALTH: MukanaHealth = { state: "ok", consecutiveFailures: 0, detail: null };

function normalizeDegradationChannel(snapshot: ShowSnapshot): ShowSnapshot {
  const copy = structuredClone(snapshot);
  copy.capabilities = {
    registry: { ...NORMALIZED_CAPABILITY },
    handsQueue: { ...NORMALIZED_CAPABILITY },
    questionFeed: { ...NORMALIZED_CAPABILITY }
  };
  copy.health = {
    panelists: { ...NORMALIZED_HEALTH },
    hands: { ...NORMALIZED_HEALTH },
    question: { ...NORMALIZED_HEALTH }
  };
  return copy;
}

/**
 * The identical show both engines are driven through. Every operator action
 * and every tick is the same; only the integration configuration differs.
 * Written once and run twice so there is no chance of the two diverging by
 * transcription.
 */
async function runIdenticalShow(rig: ShowRig): Promise<ShowSnapshot> {
  for (const member of CAST.slice(0, 5)) {
    rig.engine.onZoomEvent(joined(member.id, member.rawName));
  }
  await rig.tick();
  await rig.tick();

  assignChairs(rig);
  await rig.tick();

  rig.engine.setLook("panel");
  await rig.tick();

  rig.engine.assignBox(1, 3);
  rig.engine.assignBox(2, 4);
  await rig.tick();

  rig.engine.cut();
  await rig.tick();

  // Paging: refused identically on both, since neither has a usable hands
  // queue. The refusal STRING is part of the compared snapshot.
  rig.engine.nextGuest();
  await rig.tick();

  rig.engine.setActiveSpeakerFollow(true);
  rig.engine.onActiveSpeaker("p5");
  await rig.tick();

  rig.advance(60_000);
  await rig.tick();
  return rig.tick();
}

describe("scenario 3: degradation equivalence at the engine level", () => {
  /**
   * Plan 4 proved `unavailable ≡ disabled` for look resolution in
   * isolation. This proves it for the whole engine: a show whose
   * integrations are configured and failing behaves exactly like a show
   * that never had them.
   *
   * The invariant this must break on: any code path branching on
   * `disabled` vs `unavailable` outside the two nodes that exist to report
   * the difference. If it ever fails, a mid-show outage has stopped
   * behaving like a show that never had the integration — which is the
   * whole design.
   */
  it("produces structurally equal snapshots for a failing integration and one never configured", async () => {
    const failing = showRig({ mode: "failing" });
    const never = showRig({ mode: "none" });

    const failingSnapshot = await runIdenticalShow(failing);
    const neverSnapshot = await runIdenticalShow(never);

    // The engines really are in the two different states this property is about.
    expect(failing.fetches.length).toBeGreaterThan(0);
    expect(never.fetches).toEqual([]);
    expect(failingSnapshot.capabilities.handsQueue.state).toBe("unavailable");
    expect(failingSnapshot.capabilities.handsQueue.detail).not.toBeNull();
    expect(neverSnapshot.capabilities.handsQueue.state).toBe("disabled");
    expect(neverSnapshot.capabilities.handsQueue.detail).toBeNull();

    // And everything the show is actually made of is identical.
    expect(normalizeDegradationChannel(failingSnapshot)).toEqual(
      normalizeDegradationChannel(neverSnapshot)
    );

    // Not vacuous: the compared residue is a real show, not an empty one.
    expect(failingSnapshot.panelists).toHaveLength(5);
    expect(failingSnapshot.look?.boxes).toEqual([
      { box: 1, slot: 3 },
      { box: 2, slot: 4 }
    ]);
    expect(failingSnapshot.tally.onAirSlots).toEqual([5]);
    expect(failingSnapshot.pagingRefused).toMatch(/manual/i);
  });

  /**
   * The guard on the guard. A normalizer that erased too much would make
   * two genuinely different shows compare equal, and the property above
   * would pass for the wrong reason forever. Every field the normalizer
   * does NOT erase must still be able to red it.
   */
  it("the normalizer is not a blanket: a real difference still reds", async () => {
    const rig = showRig({ mode: "none" });
    const snapshot = await runIdenticalShow(rig);

    const withDifferentPage = structuredClone(snapshot);
    withDifferentPage.revision += 1;
    expect(normalizeDegradationChannel(withDifferentPage)).not.toEqual(
      normalizeDegradationChannel(snapshot)
    );

    const withDifferentSeat = structuredClone(snapshot);
    const seat = withDifferentSeat.slots[0];
    if (seat === undefined || seat.panelist === null) {
      throw new Error("fixture: slot 1 must be occupied for this guard to mean anything");
    }
    seat.panelist.displayName = "Someone Else";
    expect(normalizeDegradationChannel(withDifferentSeat)).not.toEqual(
      normalizeDegradationChannel(snapshot)
    );

    const withDifferentLook = structuredClone(snapshot);
    const look = withDifferentLook.look;
    if (look === null) {
      throw new Error("fixture: a look must be resolved for this guard to mean anything");
    }
    look.boxes = [];
    expect(normalizeDegradationChannel(withDifferentLook)).not.toEqual(
      normalizeDegradationChannel(snapshot)
    );

    const withDifferentRefusal = structuredClone(snapshot);
    withDifferentRefusal.pagingRefused = null;
    expect(normalizeDegradationChannel(withDifferentRefusal)).not.toEqual(
      normalizeDegradationChannel(snapshot)
    );
  });
});

// ---------------------------------------------------------------------------
// The barrel.
// ---------------------------------------------------------------------------

describe("package barrel", () => {
  /**
   * Plans 6–9 build three host adapters against these names. A name that
   * exists somewhere in `src/` but is not reachable through `./index.js` is
   * not exported as far as those plans are concerned — so this asserts on
   * the values resolved through the barrel (every import in this file comes
   * from there), not on the modules that define them.
   */
  it("exports every runtime value an out-of-package host adapter needs", () => {
    expect(systemClock).toBeTypeOf("object");
    expect(systemClock.now()).toBeTypeOf("number");
    expect(MockHost).toBeTypeOf("function");
    expect(new MockHost().capabilities().hasPreviewBus).toBe(true);
    expect(buildSnapshot).toBeTypeOf("function");
    expect(ShowEngine).toBeTypeOf("function");
    expect(STATE_VERSION).toBe(3);
  });

  /**
   * The type-only half. A type-only export cannot be asserted at runtime,
   * so each one is used in a type position here and carried by
   * `npm run typecheck:tests` — a file that fails to typecheck is a failing
   * test (`index.test.ts` makes the same promise).
   */
  it("exports every type an out-of-package host adapter needs to name", () => {
    const clock: Clock = systemClock;
    const capabilities: HostCapabilities = {
      hasPreviewBus: false,
      maxGalleryCells: 4,
      transitions: ["cut"]
    };
    const host: HostAdapter = new MockHost(capabilities);
    const call: HostCall = { kind: "cut" };
    const persisted: PersistedShowState = seedStateWithGallery();
    const deps: ShowEngineDeps = {
      config: buildConfig("none"),
      host,
      clock,
      store: new StateStore(STATE_PATH, { fs: memoryFs() })
    };
    const snapshot: ShowSnapshot = new ShowEngine(deps).snapshot();

    expect(host.capabilities().maxGalleryCells).toBe(4);
    expect(call.kind).toBe("cut");
    expect(persisted.version).toBe(STATE_VERSION);
    expect(snapshot.revision).toBe(0);
  });
});
