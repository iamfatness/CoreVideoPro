# Show Engine Orchestrator & Host Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Compose the 21 shipped `show-engine` modules into a single ticking `ShowEngine` that drives a host through a `HostAdapter` port, publishes one snapshot, and closes the three obligations carried from Plans 3–4.

**Architecture:** A `ShowEngine` class owns every module instance, an injected `Clock`, and one `HostAdapter`. Host events and operator intent mutate module state; a tick recomputes the derived layers (look → program → tally → overlays), publishes an immutable `ShowSnapshot`, and emits host commands only where a value actually changed. Every module it calls is already clock-free and pure-ish, so the engine is the sole owner of time, I/O scheduling, and cross-module vocabulary translation.

**Tech Stack:** TypeScript 5.9 strict, ES2022, NodeNext modules, vitest 4, Node 24. No new dependencies.

## Global Constraints

- Module system is **NodeNext**: every relative import MUST end in `.js`.
- vitest runs with `globals: false`: every test file MUST `import { describe, expect, it } from "vitest";` explicitly.
- Strict TypeScript. **No `any`.** No non-null assertions where a guard will do.
- **No I/O in engine modules.** `StateStore` is the package's only writer and reaches the filesystem through the injected `StateFs`. `MukanaClient` reaches the network through the injected `FetchLike`. The orchestrator adds no third I/O path — it schedules, it does not fetch or write directly.
- **No `Date.now()`, no `new Date()`, no `setTimeout` inside engine modules.** Wall-clock time enters only through the injected `Clock` (Task 1). This is what keeps every test deterministic without fake timers.
- Everything a host adapter consumes MUST be reachable through the barrel `index.ts`.
- **Do NOT use `git stash`** for any reason. The stash stack is shared with other working trees on this machine and their owner works in them concurrently.
- Branch: `plan/show-engine-orchestrator`, stacked on `plan/show-engine-capabilities`.

## Naming hazard — read before Task 1

`persistence.ts` already exports a type named `ShowState`: the **persisted subset** (slots, overrides, gallery). The spec's §4.3 "state" is a much richer **published snapshot** (panelists, slots, gallery, queue, program, tally, overlays, capabilities, health). These are different objects with different lifetimes and must never share a name.

- The persisted type is **renamed** to `PersistedShowState` in Task 3.
- The published type is **new**, named `ShowSnapshot`, in Task 4.

Any task that says "state" without a qualifier is wrong; use one of those two names.

## File Structure

**Create:**
- `show-engine/src/clock.ts` — the `Clock` seam. One type, one real implementation. Sole owner of wall-clock access in the package.
- `show-engine/src/hostAdapter.ts` — the `HostAdapter` port (§6 of the design spec): host capability declaration and the command surface the engine drives.
- `show-engine/src/mockHost.ts` — a recording `HostAdapter` double, **shipped in `src/`, not a test file**, because Plan 6's conformance suite and every future adapter author needs it.
- `show-engine/src/showSnapshot.ts` — the `ShowSnapshot` type and the pure `buildSnapshot()` that assembles it from module state.
- `show-engine/src/showEngine.ts` — the `ShowEngine` class: construction, restore, event intake, the tick, and host command emission.

**Modify:**
- `show-engine/src/persistence.ts` — rename `ShowState` → `PersistedShowState`; add `manualBoxes` and `lookId`; bump `STATE_VERSION` 2 → 3; export the constant.
- `show-engine/src/index.ts` — export everything new.

**Test:**
- `show-engine/src/hostAdapter.test.ts`, `showSnapshot.test.ts`, `showEngine.test.ts`, `speakerGateDispatch.test.ts`, `enginePipeline.test.ts`.
- `show-engine/src/persistence.test.ts` (existing, extended).

Why `showEngine.ts` is one file rather than several: the tick's ordering *is* the design — roster before look, look before tally, tally before overlays — and splitting it across files would hide that sequence behind imports. It stays one file with one class; the pure helpers it needs (`buildSnapshot`) live outside it so they are testable without constructing an engine. If it exceeds ~450 lines, extract the command-emission half to `hostCommands.ts` rather than letting it sprawl.

---

### Task 1: The clock seam and the HostAdapter port

**Files:**
- Create: `show-engine/src/clock.ts`, `show-engine/src/hostAdapter.ts`, `show-engine/src/mockHost.ts`
- Test: `show-engine/src/hostAdapter.test.ts`

**Interfaces:**
- Consumes: `ProgramSource`, `Nameplate`, `QuestionOverlay` (from `contracts.js` / `lookDirector.js` / `overlayDirector.js`).
- Produces, for every later task and for Plans 6–9:

```ts
// clock.ts
export type Clock = { now(): number };
export const systemClock: Clock;

// hostAdapter.ts
export type HostCapabilities = {
  hasPreviewBus: boolean;
  maxGalleryCells: number;
  transitions: readonly string[];
};

export interface HostAdapter {
  capabilities(): HostCapabilities;
  assignSlot(slot: number, participantId: string | null): void;
  applyLook(lookId: string, boxes: ReadonlyMap<number, number | null>): void;
  setPreview(source: ProgramSource): void;
  cut(): void;
  auto(transitionId?: string): void;
  setGallery(cells: ReadonlyMap<number, number>): void;
  setNameplates(plates: readonly Nameplate[]): void;
  setQuestion(question: QuestionOverlay | null): void;
}

// mockHost.ts
export type HostCall =
  | { kind: "assignSlot"; slot: number; participantId: string | null }
  | { kind: "applyLook"; lookId: string; boxes: Array<[number, number | null]> }
  | { kind: "setPreview"; source: ProgramSource }
  | { kind: "cut" }
  | { kind: "auto"; transitionId: string | undefined }
  | { kind: "setGallery"; cells: Array<[number, number]> }
  | { kind: "setNameplates"; plates: Nameplate[] }
  | { kind: "setQuestion"; question: QuestionOverlay | null };

export class MockHost implements HostAdapter {
  constructor(capabilities?: Partial<HostCapabilities>);
  calls(): readonly HostCall[];
  callsOfKind<K extends HostCall["kind"]>(kind: K): ReadonlyArray<Extract<HostCall, { kind: K }>>;
  clear(): void;
}
```

**Behavior:**

`systemClock` is the only place in the package that may read the system clock; it wraps `Date.now()`. Nothing else — engine module or test — calls it directly.

`MockHost` records every call in arrival order into an array. Its default capabilities are `{ hasPreviewBus: true, maxGalleryCells: 16, transitions: ["cut", "fade"] }`; the constructor merges a partial override over those defaults so a test can build a preview-less host with `new MockHost({ hasPreviewBus: false })`.

`MockHost` **copies every Map argument into an array of entries at record time.** The engine will reuse and mutate its working maps between ticks; a mock that stored the reference would report the final state for every historical call, which silently turns a diffing bug into a passing test. `calls()` returns the recorded array; `clear()` empties it so a test can assert about one tick without counting startup traffic.

- [ ] **Step 1: Write the failing test**

```ts
// show-engine/src/hostAdapter.test.ts
import { describe, expect, it } from "vitest";
import { MockHost } from "./mockHost.js";

describe("MockHost", () => {
  it("defaults to a host with a preview bus and 16 gallery cells", () => {
    expect(new MockHost().capabilities()).toEqual({
      hasPreviewBus: true,
      maxGalleryCells: 16,
      transitions: ["cut", "fade"]
    });
  });

  it("merges a partial capability override over the defaults", () => {
    const host = new MockHost({ hasPreviewBus: false });
    expect(host.capabilities().hasPreviewBus).toBe(false);
    expect(host.capabilities().maxGalleryCells).toBe(16);
  });

  it("records calls in arrival order", () => {
    const host = new MockHost();
    host.assignSlot(1, "p1");
    host.cut();
    host.assignSlot(2, null);
    expect(host.calls().map((c) => c.kind)).toEqual(["assignSlot", "cut", "assignSlot"]);
  });

  it("filters by kind with the narrowed element type", () => {
    const host = new MockHost();
    host.assignSlot(3, "p9");
    host.cut();
    const assigns = host.callsOfKind("assignSlot");
    expect(assigns).toHaveLength(1);
    expect(assigns[0]?.slot).toBe(3);
    expect(assigns[0]?.participantId).toBe("p9");
  });

  /**
   * The invariant this must break on: storing the caller's Map by reference.
   * The engine reuses its working maps between ticks, so a reference-storing
   * mock reports every historical call as the final state — turning a real
   * diffing bug into a green test.
   */
  it("snapshots map arguments so later mutation cannot rewrite history", () => {
    const host = new MockHost();
    const boxes = new Map<number, number | null>([[1, 4]]);
    host.applyLook("teatime", boxes);
    boxes.set(1, 7);
    boxes.set(2, 9);
    const recorded = host.callsOfKind("applyLook")[0];
    expect(recorded?.boxes).toEqual([[1, 4]]);
  });

  it("snapshots gallery maps the same way", () => {
    const host = new MockHost();
    const cells = new Map<number, number>([[1, 2]]);
    host.setGallery(cells);
    cells.set(1, 5);
    expect(host.callsOfKind("setGallery")[0]?.cells).toEqual([[1, 2]]);
  });

  it("clears recorded calls without clearing capabilities", () => {
    const host = new MockHost({ maxGalleryCells: 4 });
    host.cut();
    host.clear();
    expect(host.calls()).toEqual([]);
    expect(host.capabilities().maxGalleryCells).toBe(4);
  });

  it("records auto's transition id, including when omitted", () => {
    const host = new MockHost();
    host.auto();
    host.auto("fade");
    expect(host.callsOfKind("auto").map((c) => c.transitionId)).toEqual([undefined, "fade"]);
  });
});
```

- [ ] **Step 2: Run the test and watch it fail**

Run: `cd show-engine && npx vitest run src/hostAdapter.test.ts`
Expected: FAIL — cannot resolve `./mockHost.js`.

- [ ] **Step 3: Write `clock.ts`, `hostAdapter.ts`, and `mockHost.ts`**

Write the three files to the interfaces and behavior above. `hostAdapter.ts` is types only — no runtime code beyond the interface declaration. Document on `Clock` that it is the package's sole wall-clock owner and why (every other module is already deterministic; the engine must stay so).

- [ ] **Step 4: Run the test and watch it pass**

Run: `cd show-engine && npx vitest run src/hostAdapter.test.ts`
Expected: PASS, 8 tests.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/clock.ts show-engine/src/hostAdapter.ts show-engine/src/mockHost.ts show-engine/src/hostAdapter.test.ts
git commit -m "feat(show-engine): the clock seam and the host adapter port"
```

---

### Task 2: Persisted state gains manual box assignments

**Files:**
- Modify: `show-engine/src/persistence.ts`
- Test: `show-engine/src/persistence.test.ts` (existing — extend, do not rewrite)

**Interfaces:**
- Consumes: `ManualBoxAssignments` from `lookDirector.js`.
- Produces:

```ts
export const STATE_VERSION = 3;   // now EXPORTED (it was module-private at v2)

export type PersistedShowState = {   // renamed from ShowState
  version: 3;
  slots: LiveSlotsState;
  overrides: Record<PersonKey, OverrideRecord>;
  gallery: GalleryState;
  manualBoxes: ManualBoxAssignments;
  lookId: string | null;
};

export class StateStore {
  constructor(path: string, deps: { fs: StateFs });
  save(state: PersistedShowState): Promise<void>;
  load(): Promise<PersistedShowState | null>;
}
```

**Behavior:**

Rename the exported type `ShowState` to `PersistedShowState` everywhere, including the barrel. Every reference in the package must move; leave no alias behind — an alias is exactly how the two "state" concepts would re-merge.

Add two fields. `manualBoxes` is the operator's box → roster-slot assignments; spec §3.2 requires them to survive a tick and be cleared when the look changes. `lookId` records **which look those assignments belong to**. Without it, restarting into a different look would silently inherit the previous look's manual boxes — assignments the operator made for a different arrangement, applied to boxes that mean something else. Persisting the pair is what makes "cleared when the look changes" hold across a restart, not just within a session.

Bump `STATE_VERSION` to 3 and **export it**, so a host can report the version it is about to reject rather than failing opaquely. `load()` keeps its existing contract exactly: `null` on missing, unparseable, wrong-version, or shape-check failure — never a throw, and no migration. A v2 file is rejected, per the module's stated reject-don't-migrate policy.

Extend the shallow shape check to cover the two new fields: `manualBoxes` must be an object, `lookId` must be a string or `null`.

- [ ] **Step 1: Write the failing tests**

Append to `show-engine/src/persistence.test.ts`:

```ts
describe("PersistedShowState v3", () => {
  it("round-trips manual box assignments and the look they belong to", async () => {
    const fs = memoryFs();
    const store = new StateStore("/state/show.json", { fs });
    const written: PersistedShowState = {
      ...baseState(),
      manualBoxes: { 1: 4, 2: 7 },
      lookId: "teatime"
    };
    await store.save(written);
    expect(await store.load()).toEqual(written);
  });

  it("round-trips an empty manual assignment set and a null look", async () => {
    const fs = memoryFs();
    const store = new StateStore("/state/show.json", { fs });
    const written: PersistedShowState = { ...baseState(), manualBoxes: {}, lookId: null };
    await store.save(written);
    expect(await store.load()).toEqual(written);
  });

  /**
   * The invariant this must break on: accepting a v2 file. A v2 file has no
   * manualBoxes and its overrides are PIN-keyed, so loading one would seat a
   * roster whose operator role assignments silently vanished.
   */
  it("rejects a version-2 file rather than migrating it", async () => {
    const fs = memoryFs();
    await fs.writeFile(
      "/state/show.json",
      JSON.stringify({ ...baseState(), version: 2, manualBoxes: {}, lookId: null })
    );
    expect(await new StateStore("/state/show.json", { fs }).load()).toBeNull();
  });

  it("rejects a v3 file whose manualBoxes is not an object", async () => {
    const fs = memoryFs();
    await fs.writeFile(
      "/state/show.json",
      JSON.stringify({ ...baseState(), manualBoxes: [], lookId: null })
    );
    expect(await new StateStore("/state/show.json", { fs }).load()).toBeNull();
  });

  it("rejects a v3 file whose lookId is neither string nor null", async () => {
    const fs = memoryFs();
    await fs.writeFile(
      "/state/show.json",
      JSON.stringify({ ...baseState(), manualBoxes: {}, lookId: 7 })
    );
    expect(await new StateStore("/state/show.json", { fs }).load()).toBeNull();
  });

  it("exports the current version so a host can name what it rejected", () => {
    expect(STATE_VERSION).toBe(3);
  });
});
```

`baseState()` is a local helper returning a valid `PersistedShowState` with `version: 3`, an empty-seat `slots`, empty `overrides`, an empty `gallery`, `manualBoxes: {}`, and `lookId: null`. `memoryFs()` is the existing in-file `StateFs` double — reuse it, do not write a second one.

- [ ] **Step 2: Run and watch them fail**

Run: `cd show-engine && npx vitest run src/persistence.test.ts`
Expected: FAIL — `PersistedShowState` and `STATE_VERSION` are not exported.

- [ ] **Step 3: Apply the rename, the two fields, and the version bump**

Update `persistence.ts`, then fix every reference across `src/` (the barrel and any existing fixture that names `ShowState`). The build is the checklist: `npm run typecheck && npm run typecheck:tests` must both be clean before you move on.

- [ ] **Step 4: Run the whole suite**

Run: `cd show-engine && npx vitest run && npm run typecheck && npm run typecheck:tests`
Expected: all green. Every prior test still passes — this is a rename plus two additive fields, not a behavior change.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/persistence.ts show-engine/src/persistence.test.ts show-engine/src/index.ts
git commit -m "feat(show-engine): persist manual box assignments and their look"
```

---

### Task 3: The published snapshot

**Files:**
- Create: `show-engine/src/showSnapshot.ts`
- Test: `show-engine/src/showSnapshot.test.ts`

**Interfaces:**
- Consumes: `Panelist`, `Slot`, `GalleryCell`, `QueueState`, `ShowCapabilities` (contracts), `ProgramState` (programBus), `LookResolution`, `ManualBoxAssignments` (lookDirector), `TallyState` (tallyPublisher), `OverlayState` (overlayDirector), `MukanaEndpoint`, `MukanaHealth` (mukanaClient).
- Produces:

```ts
export type ShowSnapshot = {
  revision: number;
  panelists: Panelist[];
  slots: Slot[];
  gallery: GalleryCell[];
  queue: QueueState;
  program: ProgramState;
  look: LookResolution | null;
  page: number;
  manualBoxes: ManualBoxAssignments;
  tally: TallyState;
  overlays: OverlayState;
  capabilities: ShowCapabilities;
  health: Record<MukanaEndpoint, MukanaHealth>;
};

export function buildSnapshot(input: {
  revision: number;
  panelists: ReadonlyMap<string, Panelist>;
  slots: readonly Slot[];
  gallery: readonly GalleryCell[];
  queue: QueueState;
  program: ProgramState;
  look: LookResolution | null;
  page: number;
  manualBoxes: ManualBoxAssignments;
  tally: TallyState;
  overlays: OverlayState;
  capabilities: ShowCapabilities;
  health: Record<MukanaEndpoint, MukanaHealth>;
}): ShowSnapshot;
```

**Behavior:**

`buildSnapshot` is pure: same input, structurally equal output, no clock, no I/O, no module instances. It exists so the shape of published state is testable without constructing an engine, and so Plan 6's control integration has one function to call.

It **deep-copies every mutable structure it publishes** — the panelist map flattened to an array, the slot array, gallery cells, queue arrays, manual box assignments, tally arrays, and overlay nameplates. This is the load-bearing property: the engine holds live module state that keeps mutating, and a consumer (a WinUI panel, an OSC feedback loop, a WebSocket push) may hold a snapshot across ticks. A shallow copy would let the next tick rewrite a snapshot a surface is mid-render on.

`panelists` is published as an array sorted by `participantId`, so two snapshots built from the same roster compare equal regardless of Map insertion order.

`revision` is supplied by the caller and published verbatim: it is the engine's tick counter, letting a consumer discard a stale snapshot without deep-comparing.

- [ ] **Step 1: Write the failing test**

```ts
// show-engine/src/showSnapshot.test.ts
import { describe, expect, it } from "vitest";
import { buildSnapshot } from "./showSnapshot.js";
import type { Panelist, Slot } from "./contracts.js";

function panelist(id: string, name: string): Panelist {
  return {
    participantId: id,
    rawName: name,
    online: true,
    videoOn: true,
    audioOn: true,
    handRaised: false,
    zoomRole: 0,
    displayName: name,
    location: "",
    pin: null,
    hasMukana: false,
    role: "panelist",
    personKey: `name:${name.toLowerCase()}`
  };
}

function input() {
  const ann = panelist("p2", "Ann");
  const bo = panelist("p1", "Bo");
  const slots: Slot[] = [
    { slot: 1, panelist: bo },
    { slot: 2, panelist: null }
  ];
  return {
    revision: 7,
    panelists: new Map([
      ["p2", ann],
      ["p1", bo]
    ]),
    slots,
    gallery: [{ cell: 1, slot: 1 }],
    queue: { previous: [], current: "4242", upcoming: ["5555"] },
    program: {
      program: { kind: "black" } as const,
      preview: { kind: "black" } as const,
      activeSpeakerFollow: false,
      activeSpeakerId: null
    },
    look: null,
    page: 0,
    manualBoxes: { 1: 4 },
    tally: { mode: "none" as const, onAirSlots: [], onAirPins: [], onAirParticipantIds: [] },
    overlays: { nameplates: [], question: null },
    capabilities: {
      registry: { state: "disabled" as const, detail: null },
      handsQueue: { state: "disabled" as const, detail: null },
      questionFeed: { state: "disabled" as const, detail: null }
    },
    health: {
      panelists: { state: "ok" as const, consecutiveFailures: 0, detail: null },
      hands: { state: "ok" as const, consecutiveFailures: 0, detail: null },
      question: { state: "ok" as const, consecutiveFailures: 0, detail: null }
    }
  };
}

describe("buildSnapshot", () => {
  it("publishes the revision verbatim", () => {
    expect(buildSnapshot(input()).revision).toBe(7);
  });

  it("flattens the panelist map to an array sorted by participant id", () => {
    expect(buildSnapshot(input()).panelists.map((p) => p.participantId)).toEqual(["p1", "p2"]);
  });

  it("is deterministic across Map insertion order", () => {
    const a = input();
    const b = input();
    b.panelists = new Map([...b.panelists.entries()].reverse());
    expect(buildSnapshot(a)).toEqual(buildSnapshot(b));
  });

  /**
   * The invariant these must break on: a shallow copy. The engine keeps
   * mutating its live module state, and a surface may hold a snapshot across
   * ticks — a shared reference lets a later tick rewrite a snapshot that a
   * panel is mid-render on.
   */
  it("does not share the slot array with its caller", () => {
    const src = input();
    const snap = buildSnapshot(src);
    src.slots.push({ slot: 3, panelist: null });
    expect(snap.slots).toHaveLength(2);
  });

  it("does not share a seated panelist object with its caller", () => {
    const src = input();
    const snap = buildSnapshot(src);
    const seated = src.slots[0]?.panelist;
    if (seated) seated.displayName = "MUTATED";
    expect(snap.slots[0]?.panelist?.displayName).toBe("Bo");
  });

  it("does not share the queue arrays with its caller", () => {
    const src = input();
    const snap = buildSnapshot(src);
    src.queue.upcoming.push("9999");
    expect(snap.queue.upcoming).toEqual(["5555"]);
  });

  it("does not share the manual box assignments with its caller", () => {
    const src = input();
    const snap = buildSnapshot(src);
    src.manualBoxes[2] = 8;
    expect(snap.manualBoxes).toEqual({ 1: 4 });
  });

  it("does not share the gallery, tally, or overlay structures with its caller", () => {
    const src = input();
    const snap = buildSnapshot(src);
    src.gallery.push({ cell: 2, slot: 2 });
    src.tally.onAirSlots.push(1);
    src.overlays.nameplates.push({
      position: { kind: "host" },
      slot: 1,
      name: "X",
      location: "",
      tone: "neutral"
    });
    expect(snap.gallery).toHaveLength(1);
    expect(snap.tally.onAirSlots).toEqual([]);
    expect(snap.overlays.nameplates).toEqual([]);
  });
});
```

- [ ] **Step 2: Run and watch it fail**

Run: `cd show-engine && npx vitest run src/showSnapshot.test.ts`
Expected: FAIL — cannot resolve `./showSnapshot.js`.

- [ ] **Step 3: Write `showSnapshot.ts`**

- [ ] **Step 4: Run and watch it pass**

Run: `cd show-engine && npx vitest run src/showSnapshot.test.ts`
Expected: PASS, 8 tests.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/showSnapshot.ts show-engine/src/showSnapshot.test.ts
git commit -m "feat(show-engine): the published show snapshot"
```

---

### Task 4: The engine skeleton — construction, restore, and snapshot

**Files:**
- Create: `show-engine/src/showEngine.ts`
- Test: `show-engine/src/showEngine.test.ts`

**Interfaces:**
- Consumes: everything from Tasks 1–3, plus `ShowEngineConfig`, `LiveSlots`, `GalleryDirector`, `OverrideDb`, `MukanaRegistry`, `MukanaClient`, `ZoomIngest`, `ProgramBus`, `OverlayDirector`, `StateStore`, `FiloAssigner`, `VisibleSetAssigner`.
- Produces:

```ts
export type ShowEngineDeps = {
  config: ShowEngineConfig;
  host: HostAdapter;
  clock: Clock;
  store: StateStore;
  mukana?: MukanaClient;      // absent for a show with config.mukana === null
  assigner?: PositionAssigner; // defaults to FiloAssigner sized to config.capacity
};

export class ShowEngine {
  constructor(deps: ShowEngineDeps);
  snapshot(): ShowSnapshot;
  restore(): Promise<boolean>;   // true when a state file was loaded and applied
  revision(): number;
}
```

**Behavior:**

The constructor builds every module from `config` and wires nothing to the host yet. `LiveSlots` gets `{ capacity, utilityPinBase }`; `GalleryDirector` gets `{ cells: min(config.galleryCells, host.capabilities().maxGalleryCells) }` — a host that cannot render 16 cells must not be handed 16, and clamping at construction is the one place that decision belongs. `ProgramBus` gets `{ skipRoles: config.skipRoles }`.

`mukana` is optional. **A show with `config.mukana === null` must construct and run** — that is the case Plan 4 exists to serve. When absent, health for all three endpoints reports `{ state: "failing", consecutiveFailures: 0, detail: "no registry configured" }`, which `resolveCapabilities` maps to `disabled` for every integration the config also has switched off. Passing a `mukana` client while `config.mukana` is `null` is a programming error: throw with a message naming both.

`restore()` loads through the `StateStore`, returning `false` when it finds nothing. On a successful load it rebuilds `LiveSlots` and `GalleryDirector` via their `fromJSON` statics, restores the override table, and applies `manualBoxes` **only if `lookId` matches the currently selected look** — a restored assignment set that belongs to a different look is discarded, which is the persisted half of "cleared when the look changes". A `LiveSlotsRestoreError` or `GalleryError` from a structurally-bad file propagates: the file passed `StateStore`'s shallow check but is genuinely corrupt, and a show that silently starts with an empty roster because its state file was damaged is worse than one that refuses to start.

`snapshot()` calls `buildSnapshot` over current module state. Before the first tick it is fully valid: empty roster, `look: null`, `mode: "none"` tally, empty overlays, and capabilities resolved from current health.

`revision()` returns the tick counter, starting at 0.

- [ ] **Step 1: Write the failing test**

```ts
// show-engine/src/showEngine.test.ts
import { describe, expect, it } from "vitest";
import { ShowEngine } from "./showEngine.js";
import { MockHost } from "./mockHost.js";
import { StateStore } from "./persistence.js";
import { parseShowEngineConfig } from "./config.js";
import type { StateFs } from "./persistence.js";
import type { Clock } from "./clock.js";

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

const registryLess = parseShowEngineConfig({
  capacity: 8,
  statePath: "/state/show.json",
  galleryCells: 16,
  looks: [
    {
      id: "teatime",
      label: "Teatime",
      scenePreset: "scene-teatime",
      boxes: 2,
      includesHost: true,
      includesReader: false
    },
    {
      id: "banter",
      label: "Banter",
      scenePreset: "scene-banter",
      boxes: 1,
      includesHost: true,
      includesReader: true
    }
  ]
});

function engine(overrides: { host?: MockHost; fs?: StateFs } = {}) {
  const host = overrides.host ?? new MockHost();
  const fs = overrides.fs ?? memoryFs();
  return new ShowEngine({
    config: registryLess,
    host,
    clock: fixedClock(),
    store: new StateStore(registryLess.statePath, { fs })
  });
}

describe("ShowEngine construction", () => {
  /**
   * The invariant this must break on: requiring a Mukana client. A show with
   * no registry is the case the capability model exists to serve; if it cannot
   * even construct, none of that work is reachable.
   */
  it("constructs for a show with no Mukana at all", () => {
    expect(() => engine()).not.toThrow();
  });

  it("starts at revision zero with an empty, valid snapshot", () => {
    const snap = engine().snapshot();
    expect(snap.revision).toBe(0);
    expect(snap.panelists).toEqual([]);
    expect(snap.look).toBeNull();
    expect(snap.tally.mode).toBe("none");
    expect(snap.overlays).toEqual({ nameplates: [], question: null });
  });

  it("resolves every capability to disabled for a registry-less show", () => {
    const caps = engine().snapshot().capabilities;
    expect(caps.registry.state).toBe("disabled");
    expect(caps.handsQueue.state).toBe("disabled");
    expect(caps.questionFeed.state).toBe("disabled");
  });

  it("seats the roster to config capacity", () => {
    expect(engine().snapshot().slots).toHaveLength(8);
  });

  /**
   * The invariant this must break on: handing the host more gallery cells than
   * it declared it can render.
   */
  it("clamps the gallery to what the host can render", () => {
    const small = engine({ host: new MockHost({ maxGalleryCells: 4 }) });
    expect(small.snapshot().gallery).toHaveLength(4);
  });

  it("does not clamp upward when the host can render more than configured", () => {
    const big = engine({ host: new MockHost({ maxGalleryCells: 64 }) });
    expect(big.snapshot().gallery).toHaveLength(16);
  });

  it("refuses a Mukana client the config has no address for", () => {
    expect(
      () =>
        new ShowEngine({
          config: registryLess,
          host: new MockHost(),
          clock: fixedClock(),
          store: new StateStore(registryLess.statePath, { fs: memoryFs() }),
          // @ts-expect-error deliberately wrong: a client with no configured address
          mukana: {}
        })
    ).toThrow(/mukana/i);
  });

  it("emits no host commands before the first tick", () => {
    const host = new MockHost();
    engine({ host });
    expect(host.calls()).toEqual([]);
  });
});

describe("ShowEngine.restore", () => {
  it("returns false when there is no state file", async () => {
    expect(await engine().restore()).toBe(false);
  });

  it("keeps manual box assignments that belong to the current look", async () => {
    const fs = memoryFs({
      "/state/show.json": JSON.stringify({
        version: 3,
        slots: { version: 1, capacity: 8, seats: new Array(8).fill(null) },
        overrides: {},
        gallery: { version: 1, cells: 16, assignments: [] },
        manualBoxes: { 1: 3 },
        lookId: "teatime"
      })
    });
    const e = engine({ fs });
    e.setLook("teatime");
    expect(await e.restore()).toBe(true);
    expect(e.snapshot().manualBoxes).toEqual({ 1: 3 });
  });

  /**
   * The invariant this must break on: restoring manual assignments across a
   * look change. Box 1 of one arrangement is not box 1 of another; inheriting
   * them puts the wrong person in the wrong window on a live show.
   */
  it("discards manual box assignments that belong to a different look", async () => {
    const fs = memoryFs({
      "/state/show.json": JSON.stringify({
        version: 3,
        slots: { version: 1, capacity: 8, seats: new Array(8).fill(null) },
        overrides: {},
        gallery: { version: 1, cells: 16, assignments: [] },
        manualBoxes: { 1: 3 },
        lookId: "some-other-look"
      })
    });
    const e = engine({ fs });
    e.setLook("teatime");
    expect(await e.restore()).toBe(true);
    expect(e.snapshot().manualBoxes).toEqual({});
  });

  it("rejects a version-2 file as no state at all", async () => {
    const fs = memoryFs({
      "/state/show.json": JSON.stringify({
        version: 2,
        slots: { version: 1, capacity: 8, seats: new Array(8).fill(null) },
        overrides: {},
        gallery: { version: 1, cells: 16, assignments: [] }
      })
    });
    expect(await engine({ fs }).restore()).toBe(false);
  });
});
```

Note: `setLook` arrives in Task 6. Until then these two `restore` tests will not compile — that is expected and correct for TDD. Implement `setLook(lookId: string): void` as a minimal setter in this task (it selects the active look definition by id and throws on an unknown id); Task 6 gives it its tick behavior.

- [ ] **Step 2: Run and watch it fail**

Run: `cd show-engine && npx vitest run src/showEngine.test.ts`
Expected: FAIL — cannot resolve `./showEngine.js`.

- [ ] **Step 3: Write `showEngine.ts`**

Constructor, `snapshot()`, `restore()`, `revision()`, and the minimal `setLook()`. No tick yet.

- [ ] **Step 4: Run and watch it pass**

Run: `cd show-engine && npx vitest run src/showEngine.test.ts && npm run typecheck:tests`
Expected: PASS, 12 tests.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/showEngine.ts show-engine/src/showEngine.test.ts
git commit -m "feat(show-engine): engine construction, restore, and snapshot"
```

---

### Task 5: Roster intake and the seating tick

**Files:**
- Modify: `show-engine/src/showEngine.ts`
- Test: `show-engine/src/showEngine.test.ts` (extend)

**Interfaces:**
- Produces, added to `ShowEngine`:

```ts
onZoomEvent(event: ZoomEvent): void;
onMukanaPanelists(outcome: MukanaOutcome): void;
setOverride(record: OverrideRecord): void;
clearOverride(personKey: PersonKey): void;
tick(): Promise<ShowSnapshot>;
```

`setOverride` writes the row with `OverrideDb.set(record)`. When `record.role`
is in `EXCLUSIVE_ROLES` (`"host"` or `"reader"`) it then calls
`OverrideDb.assignExclusiveRole(record.personKey, record.role, registry.current())`
so the single-host/single-reader invariant holds — that method is the only
thing that demotes a prior holder, and `set` alone would leave two hosts. Pass
the live registry; for a registry-less show that is an empty object, and
exclusivity is enforced across the override table alone, which is already the
documented behavior.

**Behavior:**

`onZoomEvent` forwards to `ZoomIngest.apply` and returns immediately — it does no seating. Host events arrive at frame rate; seating happens once per tick. This is the same discipline the CVP shell learned the hard way with `RefreshSurfaceBindings` (see CLAUDE.md's 0xc000027b section): never rebuild a structure at event rate when a coalesced tick will do.

`tick()` is the heartbeat and runs in this fixed order:

1. `ZoomIngest.commit()`. If it returns `false` and no other input changed, the roster half of the tick is skipped — but the tick still proceeds to the derived layers, because a look change or a capability change can alter output with a static roster.
2. `buildPanelistDb(participants, registry.current(), overrides.entries())`.
3. Seat: `LiveSlots.refresh(db)` for an unchanged participant set; `LiveSlots.rebuild(...)` only when the participant id set actually changed. `refresh` holds seats still, which is what an operator expects — a guest toggling their camera must not reseat the room. **`rebuild`'s return value is the overflow list and must never be discarded**: seat what fits, and surface the rest on the snapshot rather than letting people silently vanish.
4. Recompute the derived layers (Tasks 6–7).
5. Persist, debounced (below).
6. Emit host commands (Task 8).
7. Increment the revision, build and return the snapshot.

`rebuild` seats in the order of the array it is handed. The engine sorts panelists by `participantId` before rebuilding so seating is deterministic across runs and across the two hosts. Sorting by name would reshuffle the room whenever someone renamed themselves mid-show.

**Persistence is debounced through the injected clock**: after a change, save on the first tick at least `SAVE_DEBOUNCE_MS` (1000) after the previous save. No timers — the engine compares `clock.now()` against the last save time, so a test drives it by advancing a fake clock rather than waiting.

- [ ] **Step 1: Write the failing tests**

Append to `show-engine/src/showEngine.test.ts`:

```ts
function joined(id: string, name: string): ZoomEvent {
  return {
    kind: "joined",
    participant: {
      participantId: id,
      rawName: name,
      online: true,
      videoOn: true,
      audioOn: true,
      handRaised: false,
      zoomRole: 0
    }
  };
}

describe("ShowEngine roster tick", () => {
  it("does not seat anyone until a tick runs", () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    expect(e.snapshot().slots.every((s) => s.panelist === null)).toBe(true);
  });

  it("seats the roster on tick and bumps the revision", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    const snap = await e.tick();
    expect(snap.revision).toBe(1);
    expect(snap.slots[0]?.panelist?.displayName).toBe("Ann");
  });

  it("seats in participant-id order regardless of arrival order", async () => {
    const e = engine();
    e.onZoomEvent(joined("p3", "Cy"));
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    const snap = await e.tick();
    expect(snap.slots.slice(0, 3).map((s) => s.panelist?.participantId)).toEqual([
      "p1",
      "p2",
      "p3"
    ]);
  });

  /**
   * The invariant this must break on: calling rebuild when only a property
   * changed. A guest toggling their camera must not reseat the room.
   */
  it("holds seats still when a participant toggles video", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    e.onZoomEvent({ kind: "video", participantId: "p1", on: false });
    const snap = await e.tick();
    expect(snap.slots[0]?.panelist?.participantId).toBe("p1");
    expect(snap.slots[0]?.panelist?.videoOn).toBe(false);
    expect(snap.slots[1]?.panelist?.participantId).toBe("p2");
  });

  /**
   * A Zoom departure does NOT vacate a seat. Owner ruling, 2026-08-06: the seat
   * is held and flagged offline, so a connection blip does not drop a panelist
   * off air and a reconnect returns them to the SAME slot. Clearing a seat is an
   * explicit operator action, exactly as it was in the Isadora patch this ports.
   *
   * This matches the two modules underneath: `ZoomIngest` keeps a departed
   * participant marked offline "so they can be restored on reconnect"
   * (zoomIngest.ts:52-55), and `LiveSlots.refresh` documents that a vanished
   * participant "keeps their seat but is marked offline — visibly gone rather
   * than silently dropped" (liveSlots.ts:114-118).
   *
   * The invariant this must break on: any code that vacates a seat on an
   * offline flag. That makes a reconnecting guest permanently unseatable,
   * because a departure never changes the id set and `refresh` only re-pulls
   * ALREADY-seated panelists.
   */
  it("holds the seat and marks it offline when someone leaves", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    e.onZoomEvent({ kind: "left", participantId: "p1" });
    const snap = await e.tick();
    expect(snap.slots[0]?.panelist?.participantId).toBe("p1");
    expect(snap.slots[0]?.panelist?.online).toBe(false);
    expect(snap.slots[1]?.panelist?.participantId).toBe("p2");
  });

  it("returns a reconnecting panelist to the same slot", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    e.onZoomEvent({ kind: "left", participantId: "p1" });
    await e.tick();
    e.onZoomEvent(joined("p1", "Ann"));
    const snap = await e.tick();
    expect(snap.slots[0]?.panelist?.participantId).toBe("p1");
    expect(snap.slots[0]?.panelist?.online).toBe(true);
  });

  /**
   * The refresh-vs-rebuild branch, pinned in BOTH directions. Mutation testing
   * showed the committed suite passed under "always rebuild" AND under "always
   * refresh" — the second means a mid-show join is never seated and nothing
   * fails. Every roster test staged its joins before the first tick.
   */
  it("seats a guest who joins after the first tick", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    await e.tick();
    e.onZoomEvent(joined("p2", "Bo"));
    const snap = await e.tick();
    expect(snap.slots[1]?.panelist?.participantId).toBe("p2");
  });

  /**
   * The invariant this must break on: discarding rebuild's return value.
   * Overflow that is dropped silently means people vanish from a live show
   * with nothing anywhere reporting it.
   */
  it("reports panelists that did not fit rather than dropping them", async () => {
    const e = engine();
    const ids: string[] = [];
    for (let i = 1; i <= 10; i += 1) {
      ids.push(`p${i}`);
      e.onZoomEvent(joined(`p${i}`, `Guest ${i}`));
    }
    const snap = await e.tick();
    const seated = snap.slots.flatMap((s) => (s.panelist ? [s.panelist.participantId] : []));
    expect(seated).toHaveLength(8);
    expect(snap.unseated).toHaveLength(2);
    // Nobody vanishes: seated ∪ unseated is the whole roster, with no overlap.
    expect([...seated, ...snap.unseated.map((p) => p.participantId)].sort()).toEqual(
      [...ids].sort()
    );
  });

  it("debounces persistence against the injected clock", async () => {
    let t = 1000;
    const fs = memoryFs();
    const writes: string[] = [];
    const counting: StateFs = {
      ...fs,
      writeFile: async (p, c) => {
        writes.push(p);
        return fs.writeFile(p, c);
      }
    };
    const e = new ShowEngine({
      config: registryLess,
      host: new MockHost(),
      clock: { now: () => t },
      store: new StateStore(registryLess.statePath, { fs: counting })
    });
    e.onZoomEvent(joined("p1", "Ann"));
    await e.tick();
    const afterFirst = writes.length;
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    expect(writes.length).toBe(afterFirst);
    t += 1001;
    e.onZoomEvent(joined("p3", "Cy"));
    await e.tick();
    expect(writes.length).toBeGreaterThan(afterFirst);
  });
});
```

`ShowSnapshot` gains one field for this task: `unseated: Panelist[]`. Add it to `showSnapshot.ts` and its `buildSnapshot` input, deep-copied like the rest, defaulting to an empty array.

- [ ] **Step 2: Run and watch them fail**

Run: `cd show-engine && npx vitest run src/showEngine.test.ts`
Expected: FAIL — `tick` is not a function.

- [ ] **Step 3: Implement intake, the seating tick, and debounced persistence**

- [ ] **Step 4: Run and watch them pass**

Run: `cd show-engine && npx vitest run && npm run typecheck:tests`
Expected: all green.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/showEngine.ts show-engine/src/showSnapshot.ts show-engine/src/showEngine.test.ts show-engine/src/showSnapshot.test.ts
git commit -m "feat(show-engine): roster intake and the seating tick"
```

---

### Task 6: The active-speaker dispatch gate

**This closes the obligation carried from Plan 3.** `shouldFollowSpeaker` has existed since Plan 3 and nothing has ever called it before a position assigner. The concrete defect it was written to prevent is live today: an ASL interpreter signing continuously while a panelist talks evicts that panelist from the FILO pool, swaps into the visible gallery window, and sorts to the front of the gallery. This task is the wiring, and its property test is the proof.

**Files:**
- Modify: `show-engine/src/showEngine.ts`
- Test: `show-engine/src/speakerGateDispatch.test.ts`

**Interfaces:**
- Consumes: `shouldFollowSpeaker(role: Role | null, skipRoles: readonly Role[]): boolean` from `speakerGate.js`; `PositionAssigner` from `speakerRecency.js`.
- Produces, added to `ShowEngine`:

```ts
onActiveSpeaker(participantId: string): void;
setActiveSpeakerFollow(on: boolean): void;
```

**Behavior:**

`onActiveSpeaker` records the participant id as the pending speaker and returns. It does **not** resolve a role or dispatch anything.

The gate runs inside `tick()`, immediately after the panelist database is rebuilt and before any derived layer:

1. Take the pending speaker id, if any, and clear it.
2. Resolve their role: `panelists.get(id)?.role ?? null`.
3. `if (!shouldFollowSpeaker(role, config.skipRoles)) return;` — **before the assigner, before `ProgramBus`, before anything else touches the id.**
4. Otherwise dispatch: `assigner.onActiveSpeaker(id)` and `programBus.onActiveSpeaker(id, role ?? "panelist")`.

Gating at tick rather than at intake is deliberate. Role resolution needs the panelist database, and at intake the database is one tick stale — at startup it is empty, so an interpreter's very first event would resolve to a `null` role and pass the gate. Gating after the rebuild means the gate always sees current editorial roles, including one the operator assigned seconds earlier.

`role ?? "panelist"` in the dispatch is safe **only** because it sits after the gate: an unknown speaker has already been evaluated as `null` by `shouldFollowSpeaker`, which returns `true` for a role not in the skip list. Never move that coalesce above the gate — it would convert "unknown role" into "definitely follow" before the gate could see it.

- [ ] **Step 1: Write the failing test**

```ts
// show-engine/src/speakerGateDispatch.test.ts
import { describe, expect, it } from "vitest";
import { ShowEngine } from "./showEngine.js";
import { MockHost } from "./mockHost.js";
import { StateStore } from "./persistence.js";
import { parseShowEngineConfig } from "./config.js";
import { FiloAssigner, VisibleSetAssigner } from "./speakerRecency.js";
import { ROLES } from "./contracts.js";
import type { PlacementChange, PositionAssigner } from "./speakerRecency.js";
import type { OverrideRecord } from "./overrideDb.js";
import type { Role } from "./contracts.js";
import type { StateFs } from "./persistence.js";
import type { ZoomEvent } from "./zoomIngest.js";

/** Wraps a real assigner and records every dispatch that reaches it. */
class RecordingAssigner implements PositionAssigner {
  readonly seen: string[] = [];
  readonly changes: PlacementChange[] = [];
  constructor(private readonly inner: PositionAssigner) {}
  onActiveSpeaker(participantId: string): PlacementChange[] {
    this.seen.push(participantId);
    const out = this.inner.onActiveSpeaker(participantId);
    this.changes.push(...out);
    return out;
  }
  positions(): Map<number, string> {
    return this.inner.positions();
  }
  reset(participantIds: readonly string[]): void {
    this.inner.reset(participantIds);
  }
}

function memoryFs(): StateFs {
  const files = new Map<string, string>();
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

const config = parseShowEngineConfig({
  capacity: 8,
  statePath: "/state/show.json",
  looks: [
    {
      id: "teatime",
      label: "Teatime",
      scenePreset: "scene-teatime",
      boxes: 2,
      includesHost: true,
      includesReader: false
    }
  ]
});

function joined(id: string, name: string): ZoomEvent {
  return {
    kind: "joined",
    participant: {
      participantId: id,
      rawName: name,
      online: true,
      videoOn: true,
      audioOn: true,
      handRaised: false,
      zoomRole: 0
    }
  };
}

function override(personKey: string, role: Role): OverrideRecord {
  return { personKey, displayName: "Ann", location: "", role };
}

function build(assigner: PositionAssigner) {
  return new ShowEngine({
    config,
    host: new MockHost(),
    clock: { now: () => 1000 },
    store: new StateStore(config.statePath, { fs: memoryFs() }),
    assigner
  });
}

/**
 * THE PLAN 3 OBLIGATION, discharged as a property.
 *
 * Quantified over, deliberately — a property that holds only at one point is
 * how Plan 4's degradation bug survived 404 tests:
 *   - every role in ROLES (5), so adding a role to skipRoles cannot silently
 *     escape coverage;
 *   - both shipped assigner types, since FILO evicts and VisibleSet swaps and
 *     the bug looks different in each;
 *   - active-speaker follow both on and off, since the gate must not be
 *     accidentally implemented as a side effect of the follow flag.
 *
 * The invariant it must break on: any dispatch to a POSITION ASSIGNER for a
 * role in skipRoles. If the engine's gate is removed, or reordered below the
 * dispatch, `recorder.seen` is non-empty for "aslinterpreter" in every
 * combination and this fails.
 *
 * Honest scoping of the ProgramBus assertion below: it is CONFIRMATORY, not
 * independent. `ProgramBus.onActiveSpeaker` (programBus.ts:76-79) applies
 * `shouldFollowSpeaker` internally before touching `activeSpeakerId`, so that
 * half would still pass with the engine's gate entirely absent. The assigner
 * assertions are the real proof — ProgramBus's internal gate does not protect
 * a position assigner, which is exactly why the engine needs its own. Keep
 * both, but do not mistake the ProgramBus half for a second guarantee.
 */
describe("active-speaker dispatch gate", () => {
  const assigners: Array<[string, () => PositionAssigner]> = [
    ["FiloAssigner", () => new FiloAssigner({ capacity: 4 })],
    ["VisibleSetAssigner", () => new VisibleSetAssigner({ capacity: 8, visible: 4 })]
  ];

  for (const [assignerName, make] of assigners) {
    for (const role of ROLES) {
      for (const follow of [true, false]) {
        const skipped = config.skipRoles.includes(role);
        const verb = skipped ? "blocks" : "dispatches";

        it(`${verb} a ${role} speaker with ${assignerName}, follow=${follow}`, async () => {
          const recorder = new RecordingAssigner(make());
          const e = build(recorder);
          e.onZoomEvent(joined("p1", "Ann"));
          e.onZoomEvent(joined("p2", "Bo"));
          await e.tick();
          const key = e.snapshot().panelists.find((p) => p.participantId === "p1")?.personKey;
          expect(key).toBeDefined();
          if (key !== undefined) e.setOverride(override(key, role));
          e.setActiveSpeakerFollow(follow);
          await e.tick();

          recorder.seen.length = 0;
          recorder.changes.length = 0;
          e.onActiveSpeaker("p1");
          const snap = await e.tick();

          if (skipped) {
            expect(recorder.seen).toEqual([]);
            expect(recorder.changes).toEqual([]);
            expect(snap.program.activeSpeakerId).toBeNull();
          } else {
            expect(recorder.seen).toEqual(["p1"]);
            expect(snap.program.activeSpeakerId).toBe("p1");
          }
        });
      }
    }
  }

  it("follows a speaker whose role is unknown to the roster", async () => {
    const recorder = new RecordingAssigner(new FiloAssigner({ capacity: 4 }));
    const e = build(recorder);
    e.onActiveSpeaker("ghost");
    await e.tick();
    expect(recorder.seen).toEqual(["ghost"]);
  });

  it("drops a pending speaker after one tick rather than replaying it", async () => {
    const recorder = new RecordingAssigner(new FiloAssigner({ capacity: 4 }));
    const e = build(recorder);
    e.onZoomEvent(joined("p1", "Ann"));
    e.onActiveSpeaker("p1");
    await e.tick();
    recorder.seen.length = 0;
    await e.tick();
    expect(recorder.seen).toEqual([]);
  });

  it("keeps only the latest speaker when several arrive within one tick", async () => {
    const recorder = new RecordingAssigner(new FiloAssigner({ capacity: 4 }));
    const e = build(recorder);
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    recorder.seen.length = 0;
    e.onActiveSpeaker("p1");
    e.onActiveSpeaker("p2");
    await e.tick();
    expect(recorder.seen).toEqual(["p2"]);
  });
});
```

- [ ] **Step 2: Run and watch it fail**

Run: `cd show-engine && npx vitest run src/speakerGateDispatch.test.ts`
Expected: FAIL — `onActiveSpeaker` is not a function on `ShowEngine`.

- [ ] **Step 3: Implement the gate**

- [ ] **Step 4: Run and watch it pass**

Run: `cd show-engine && npx vitest run src/speakerGateDispatch.test.ts`
Expected: PASS, 23 tests (20 from the matrix, 3 standalone).

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/showEngine.ts show-engine/src/speakerGateDispatch.test.ts
git commit -m "feat(show-engine): gate active-speaker dispatch on the skip-roles rule"
```

---

### Task 7: Look resolution, the capability pairing, and the derived layers

**Files:**
- Modify: `show-engine/src/showEngine.ts`
- Test: `show-engine/src/showEngine.test.ts` (extend)

**Interfaces:**
- Consumes: `clampPage`, `resolveLook`, `effectiveBoxFill`, `pageCountFor` from `lookDirector.js`; `deriveTally`, `tallyEquals` from `tallyPublisher.js`; `OverlayDirector` from `overlayDirector.js`; `resolveCapabilities`, `canUse` from `capabilities.js`; `stripChairs`, `queueOrder` from `handsQueue.js`; `findChairSlots` from `lookDirector.js`.
- Produces, added to `ShowEngine`:

```ts
setLook(lookId: string): void;         // full behavior; Task 4 shipped the setter only
nextGuest(): void;
prevGuest(): void;
assignBox(box: number, slot: number): void;
clearBox(box: number): void;
setQuestionVisible(on: boolean): void;
setPreview(source: ProgramSource): void;
cut(): void;
auto(transitionId?: string): void;
directCut(source: ProgramSource): void;
```

**Behavior:**

The derived half of `tick()`, in order, after the speaker gate:

1. **Resolve capabilities once**: `const caps = resolveCapabilities(config, health)`. One value per tick, used everywhere. Resolving twice within a tick is how the two halves of a decision drift apart.
2. **Strip chairs from the queue**: `stripChairs(rawQueue, { hostPin, readerPin })` where the chair PINs come from the seated host and reader.
3. **Clamp the page, then resolve the look — with the same capability object passed to both.**

```ts
const page = clampPage(look, queue, this.page, caps.handsQueue);
const resolution = resolveLook(look, {
  queue, slots, page, handsQueue: caps.handsQueue, manualBoxes: this.manualBoxes
});
```

**This is the Plan 4 obligation, and it is the whole reason both parameters are optional rather than required.** `clampPage` and `resolveLook` each default an omitted capability to unusable. Passing `available` to one and omitting it on the other silently pins the operator to page 0 — no crash, no error, just a paging control that does nothing. The two calls above are the only place in the package where both are invoked, and they must always be adjacent and always share `caps.handsQueue`. Never introduce a third caller without the same pairing.

4. **Program**: when the active look changed, set `ProgramBus`'s preview or program to `{ kind: "look", lookId: look.id }`. `resolveLook` does not speak `ProgramSource` — the engine constructs it.
5. **Tally**: `deriveTally({ source: bus.program, slots, gallery, look: resolution, activeSpeakerSlot })` where `activeSpeakerSlot = liveSlots.slotOf(bus.activeSpeakerId)` — the engine owns this translation; no module does it.
6. **Overlays**: `overlayDirector.update({ look: resolution, question, questionVisible })`, keeping its returned change flag for Task 8's emission decision.

**Paging under manual fill.** `nextGuest`/`prevGuest` adjust the page only when `effectiveBoxFill(look, caps.handsQueue)` is `"queue"`. Under `"manual"` they are **inert and record a refusal reason on the snapshot** (`pagingRefused: string | null`) rather than throwing or silently doing nothing — spec §4 requires a typed refusal. Plan 6 turns that into the action-layer's refusal response.

**Box assignment.** `assignBox(box, slot)` and `clearBox(box)` mutate `manualBoxes`. `setLook` to a *different* look clears `manualBoxes` entirely (spec §3.2); `setLook` to the same look leaves them alone, so an idempotent re-select does not wipe the operator's work.

**Transport degradation.** When `host.capabilities().hasPreviewBus` is `false`, `setPreview` is a no-op and `cut`/`auto` route to `directCut(previewSource)`. The engine must never emit a `setPreview` or `cut` to a host that declared no preview bus.

`ShowSnapshot` gains `pagingRefused: string | null`.

- [ ] **Step 1: Write the failing tests**

```ts
describe("ShowEngine derived layers", () => {
  it("resolves the selected look and reports it on the snapshot", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.setLook("teatime");
    const snap = await e.tick();
    expect(snap.look?.lookId).toBe("teatime");
    expect(snap.look?.scenePreset).toBe("scene-teatime");
  });

  it("rejects an unknown look id", () => {
    expect(() => engine().setLook("nope")).toThrow(/nope/);
  });

  it("derives tally from the program source and the resolved look", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.setLook("teatime");
    await e.tick();
    e.directCut({ kind: "look", lookId: "teatime" });
    const snap = await e.tick();
    expect(snap.tally.mode).toBe("look");
  });

  it("translates the active speaker to a roster slot for tally", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    e.directCut({ kind: "activeSpeaker" });
    e.setActiveSpeakerFollow(true);
    e.onActiveSpeaker("p2");
    const snap = await e.tick();
    expect(snap.tally.mode).toBe("activeSpeaker");
    expect(snap.tally.onAirSlots).toEqual([2]);
  });

  it("clears manual box assignments when the look changes", async () => {
    const e = engine();
    e.setLook("teatime");
    e.assignBox(1, 3);
    await e.tick();
    expect(e.snapshot().manualBoxes).toEqual({ 1: 3 });
    e.setLook("banter");
    expect(e.snapshot().manualBoxes).toEqual({});
  });

  it("keeps manual box assignments when the same look is re-selected", async () => {
    const e = engine();
    e.setLook("teatime");
    e.assignBox(1, 3);
    e.setLook("teatime");
    expect(e.snapshot().manualBoxes).toEqual({ 1: 3 });
  });

  it("refuses paging under manual fill instead of throwing", async () => {
    const e = engine();
    e.setLook("teatime");
    await e.tick();
    expect(() => e.nextGuest()).not.toThrow();
    const snap = await e.tick();
    expect(snap.page).toBe(0);
    expect(snap.pagingRefused).toMatch(/manual/i);
  });

  /**
   * Transport degradation. The invariant this must break on: emitting a
   * preview or cut to a host that declared it has no preview bus.
   */
  it("routes cut to a direct cut on a host with no preview bus", async () => {
    const host = new MockHost({ hasPreviewBus: false });
    const e = engine({ host });
    e.setLook("teatime");
    await e.tick();
    host.clear();
    e.setPreview({ kind: "look", lookId: "teatime" });
    e.cut();
    await e.tick();
    expect(host.callsOfKind("setPreview")).toEqual([]);
    expect(host.callsOfKind("cut")).toEqual([]);
    expect(e.snapshot().program.program).toEqual({ kind: "look", lookId: "teatime" });
  });
});

/**
 * THE PLAN 4 OBLIGATION, discharged as a property.
 *
 * Quantified over — and the quantification is the point, because Plan 4's own
 * property held page constant at 0 and that is exactly where its bug hid:
 *   - page over -2..pageCount+2, including both out-of-range directions;
 *   - all three capability states for handsQueue;
 *   - both boxFill strategies.
 *
 * The invariant it must break on: clampPage and resolveLook receiving
 * different capability values, or either being called without one. Any of
 * those makes some (page, capability) pair throw out of tick().
 */
describe("clampPage and resolveLook always agree", () => {
  const states = ["available", "unavailable", "disabled"] as const;

  for (const fill of ["queue", "manual"] as const) {
    for (const state of states) {
      for (const page of [-2, -1, 0, 1, 2, 3, 4]) {
        it(`survives page ${page} with ${fill} fill and a ${state} hands queue`, async () => {
          const e = engineWithFill(fill, state);
          e.setLook("teatime");
          e.setPage(page);
          await expect(e.tick()).resolves.toBeDefined();
          const snap = e.snapshot();
          expect(snap.page).toBeGreaterThanOrEqual(0);
          expect(snap.page).toBeLessThan(snap.look?.pageCount ?? 1);
        });
      }
    }
  }
});
```

`engineWithFill(fill, state)` is a local helper building an engine whose `teatime` look carries the given `boxFill`, whose hands capability resolves to the given state, and whose queue holds enough PINs to span three pages. `setPage(n)` is a test-facing setter on `ShowEngine` that writes the pending page without clamping — it must exist so the property can drive an out-of-range page the way a stale operator page would. Document it as such on the method.

- [ ] **Step 2: Run and watch them fail**

Run: `cd show-engine && npx vitest run src/showEngine.test.ts`
Expected: FAIL — `setPage`, `nextGuest`, `assignBox` are not functions.

- [ ] **Step 3: Implement the derived layers**

- [ ] **Step 4: Run and watch them pass**

Run: `cd show-engine && npx vitest run && npm run typecheck:tests`
Expected: all green, including 42 property cases.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/showEngine.ts show-engine/src/showSnapshot.ts show-engine/src/showEngine.test.ts
git commit -m "feat(show-engine): look resolution, tally, overlays, and the capability pairing"
```

---

### Task 8: Host command emission

**Files:**
- Modify: `show-engine/src/showEngine.ts`
- Test: `show-engine/src/showEngine.test.ts` (extend)

**Interfaces:** no new public methods. Emission happens inside `tick()`.

**Behavior:**

At the end of every tick the engine emits host commands for **what changed, and only what changed**. It keeps a private copy of the last value it sent for each command and compares before emitting:

- `assignSlot(slot, participantId | null)` — per slot whose occupant changed. Never a full sweep of every slot on every tick.
- `applyLook(lookId, boxes)` — when the look id or any box assignment changed.
- `setGallery(cells)` — when any cell changed.
- `setNameplates(plates)` — driven by `OverlayDirector.update`'s returned change flag, which is already the module's own change detection. Do not re-derive it.
- `setQuestion(question)` — same flag.
- `setPreview` / `cut` / `auto` — emitted on operator action, not on tick, and suppressed entirely on a host with no preview bus (Task 7).

Diffing matters for the same reason it does in the CVP shell: a host adapter turns each of these into real work — a Show Input rebind, a scene-preset swap, an overlay re-raster. Re-emitting an unchanged value at tick rate is the churn class that CLAUDE.md documents as tripping a native fail-fast on a long show. The engine must be quiet when nothing changed.

**On the first tick the engine emits the full current state**, because the host starts with no knowledge. "Only what changed" is measured against what this engine has sent, not against a default.

- [ ] **Step 1: Write the failing tests**

```ts
describe("ShowEngine host emission", () => {
  it("emits the full picture on the first tick", async () => {
    const host = new MockHost();
    const e = engine({ host });
    e.onZoomEvent(joined("p1", "Ann"));
    e.setLook("teatime");
    await e.tick();
    expect(host.callsOfKind("assignSlot").length).toBeGreaterThan(0);
    expect(host.callsOfKind("applyLook")).toHaveLength(1);
  });

  /**
   * The invariant this must break on: emitting unchanged values every tick.
   * A host adapter turns each command into real work — a source rebind, a
   * scene swap, an overlay re-raster — and re-emitting at tick rate is the
   * churn class that trips native fail-fasts on a long show.
   */
  it("emits nothing when a tick changes nothing", async () => {
    const host = new MockHost();
    const e = engine({ host });
    e.onZoomEvent(joined("p1", "Ann"));
    e.setLook("teatime");
    await e.tick();
    host.clear();
    await e.tick();
    await e.tick();
    expect(host.calls()).toEqual([]);
  });

  it("emits only the slot that actually changed", async () => {
    const host = new MockHost();
    const e = engine({ host });
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    host.clear();
    e.onZoomEvent({ kind: "left", participantId: "p2" });
    await e.tick();
    expect(host.callsOfKind("assignSlot")).toEqual([
      { kind: "assignSlot", slot: 2, participantId: null }
    ]);
  });

  it("does not re-emit nameplates when the overlay is unchanged", async () => {
    const host = new MockHost();
    const e = engine({ host });
    e.onZoomEvent(joined("p1", "Ann"));
    e.setLook("teatime");
    await e.tick();
    host.clear();
    e.onZoomEvent({ kind: "audio", participantId: "p1", on: false });
    await e.tick();
    expect(host.callsOfKind("setNameplates")).toEqual([]);
  });

  it("re-emits nameplates when a display name changes", async () => {
    const host = new MockHost();
    const e = engine({ host });
    e.onZoomEvent(joined("p1", "Ann"));
    e.setLook("teatime");
    await e.tick();
    host.clear();
    e.onZoomEvent({ kind: "renamed", participantId: "p1", rawName: "Annette | Oslo" });
    await e.tick();
    expect(host.callsOfKind("setNameplates")).toHaveLength(1);
  });
});
```

- [ ] **Step 2: Run and watch them fail**

Run: `cd show-engine && npx vitest run src/showEngine.test.ts`
Expected: FAIL — the engine emits nothing, or emits every tick.

- [ ] **Step 3: Implement diffed emission**

- [ ] **Step 4: Run and watch them pass**

Run: `cd show-engine && npx vitest run`
Expected: all green.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/showEngine.ts show-engine/src/showEngine.test.ts
git commit -m "feat(show-engine): emit host commands only for what changed"
```

---

### Task 9: Mukana polling and per-tick capability resolution

**Files:**
- Modify: `show-engine/src/showEngine.ts`
- Test: `show-engine/src/showEngine.test.ts` (extend)

**Interfaces:** no new public methods. `tick()` becomes responsible for scheduling polls.

**Behavior:**

When a `MukanaClient` is present, `tick()` decides per endpoint whether a poll is due: due when `clock.now() - lastPollAt[endpoint] >= client.nextDelayMs(endpoint)`. `nextDelayMs` already folds the configured interval and the backoff-after-failure ceiling, so the engine adds no policy of its own — it only supplies the clock the client deliberately does not read.

**A poll must never block the tick.** The engine starts the fetch and applies its outcome to the *next* tick that finds it settled; a slow or hanging registry cannot stall the show loop. This is spec §2's normative rule — no external integration failure may block a tick — and it is the reason the whole capability model exists. Concretely: keep an in-flight promise per endpoint, and never `await` it inside `tick()` beyond checking whether it has already settled.

Outcomes apply as follows. `{ kind: "data" }` merges into `MukanaRegistry` (panelists), replaces the raw queue (hands), or replaces the current question. `{ kind: "dormant" }` and `{ kind: "invalid" }` **leave last-good data in place** — the client has already recorded the health transition, and discarding good data because one poll failed is precisely the incident this design prevents.

Capabilities resolve once per tick from `client.health` (or the registry-less fallback from Task 4), and that one value flows to every consumer, as established in Task 7.

- [ ] **Step 1: Write the failing tests**

```ts
describe("ShowEngine Mukana polling", () => {
  it("polls an endpoint only once its next delay has elapsed", async () => {
    const { e, fetches, advance } = mukanaEngine();
    await e.tick();
    const first = fetches.length;
    await e.tick();
    expect(fetches.length).toBe(first);
    advance(10_000);
    await e.tick();
    expect(fetches.length).toBeGreaterThan(first);
  });

  /**
   * The invariant this must break on: awaiting a fetch inside tick(). A hung
   * registry must not stall the show loop — spec §2, normative.
   */
  it("completes a tick while a fetch is still in flight", async () => {
    const { e, resolveHands } = mukanaEngine({ hangHands: true });
    await expect(e.tick()).resolves.toBeDefined();
    await expect(e.tick()).resolves.toBeDefined();
    resolveHands();
  });

  it("keeps the last good queue when a poll comes back invalid", async () => {
    const { e, advance, setHandsBody } = mukanaEngine();
    setHandsBody("4242\n5555\nNONE");
    advance(10_000);
    await e.tick();
    await e.tick();
    const good = e.snapshot().queue;
    setHandsBody("<html>gateway timeout</html>");
    advance(10_000);
    await e.tick();
    await e.tick();
    expect(e.snapshot().queue).toEqual(good);
  });

  it("moves the hands capability to unavailable when the feed fails", async () => {
    const { e, advance, failHands } = mukanaEngine();
    advance(10_000);
    await e.tick();
    await e.tick();
    expect(e.snapshot().capabilities.handsQueue.state).toBe("available");
    failHands();
    advance(10_000);
    await e.tick();
    await e.tick();
    const cap = e.snapshot().capabilities.handsQueue;
    expect(cap.state).toBe("unavailable");
    expect(cap.detail).not.toBeNull();
  });

  it("reports dormant as unavailable with the operator-facing detail", async () => {
    const { e, advance, dormantHands } = mukanaEngine();
    dormantHands();
    advance(10_000);
    await e.tick();
    await e.tick();
    expect(e.snapshot().capabilities.handsQueue.state).toBe("unavailable");
  });
});
```

`mukanaEngine(options?)` is a local helper returning `{ e, fetches, advance, setHandsBody, failHands, dormantHands, resolveHands }`: an engine wired to a `MukanaClient` over a controllable `FetchLike`, a mutable clock, and a recorded list of fetched URLs. Its config enables all three integrations and supplies a `mukana` block. `hangHands: true` makes the hands fetch return a promise that never settles until `resolveHands()` is called.

- [ ] **Step 2: Run and watch them fail**

Run: `cd show-engine && npx vitest run src/showEngine.test.ts`
Expected: FAIL — the engine never fetches.

- [ ] **Step 3: Implement non-blocking polling**

- [ ] **Step 4: Run and watch them pass**

Run: `cd show-engine && npx vitest run && npm run typecheck && npm run typecheck:tests`
Expected: all green.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/showEngine.ts show-engine/src/showEngine.test.ts
git commit -m "feat(show-engine): non-blocking Mukana polling and per-tick capabilities"
```

---

### Task 10: The composed pipeline, and the outage scenario Plan 4 could not test

**Files:**
- Create: `show-engine/src/enginePipeline.test.ts`
- Modify: `show-engine/src/index.ts` (export everything new)

**This task adds no production code beyond the barrel exports.** If a test fails, the defect is in Tasks 1–9 — fix it there, and say which task and why in your report. Do not weaken a test to make it pass.

**Interfaces:**
- The barrel must export: `Clock`, `systemClock`, `HostAdapter`, `HostCapabilities`, `MockHost`, `HostCall`, `ShowSnapshot`, `buildSnapshot`, `ShowEngine`, `ShowEngineDeps`, `PersistedShowState`, `STATE_VERSION`.

**Behavior:**

Three scenarios, each stated with the invariant it must break on.

**1. A full show, end to end.** Seven participants join, a host and reader are assigned, a look is selected, program cuts to it, a guest speaks, the operator pages the queue. Assert the snapshot is coherent at each step and the host received a plausible command sequence. *Must break on:* any tick-order inversion — deriving tally before the look resolves, or emitting nameplates from a stale resolution.

**2. The registry dies mid-show.** This is the scenario Plan 4's outcomes doc recorded as owed: it was untestable until an orchestrator existed. A show runs healthy with all three integrations, then every Mukana endpoint starts failing. Assert that the roster, the seating, the gallery, the resolved look, and tally all survive unchanged; that guest boxes fall back to the operator's manual assignments rather than emptying; and that capabilities report `unavailable` with a non-null detail. *Must break on:* any consumer treating a lost integration as a reason to clear state rather than to degrade.

**3. Degradation equivalence, at the engine level.** Plan 4 proved `unavailable ≡ disabled` for look resolution in isolation. Prove it for the whole engine: two engines, identical rosters and operator actions, one with an integration configured-but-failing and one with it never configured, produce structurally equal snapshots — everything except the `capabilities` node's `detail` strings. *Must break on:* any code path branching on `disabled` vs `unavailable` outside `detail`. This is the design's load-bearing guarantee; if it fails, a mid-show outage stops behaving like a show that never had the integration.

- [ ] **Step 1: Write the three scenarios**

Build them from the helpers established in Tasks 4–9 rather than inventing new fixtures. For scenario 3, compare with a normalizer that strips `detail` from all three capability nodes and leaves everything else intact — do not compare hand-picked fields, because the point of the property is that you did not choose which fields it covers.

- [ ] **Step 2: Run and watch them fail or pass**

Run: `cd show-engine && npx vitest run src/enginePipeline.test.ts`
A failure here means a defect in Tasks 1–9. Fix it there.

- [ ] **Step 3: Export everything from the barrel**

- [ ] **Step 4: Run everything**

Run: `cd show-engine && npx vitest run && npm run typecheck && npm run typecheck:tests && npm run build`
Expected: all four green.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/enginePipeline.test.ts show-engine/src/index.ts
git commit -m "test(show-engine): the composed pipeline and the registry-outage scenario"
```

---

## Deliberately out of scope

Named so a reviewer does not read them as gaps:

- **The `ohg.*` action surface, the flattened OSC/Companion feedback fields, and the cross-adapter conformance suite** — Plan 6. This plan gives them a ticking engine and a snapshot to render; it registers nothing with a control server.
- **Smart gallery.** `RecencyScores` and `GalleryDirector.smartCells()` are shipped and stay unused here. Smart gallery is driven by `ohg.gallery.smart.set`, an operator action, so it belongs with the action surface in Plan 6.
- **Headline overlays.** Spec §4.2 lists `ohg.gfx.headline.*` but `OverlayState` has no headline concept. Plan 3's outcomes flagged this as a decision owed before the action surface is built; it is Plan 6's to make, not this plan's to guess.
- **Real process hosting** — the Node subprocess, its JSON-line pipe, and supervision. That is per-shell work in Plans 7–9.

## Self-review

**Spec coverage.** §2's process model, event source, and normative no-blocking rule → Tasks 5, 9. §3.14 the `hostAdapter` boundary → Task 1. §4.3's state node → Tasks 3, 7 (the `ohg.*` protocol wrapper is Plan 6). §5's persistence line → Task 2. §6's command set, capability declaration, and preview-bus degradation → Tasks 1, 7, 8. §7's poll intervals, dormant handling, and last-good retention → Task 9. The capabilities spec §2's normative tick rule → Task 9; §3.2's manual-fill fallback and clearing → Tasks 2, 7; §5's equivalence property → Task 10.

**Carried obligations, all discharged with a named test:** the `shouldFollowSpeaker` dispatch gate (Task 6, quantified over role × assigner × follow); `ManualBoxAssignments` in state (Tasks 2, 7); the `clampPage`/`resolveLook` capability pairing (Task 7, quantified over page × capability state × fill); the registry-outage scenario Plan 4 could not reach (Task 10).

**Type consistency.** `PersistedShowState` and `ShowSnapshot` are distinct throughout, per the naming hazard note. `ShowSnapshot` accretes three fields across tasks — `unseated` (Task 5), `pagingRefused` (Task 7) — and each is added to `buildSnapshot`'s input in the same task, deep-copied like the rest. `setLook` is introduced minimally in Task 4 because Task 4's restore tests need it, and completed in Task 7; both tasks say so.

**Known plan risk.** `showEngine.ts` accumulates behavior across six tasks and will be the largest file in the package. The guardrail is in the File Structure section: past ~450 lines, extract command emission to `hostCommands.ts`. A reviewer should hold Task 8 to that.
