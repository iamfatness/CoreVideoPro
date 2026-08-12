# Show Engine Control Surface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `show-engine` the complete `ohg.*` action surface, a flattened control-state projection, and a conformance suite — so three host shells can drive the engine without any of them re-deriving show logic.

**Architecture:** The engine owns the action *semantics* and the state *shape*; each host bridges them to its own control server (Plans 7–9). A declarative registry maps `ohg.*` ids and positional typed params onto `ShowEngine` methods, returning typed results rather than throwing. A pure projection flattens `ShowSnapshot` into OSC/Companion feedback fields. A shared conformance suite runs any `HostAdapter` through scripted scenarios.

**Tech Stack:** TypeScript 5.9 strict, ES2022, NodeNext modules, vitest 4, Node 24. No new dependencies.

## Global Constraints

- Module system is **NodeNext**: every relative import MUST end in `.js`.
- vitest runs with `globals: false`: every test file MUST `import { describe, expect, it } from "vitest";` explicitly.
- Strict TypeScript. **No `any`.** No non-null assertions where a guard will do.
- **No I/O in engine modules** beyond the injected `StateFs` and `FetchLike`. **No `Date.now()`, no `setTimeout`, no timers** — time enters only through the injected `Clock`. `clock.ts` is the sole exception and must stay so.
- Everything a host adapter consumes MUST be reachable through the barrel `index.ts`.
- Polling tests MUST use the rig's `flush()` (8-turn microtask drain) after every `tick()`, or they pass vacuously. Node's deferred unhandled-rejection check additionally needs a macrotask boundary (`setImmediate`).
- **Do NOT use `git stash`** for any reason. The stash stack is shared with other working trees on this machine and their owner works in them concurrently.
- Branch: `plan/show-engine-control-surface`, stacked on `plan/show-engine-orchestrator`.

## Read before Task 1

`docs/superpowers/plan-authoring-rules.md` — eight rules distilled from the defects in Plans 1–5.
Rule 7 is operationalized here: **every task carries a "Mutations to run" block.** Those are not
suggestions; a task is not done until each named mutation has been applied, observed to red the
right test for the right reason, and reverted. Report the results.

## Decisions taken before this plan (owner, 2026-08-12)

1. **The action surface lives in the engine; hosts bridge it.** The parent design spec §4.1 claims
   the host "proxies `ohg.*` invokes to the engine subprocess and merges engine state into
   `ControlState` under an `ohg` node." **That mechanism does not exist.**
   `ControlActionRegistry` is a hardcoded static list (`ControlActionRegistry.cs:130-249`),
   `StudioControlSurface` dispatches through a compile-time `switch`, and a coverage test enforces
   the two stay a closed 1:1 set. There is no dynamic registration and `ControlState` has no
   extensible subtree. Building that is per-shell work in Plans 7–9. This plan ships a
   host-agnostic surface those bridges translate.
2. **`applyLook` gains `scenePreset`, `hostSlot`, and `readerSlot`** so three adapters do not each
   re-derive `lookId → scenePreset` in C#, Swift, and C++.
3. **`OverlayState` gains a headline**, so spec §4.2's `ohg.gfx.headline.*` actions have something
   real to drive.

## What the host control stack actually looks like

Established by extraction, for the bridges in Plans 7–9. The engine does not implement any of this
— it must merely be *mechanically translatable* to it.

- Action shape: `ControlAction(Id, Title, Description, IReadOnlyList<ControlParam>)`;
  `ControlParam(Name, ControlParamType{String|Int|Double|Bool}, Required = true, Description?)`.
  Params are positional. (`ControlAction.cs:14-34`)
- OSC address derivation: `Root + "/" + actionId.Replace('.', '/')`, verbatim, with **no case or
  word transforms** — `transport.record.set` → `/cvp/transport/record/set`. (`OscAddressMap.cs:19-35`)
- Feedback fields: a hand-maintained flat `string[]` on `ControlManifest`; camelCase for scalars,
  and a `input/{slot}/<field>` template expanded at runtime. It is **not** derived from
  `ControlState`. (`ControlManifest.cs:37-45`, `OscFeedback.cs:40-46`)
- `GET /manifest` serializes **PascalCase**; `/state` and WebSocket pushes are **camelCase**. Real
  asymmetry — do not assume one casing.
- OSC carries **no auth token**; only HTTP/WS use the bearer token.

## File Structure

**Create:**
- `show-engine/src/mukanaPoller.ts` — the polling scheduler, busy gate, and outcome application, moved out of `showEngine.ts`.
- `show-engine/src/lookController.ts` — look selection, paging, manual boxes, and refusals, moved out of `showEngine.ts`.
- `show-engine/src/actions.ts` — the `ohg.*` registry: ids, positional typed params, and dispatch onto `ShowEngine`.
- `show-engine/src/controlState.ts` — the pure `ShowSnapshot` → flattened feedback-field projection.
- `show-engine/src/conformance.ts` — the shared scenario suite any `HostAdapter` implementation runs against.

**Modify:** `showEngine.ts` (carve-outs, then new operator methods), `hostAdapter.ts` + `mockHost.ts` + `hostCommands.ts` (`applyLook` widening), `overlayDirector.ts` + `contracts.ts` (headline), `mukanaClient.ts` (hysteresis + timeout contract), `index.ts`.

**Test:** one `*.test.ts` beside each new module, plus `actionsPipeline.test.ts` for the composed surface.

---

### Task 1: Carve out `MukanaPoller`

**Files:** Create `show-engine/src/mukanaPoller.ts`, `mukanaPoller.test.ts`; modify `showEngine.ts`.

**This is move-only.** No behavior change; the full suite must stay green across the commit. Plan 5's
outcomes named this carve-out explicitly and said to do it *before* adding an action surface.

**Interfaces — Produces:**

```ts
export type PollOutcomes = {
  panelists: MukanaOutcome | null;
  hands: HandsOutcome | DormantOutcome | null;
  question: QuestionOutcome | null;
};

export class MukanaPoller {
  constructor(deps: { client: MukanaClient; clock: Clock });
  /** Start any due, non-busy fetches. Never awaits. Never throws. */
  poll(): void;
  /** Outcomes that settled since the last call, then cleared. */
  drain(): PollOutcomes;
  /** Endpoints whose in-flight poll has been outstanding beyond the hung threshold. */
  hungEndpoints(): readonly MukanaEndpoint[];
}
```

**Behavior:** exactly what `showEngine.ts` does today — the due-check against `nextDelayMs`, the
per-endpoint busy gate cleared on both settle paths, rejection handlers that cannot let a rejection
escape, and the hung-poll detection. Move it; do not improve it. Task 3 changes its behavior.

`ShowEngine` keeps the *sequencing*: it calls `poll()` then `drain()` inside `tick()` at the same
point in the order it does today, and applies outcomes exactly as before.

**Mutations to run:**
- Make `poll()` await a fetch → the "completes a tick while a fetch is still in flight" test reds.
- Drop the busy gate → the in-flight-accumulation test reds.
- Remove a rejection handler → the unhandled-rejection test reds.

If any of those three fails to red after the move, the move lost behavior — stop and report it.

**Steps:** write the move, run `npx vitest run` (must be exactly the pre-move count), run
`npm run typecheck && npm run typecheck:tests`, run the three mutations, commit
`refactor(show-engine): extract Mukana polling into MukanaPoller`.

---

### Task 2: Carve out `LookController`

**Files:** Create `show-engine/src/lookController.ts`, `lookController.test.ts`; modify `showEngine.ts`.

**Move-only**, same discipline as Task 1.

**Interfaces — Produces:**

```ts
export type PagingRefusalKind = "no-look" | "fill" | "range";

export class LookController {
  constructor(deps: { looks: readonly LookDefinition[] });
  selected(): LookDefinition | null;
  select(lookId: string): void;              // throws on an unknown id
  adoptRestored(lookId: string | null): string | null;  // returns a warning, or null
  page(): number;
  setPage(page: number): void;
  adjustPage(delta: number, queue: QueueState, handsQueue: Capability): void;
  manualBoxes(): ManualBoxAssignments;
  assignBox(box: number, slot: number): void;
  clearBox(box: number): void;
  restoreManualBoxes(boxes: ManualBoxAssignments, lookId: string | null): void;
  refusal(): { message: string; kind: PagingRefusalKind } | null;
  clearStaleRefusal(effectiveFill: BoxFill): void;
}
```

**Behavior:** exactly today's semantics. Selecting a *different* look clears manual boxes; re-selecting
the same one does not. `adjustPage` bounds against `pageCountFor` and records a refusal at the edges
rather than letting a later clamp swallow it. `"range"` refusals are not auto-cleared by a fill change;
`"no-look"` and `"fill"` are. `adoptRestored` discards an unknown id, returns the warning text, and
does not re-persist it.

`ShowEngine` retains the `clampPage`/`resolveLook` pairing — **that pairing does not move.** Both calls
must stay adjacent in `tick()` and share one per-tick capability value; splitting them across a module
boundary is exactly how they would drift.

**Mutations to run:**
- Make `select` clear manual boxes on a same-look re-select → the idempotent-re-select test reds.
- Make `adjustPage` unbounded → the off-the-end refusal test reds.
- Auto-clear a `"range"` refusal on a fill change → the still-valid-refusal test reds.
- Make `adoptRestored` accept an unknown id → the discard-and-warn test reds.

**Steps:** as Task 1; commit `refactor(show-engine): extract look selection and paging into LookController`.

---

### Task 3: Close the hung-endpoint obligation

**Files:** Modify `mukanaPoller.ts`, `mukanaClient.ts`; test in `mukanaPoller.test.ts`.

**This is the obligation Plan 5 carried forward, and it must ship before any host wires a real
`FetchLike`** (Plan 7 does). Plan 5 fixed "a hung endpoint reports `available` forever" — which had
made the operator's manual-box fallback unreachable on air — by degrading a poll outstanding more than
`MUKANA_HUNG_POLL_INTERVALS` (3) times its interval. That fix has two defects:

1. **It false-positives with no hysteresis.** An endpoint answering correctly after ~6.5 s flips
   `boxFill` `queue → manual → queue`; one hovering near the threshold oscillates the guest boxes every
   poll cycle. On air that is visible flapping.
2. **The constant's lower bound is unpinned** — `3 → 1` currently leaves the suite green, because the
   test comment claiming "one interval of ordinary slowness is NOT reported as an outage" asserts at
   `outstanding == 0`.

**Interfaces — Produces:**

```ts
/** Consecutive healthy settles required before a degraded endpoint is trusted again. */
export const MUKANA_RECOVERY_SETTLES = 2;

export type FetchLike = (url: string, init?: { signal?: AbortSignal }) => Promise<FetchResponse>;
```

**Behavior:**

- **Hysteresis.** An endpoint degraded by the hung-poll rule stays degraded until
  `MUKANA_RECOVERY_SETTLES` consecutive polls settle healthily. One good answer after a hang does not
  immediately restore `available`, so a marginal endpoint cannot oscillate the picture.
- **The timeout contract.** `FetchLike` gains an optional `init.signal`, and `MukanaClient` passes an
  `AbortSignal` derived from the endpoint's interval. The engine still holds no timer — the abort is
  the *caller's* to honor. **Document on `FetchLike` that a host MUST supply a fetch that honors
  `signal`**, and say why: without it a hung endpoint never fails, so `consecutiveFailures` stays 0,
  backoff never engages, and the hung-poll rule is the only thing standing between the show and a
  frozen queue. A host that ignores the signal gets degradation but never recovery.
- **Pin the constant at both bounds.** A test must red at `MUKANA_HUNG_POLL_INTERVALS = 1` *and* at
  `= 30`. The lower-bound test asserts on an endpoint outstanding for one interval plus a margin and
  requires it still `available`.

**Mutations to run:**
- `MUKANA_HUNG_POLL_INTERVALS` 3 → 1 → the lower-bound test reds.
- `MUKANA_HUNG_POLL_INTERVALS` 3 → 30 → the hung-detection test reds.
- `MUKANA_RECOVERY_SETTLES` 2 → 1 → the no-flapping test reds.
- Drop the signal from the fetch call → the abort-plumbing test reds.

**Property to write, with its quantification stated:** for a sequence of poll latencies drawn from
`[0.5×, 1×, 2×, 2.9×, 3.1×, 10×]` the interval, crossed with recovery after 1 and after 2 settles,
the published `handsQueue` capability must never change state more than once per `MUKANA_RECOVERY_SETTLES`
settles. That quantification is the point — the Plan 5 version held latency constant and never saw the
oscillation.

**Steps:** TDD, full suite, both typechecks, mutations, commit
`fix(show-engine): hysteresis and an abort contract for hung Mukana endpoints`.

---

### Task 4: Widen `applyLook`

**Files:** Modify `hostAdapter.ts`, `mockHost.ts`, `hostCommands.ts`; test in `hostAdapter.test.ts`, `showEngine.test.ts`.

**Interfaces — Produces:**

```ts
export type LookPlacement = {
  lookId: string;
  scenePreset: string;
  hostSlot: number | null;
  readerSlot: number | null;
  boxes: ReadonlyMap<number, number | null>;
};

// on HostAdapter
applyLook(placement: LookPlacement): void;
```

**Behavior:** the engine already resolves all five values (`LookResolution` carries `lookId`,
`scenePreset`, `hostSlot`, `readerSlot`, and `boxes`), so this is plumbing what it already knows
rather than computing anything new. Diffing in `hostCommands.ts` must compare **every** field of
the placement, not just the look id and boxes — a `scenePreset` or chair change with an unchanged
look id must still emit.

`MockHost` records the placement with the boxes map **snapshotted at record time**, as it already
does — the engine reuses its working maps between ticks, and a stored reference would report the
final state for every historical call.

**Mutations to run:**
- Diff on `lookId` only → the "re-emits when the host chair moves" test reds.
- Diff on `lookId` + `boxes` only → the "re-emits when `scenePreset` changes" test reds.
- Store the boxes map by reference in `MockHost` → the map-snapshot test reds.

**Steps:** TDD, full suite, both typechecks, mutations, commit
`feat(show-engine): applyLook carries the scene preset and both chairs`.

---

### Task 5: A headline in the overlay model

**Files:** Modify `contracts.ts`, `overlayDirector.ts`; test in `overlayDirector.test.ts`.

**Interfaces — Produces:**

```ts
export type Headline = { name: string; location: string };

export type OverlayState = {
  nameplates: Nameplate[];
  question: QuestionOverlay | null;
  headline: Headline | null;
  headlineVisible: boolean;
};

export type OverlayInput = {
  look: LookResolution | null;
  question: MukanaQuestion | null;
  questionVisible: boolean;
  headline: Headline | null;
  headlineVisible: boolean;
};
```

**Behavior:** the headline is **operator-driven, not derived** — unlike nameplates, nothing computes
it from the roster. It mirrors how `questionVisible` already works: `OverlayDirector.update` reports
`true` when the derived state changed, and a headline change (text or visibility) counts. The
director keeps its clone-on-ingest and clone-on-egress discipline; Plan 3's outcomes recorded both as
load-bearing and a per-task review's call to remove one was overturned. Do not remove either.

An invisible headline still carries its text, so toggling visibility twice restores the same content
without the operator retyping it.

**Mutations to run:**
- Return `false` from `update` when only the headline text changed → the change-detection test reds.
- Return `false` when only `headlineVisible` changed → the visibility test reds.
- Clear `headline` when `headlineVisible` goes false → the retains-text test reds.
- Remove the ingest clone → the caller-mutation test reds.

**Steps:** TDD, full suite, both typechecks, mutations, commit
`feat(show-engine): an operator-driven headline in the overlay model`.

---

### Task 6: Roster operator methods

**Files:** Modify `showEngine.ts`; test in `showEngine.test.ts`.

**Interfaces — Produces, on `ShowEngine`:**

```ts
addPanelist(participantId: string, slot?: number): number | null;  // slot omitted = first empty
removePanelist(slot: number): void;
replacePanelist(slot: number, participantId: string): void;
setRole(pin: string, role: Role): void;
syncAll(): void;
setHeadline(headline: Headline | null): void;
setHeadlineVisible(on: boolean): void;
```

**Behavior:** these are the operator's explicit seat controls, and they are the reason **a Zoom
departure does not vacate a seat** (owner ruling, 2026-08-06) — clearing a seat is this, not a
disconnect. Each maps onto `LiveSlots` (`add`/`removeSlot`/`replace`) and marks the roster dirty so
the next tick re-seats and persists.

`setRole` takes the **PIN** as spec §4.2 declares, and resolves it to a `PersonKey` via
`personKeyForPin` — never by parsing a key, which `personKey.ts` documents as opaque. For an
exclusive role it must route through `assignExclusiveRole`, which is the only thing that demotes a
prior holder; `set` alone leaves two hosts.

`syncAll()` forces every Mukana endpoint due on the next tick without waiting for its interval.

`addPanelist` with an occupied slot is an error, not a silent replace — `replacePanelist` is the
explicit verb.

**Mutations to run:**
- Route `setRole` through `set` alone for an exclusive role → the single-host test reds.
- Make `addPanelist` overwrite an occupied slot → the refuses-occupied test reds.
- Skip the dirty flag on `removePanelist` → the persists-after-remove test reds.
- Parse the `PersonKey` instead of using `personKeyForPin` → the opacity test reds.

**Steps:** TDD, full suite, both typechecks, mutations, commit
`feat(show-engine): operator roster and headline controls`.

---

### Task 7: Gallery operator methods, including smart gallery

**Files:** Modify `showEngine.ts`; test in `showEngine.test.ts`.

**Interfaces — Produces, on `ShowEngine`:**

```ts
resetGalleryFromSlots(): void;
replaceGalleryCell(cell: number, slot: number): void;
removeGalleryCell(cell: number): void;
emptyGallery(): void;
setSmartGallery(on: boolean): void;
```

**Behavior:** `GalleryDirector` already implements `resetFromSlots`, `replace`, `remove`, `empty`,
`smartCells`, and `applyOrder`; this exposes them. **Smart gallery was deferred from Plan 5** and
lands here because it is operator-driven.

When smart gallery is on, the tick orders cells by `RecencyScores.order(...)` over the seated
participant ids and applies it with `applyOrder`. `RecencyScores` has been shipped and unused since
Plan 2; this is its first consumer. It deliberately does **not** implement `PositionAssigner` — it
orders candidates rather than placing them — so it is fed the seated ids and its output drives
`applyOrder`, never a placement.

The speaker gate governs what reaches `RecencyScores`: an ASL interpreter must not reorder the
gallery, for the same reason they must not evict a panelist from the speaker pool.

Plan 5's ledger recorded that `emitGallery`'s diffing was never exercised through `tick()` because no
operator API mutated cells. **These methods close that**; the emission tests must now drive it
end-to-end rather than only through `hostCommands.test.ts`.

**Mutations to run:**
- Feed the interpreter's id to `RecencyScores` → the interpreter-does-not-reorder test reds.
- Apply the smart order when the toggle is off → the toggle test reds.
- Skip `emitGallery` on a cell change → the end-to-end gallery emission test reds.

**Steps:** TDD, full suite, both typechecks, mutations, commit
`feat(show-engine): gallery operator controls and smart gallery ordering`.

---

### Task 8: The `ohg.*` action registry

**Files:** Create `show-engine/src/actions.ts`, `actions.test.ts`; modify `index.ts`.

**Interfaces — Produces:**

```ts
export type ActionParamType = "string" | "int" | "double" | "bool";
export type ActionParam = {
  name: string;
  type: ActionParamType;
  required: boolean;
  description: string;
};

export type ActionDefinition = {
  id: string;                 // e.g. "ohg.look.nextGuest"
  title: string;
  description: string;
  params: readonly ActionParam[];
};

export type ActionResult =
  | { kind: "ok" }
  | { kind: "refused"; reason: string }
  | { kind: "error"; message: string };

export const OHG_ACTIONS: readonly ActionDefinition[];

export function invokeAction(
  engine: ShowEngine,
  id: string,
  args: readonly unknown[]
): ActionResult;

/** The OSC address a host must expose for `id`, by the host stack's own rule. */
export function oscAddressFor(id: string, root?: string): string;
```

**Behavior:**

The registry is **declarative and complete** — the full spec §4.2 list: `ohg.panelist.add/remove/replace/role.set/syncAll`,
`ohg.program.preview/cut/auto/directCut/asFollow.set`, `ohg.look.set/nextGuest/prevGuest/box.assign/box.clear`,
`ohg.gallery.resetFromSlots/replace/remove/empty/smart.set`, `ohg.gfx.headline.in/out/change`,
`ohg.gfx.question.in/out`, `ohg.mukana.sync/override.set/override.delete`.

**Params are positional and typed**, matching the host stack so a bridge is mechanical.
`invokeAction` coerces and validates each arg against its declared type and arity, and returns
`{kind:"error"}` on a mismatch — **it never throws**, because a malformed OSC packet from a Companion
button must not take down a live show.

**A refusal is a first-class result, not an error.** `ohg.look.nextGuest` under manual box fill
returns `{kind:"refused", reason}` carrying the engine's own `pagingRefused` text — spec §4 requires a
typed refusal, "not a silent no-op, not a throw."

`oscAddressFor` implements the host stack's rule **verbatim**: `root + "/" + id.replace(/\./g, "/")`,
with no case or word transformation (`OscAddressMap.cs:19-35`). Default root `"/cvp"`. So
`ohg.look.box.assign` → `/cvp/ohg/look/box/assign`. Do not "improve" the casing; the bridge must
match the shipped host behavior exactly.

**Two structural tests, because drift here is silent:**
- Every `ActionDefinition.id` maps to a dispatch case, and every dispatch case has a definition —
  a closed 1:1 set, mirroring the coverage test the C# registry already enforces.
- Every id round-trips through `oscAddressFor` to a unique address (no two actions collide).

**Mutations to run:**
- Delete one dispatch case → the 1:1 coverage test reds.
- Delete one definition → the same test reds from the other side.
- Make `invokeAction` throw on an arity mismatch instead of returning `error` → the malformed-args test reds.
- Return `{kind:"ok"}` for a refused paging call → the typed-refusal test reds.
- Add a case transform to `oscAddressFor` → the address test reds.

**Steps:** TDD, full suite, both typechecks, mutations, commit `feat(show-engine): the ohg.* action registry`.

---

### Task 9: The control-state projection

**Files:** Create `show-engine/src/controlState.ts`, `controlState.test.ts`; modify `index.ts`.

**Interfaces — Produces:**

```ts
export type ControlFieldValue = string | number | boolean | null;

/** Flattened feedback fields, keyed by the host stack's own naming convention. */
export function projectControlFields(snapshot: ShowSnapshot): Record<string, ControlFieldValue>;

/** The stable field-name templates a host declares up front, e.g. "ohg/slot/{slot}/name". */
export const OHG_FIELD_TEMPLATES: readonly string[];
```

**Behavior:**

Pure — same snapshot in, same record out; no clock, no I/O, no engine instance.

Field names follow the host stack's convention (`ControlManifest.cs:37-45`): camelCase segments, a
`{slot}` template expanded at runtime, all under an `ohg/` prefix. From spec §4.3:
`ohg/slot/{slot}/name`, `ohg/slot/{slot}/role`, `ohg/slot/{slot}/tally`, `ohg/program/mode`,
`ohg/queue/current`, `ohg/health/mukana`, plus `ohg/capabilities/{name}/state`.

**The value type is deliberately narrow.** OSC carries scalars; a field whose value is an object or
array cannot be fed back to a Companion button. `ControlFieldValue` enforces that at the type level
so no future field can smuggle a structure through.

**`OHG_FIELD_TEMPLATES` and `projectControlFields` must not drift.** The host stack's own field list
is hand-maintained and explicitly *not* derived from its state type — the extraction found that, and
it is a known weakness we are not copying. A structural test asserts every key produced by
`projectControlFields`, for a snapshot with every optional value populated, matches a declared
template after `{slot}` substitution — and vice versa.

**Fixture shape invariant (rule 1):** the projection fixture MUST have no empty, null, or zero value
anywhere it can be avoided — at least two occupied slots with different roles, a non-empty queue, a
non-`none` tally mode, and all three capabilities in *different* states. A projection bug that drops a
field is invisible against a fixture whose value was already absent.

**Mutations to run:**
- Drop `ohg/slot/{slot}/tally` from the projection → the template-coverage test reds.
- Emit a slot field for an empty seat → the holes test reds.
- Widen `ControlFieldValue` and emit an object → `typecheck:tests` fails (report this as the mutation result).
- Change one produced key's casing → the template-coverage test reds.

**Steps:** TDD, full suite, both typechecks, mutations, commit `feat(show-engine): flattened control-state projection`.

---

### Task 10: The conformance suite, and the composed surface

**Files:** Create `show-engine/src/conformance.ts`, `actionsPipeline.test.ts`; modify `index.ts`.

**This task adds no engine behavior.** If a scenario fails, the defect is in Tasks 1–9 — fix it there,
say which task owned it, and do not weaken a scenario to make it pass.

**Interfaces — Produces:**

```ts
export type ConformanceCase = {
  name: string;
  /** Drive the engine, then assert against what the adapter received. */
  run(engine: ShowEngine, host: MockHost, flush: () => Promise<void>): Promise<void>;
};

export const HOST_CONFORMANCE_CASES: readonly ConformanceCase[];
```

**Behavior:** the suite is **exported, not just used** — spec §6 calls it "the mechanism that keeps
CVP-Windows, CVP-Mac, and OBS semantically identical," so Plans 7–9 run these same cases against
their real adapters. Each case drives the engine through public actions only and asserts on the
`HostAdapter` calls that result, never on engine internals — an adapter author must be able to run
them without reaching inside.

Cases must cover, at minimum: a slot assignment reaching the host; a look application carrying scene
preset and both chairs; transport degradation on a preview-less host; gallery composition; nameplate
and question emission with change detection; and quiet ticks emitting nothing.

**`actionsPipeline.test.ts` composes the whole surface**, with each scenario's breaking invariant stated:

1. **Every action drives the engine.** For each of the ~25 registry ids, invoke it with valid args and
   assert the snapshot or host calls changed as that action promises. *Must break on:* a dispatch case
   wired to the wrong engine method — a class of bug the 1:1 coverage test cannot see, because it only
   checks that a case *exists*.
2. **A malformed invoke never throws and never mutates.** For each id, invoke with too few args, too
   many, and wrong types; assert `{kind:"error"}` and a byte-identical snapshot. *Must break on:* any
   dispatch path that mutates before validating.
3. **A refused action reports why.** *Must break on:* a refusal degrading into `ok` or into a throw.
4. **The projection tracks the engine.** Drive a show; after each tick assert `projectControlFields`
   agrees with the snapshot it was built from. *Must break on:* the projection reading stale state.

**Fixture discipline:** every scenario that polls MUST use `flush()` after each `tick()`. Plan 5
shipped a vacuous retention test because a fixture read at ticks 2 and 4 while the outcome first
landed at tick 5 — and the entire outcome-apply path could then be deleted with the full suite green.

**Barrel:** export `OHG_ACTIONS`, `ActionDefinition`, `ActionParam`, `ActionParamType`, `ActionResult`,
`invokeAction`, `oscAddressFor`, `projectControlFields`, `ControlFieldValue`, `OHG_FIELD_TEMPLATES`,
`HOST_CONFORMANCE_CASES`, `ConformanceCase`, `MukanaPoller`, `LookController`, `LookPlacement`,
`Headline`. Verify each resolves through the **built** `dist/index.js` and `.d.ts`, not merely that an
export line exists — Plan 5's Task 10 found several names unreachable despite being written.

**Mutations to run:**
- Wire one dispatch case to the wrong engine method → scenario 1 reds.
- Validate args after mutating → scenario 2 reds.
- Set `flush()`'s loop bound to 0 → report whether any scenario reds; if none does, say so plainly
  rather than claiming the drain is load-bearing.

**Steps:** build the scenarios, export, run the full suite + both typechecks + `npm run build`,
mutations, commit `test(show-engine): the conformance suite and the composed action surface`.

---

## Deliberately out of scope

- **Host bridges.** No C#, Swift, or C++ is written here. Plan 7 (WinUI) must add dynamic or proxied
  registration to `CoreVideoPro.Control` — which today is a closed compile-time 1:1 registry — plus an
  extensible `ControlState` subtree. That is real work and it is not this plan's.
- **The Node subprocess and its JSON-line pipe.** Per-shell, Plans 7–9.
- **A browser operator surface.** Explicitly not in v1 (spec §4.5); remote is Companion + OSC.
- **Auth.** The host server owns it. Note for Plan 7: OSC carries **no** token today, only
  loopback-vs-LAN gating, so any `ohg.*` action reachable over OSC is reachable unauthenticated on the LAN.

## Self-review

**Spec coverage.** §4.1 one-endpoint-per-host → Task 8 (engine half) with the host half named out of
scope and its true cost stated. §4.2's full action list → Tasks 6–8. §4.3's state node and flattened
fields → Task 9. §4.4's native panels → they render Task 9's projection; no plan work. §6's
conformance suite → Task 10. §3.8's smart gallery → Task 7. §3.12's headline → Task 5.

**Carried obligations, walked line by line against the Interfaces blocks (rule 6):** the hung-endpoint
hysteresis, the `FetchLike` timeout contract, and the constant's lower bound → Task 3. The two
move-only carve-outs → Tasks 1–2, first, as Plan 5's outcomes required. `applyLook`'s missing chairs
and scene preset → Task 4. The headline decision → Task 5. Smart gallery and `emitGallery`'s untested
end-to-end path → Task 7.

**Still open, deliberately, and to be restated in this plan's outcomes:** `mukanaClient.ts`'s doc
comment claims `applyHealth` is the sole health writer while `fail()` also writes directly;
`TallyState`'s three parallel arrays carry a zip hazard that becomes a breaking change once host
adapters read them — Task 9's projection is the first consumer, so if it finds the zip awkward, say so
in the report rather than working around it silently.

**Type consistency.** `LookPlacement` (Task 4) is what `HostAdapter.applyLook` takes and what
`hostCommands` diffs; `Headline` (Task 5) appears in `OverlayState`, `OverlayInput`, and
`ShowEngine.setHeadline`; `ActionResult` (Task 8) is what `invokeAction` returns and what Task 10's
scenarios assert on. `PagingRefusalKind` moves to `lookController.ts` in Task 2 and is referenced by
Task 8's refusal mapping.

**Known plan risk.** Tasks 6 and 7 add operator methods to `showEngine.ts` while Tasks 1–2 remove
polling and paging from it. The net direction should be down; if it is not by Task 7, say so in that
task's report rather than letting it drift — Plan 5's carve-out was add-only and the file grew.
