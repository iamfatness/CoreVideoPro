# Show Engine — Plan 4: Capabilities

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a show run only the features its data sources support, and survive a third-party integration failing mid-broadcast.

**Architecture:** A pure `capabilities` resolver turns config plus live integration health into three effective states, and every consumer branches on that rather than on configuration. Two structural couplings to the registry are closed alongside: role overrides re-key from PIN to a resolved `PersonKey`, and look guest boxes gain a named fill strategy that always has a working fallback.

**Tech Stack:** TypeScript 5.9 (strict, ES2022, NodeNext), vitest 4, Node 24.

**Source documents:**
- Spec: `docs/superpowers/specs/2026-08-05-show-engine-capabilities-design.md`
- Parent spec: `docs/superpowers/specs/2026-08-04-ohg-show-engine-design.md`
- **Read first:** `docs/superpowers/plans/2026-08-05-ohg-show-engine-outputs-outcomes.md`

**Plan series (this is Plan 4 of 9):**
1. Core identity & roster — shipped (PR #363)
2. Direction — shipped (PR #364)
3. Outputs — shipped (PR #365)
4. **Capabilities** ← this plan
5. Host port & orchestrator
6. CVP Windows host integration
7. mac-shell control server + panel
8. OBS plugin adapter
9. Migration tooling

## Plan format

As in Plans 2 and 3: **interfaces, behavior prose, and complete tests — no implementation
bodies.** Write the implementation from the interface and the behavior. Tests are the
executable specification and are binding. If behavior text seems to contradict a test, stop
and report NEEDS_CONTEXT rather than guessing.

Two refinements from Plan 3's outcomes are applied throughout:

- **Every Interfaces block was run against its consuming call site**, not just its producing
  module. Plan 3's two misses were both "producer has it, consumer cannot reach it" —
  `tallySource` on the wrong type, and `TallySource` unexported. Consumers are named
  explicitly in each Interfaces block below.
- **Every integration scenario states the invariant it must break.** Plan 3 supplied a test
  body *and* a prose rule; the body was copied faithfully, did not satisfy the prose, and
  the prose lost. Where a test's intent is not obvious from its assertions, this plan says
  what must fail if the composition is wrong.

## Global Constraints

- **Branch:** `plan/show-engine-capabilities`, stacked on `spec/show-engine-capabilities`. Commit after every task.
- **Module system:** NodeNext. **Every relative import MUST end in `.js`.**
- **vitest runs with `globals: false`** — test files must explicitly `import { describe, expect, it } from "vitest";`.
- **Strict TypeScript.** No `any`. No non-null assertions where a guard will do.
- **No I/O in this plan.** Pure logic over in-memory state; no network, filesystem, or timers.
- **Copy, do not alias** — on both read and write. Accessors return fresh structures; mutators clone what they are given.
- **Loud, never silent.** Invalid arguments throw with actionable messages; parse failures return typed outcomes.
- **`unavailable` and `disabled` must produce identical downstream behavior.** This is the design's load-bearing property, and Task 8 tests it directly. Any code that branches on *which* of the two it is — outside the operator-facing `detail` field — is a defect.
- **No external integration failure may block a tick.** Nothing in this plan awaits a recovery, retries inline, or lets a fetch outcome gate a derivation.
- **`OverrideDb` remains authoritative for editorial roles.** This plan changes how they are *keyed*, never who decides them.
- **Slot, cell, box, and page numbers are 1-based** (pages are 0-based, as established in Plan 3).
- **Test files sit adjacent to their module**; every source file opens with a `/** ... */` block comment describing its responsibility.
- **Do NOT use `git stash`.** The stash stack is shared with other working trees on this machine and the owner works in them concurrently.

---

## File Structure

```
show-engine/src/
  personKey.ts         — NEW: resolve a stable role key from a participant
  capabilities.ts      — NEW: config × health → effective capability states
  config.ts            — MODIFY: declare which integrations this show has
  contracts.ts         — MODIFY: Panelist gains personKey; LookDefinition gains boxFill
  panelistDb.ts        — MODIFY: compute and carry personKey
  overrideDb.ts        — MODIFY: re-key from pin to PersonKey
  lookDirector.ts      — MODIFY: honour boxFill; manual box assignments
  index.ts             — MODIFY: export the new surface
```

---

### Task 1: Person key resolution

**Files:**
- Create: `show-engine/src/personKey.ts`
- Test: `show-engine/src/personKey.test.ts`

**Interfaces:**
- Consumes: `Participant` and `Identity` from `./contracts.js`; `identityFromName` from `./identity.js`.
- Produces: `type PersonKey = string`, and
  `function resolvePersonKey(participant: { participantId: string; rawName: string }): PersonKey`.
- **Consuming call sites (verified):** Task 3 (`panelistDb.buildPanelistDb`) calls this once per participant and stores the result on the `Panelist`. Task 4 (`overrideDb`) accepts the stored value as its key and never recomputes it. Nothing else calls it.

**Behavior:**

Role overrides are keyed by PIN today, and the panelist join only consults them when a PIN
exists. PINs come from the registry's registration convention, so a registry-less show
cannot assign a host or reader at all. This module produces the replacement key.

Resolution order, mirroring how identity already resolves:

```
pin (when present)  →  normalized display name  →  participantId
```

The PIN and display name both come from `identityFromName(participant.rawName)` — reuse it
rather than re-deriving either.

**Normalization** of the display name: lowercase, collapse internal whitespace runs to a
single space, trim. The PIN is already excluded because `identityFromName` splits it out
into its own field, so no separate stripping step is needed — but a display name that still
contains a bare 4-digit run (a name that failed to parse cleanly) keeps it, which is
correct: that string is what the operator sees and types.

**A name that normalizes to the empty string is treated as absent** and falls through to
`participantId`. This matters: a blank or PIN-only display name must never produce a shared
key that silently merges two people's roles.

Keys are opaque strings and must not be parsed by consumers. Prefix each resolution tier so
two tiers can never collide — a participant whose id happens to equal another's normalized
name must not share a key. Use `pin:`, `name:`, and `id:` respectively.

Accepted consequences, already agreed in the spec: two guests genuinely sharing a display
name in a registry-less show collide and share a role; a host who renames themselves
mid-show loses their role and the operator reassigns. Both are visible and recoverable. In
a registry-backed show, PINs win and neither can occur.

- [ ] **Step 1: Write the failing test**

`show-engine/src/personKey.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { resolvePersonKey } from "./personKey.js";

describe("resolvePersonKey", () => {
  it("prefers the PIN when the name carries one", () => {
    expect(resolvePersonKey({ participantId: "z1", rawName: "Ann Lee | 4242" })).toBe("pin:4242");
  });

  it("falls back to the normalized display name", () => {
    expect(resolvePersonKey({ participantId: "z1", rawName: "Ann Lee" })).toBe("name:ann lee");
  });

  it("normalizes case and whitespace so a retyped name still matches", () => {
    const a = resolvePersonKey({ participantId: "z1", rawName: "Ann   Lee" });
    const b = resolvePersonKey({ participantId: "z9", rawName: "  ann lee  " });
    expect(a).toBe(b);
  });

  it("gives the same key to the same PIN regardless of surrounding name", () => {
    const a = resolvePersonKey({ participantId: "z1", rawName: "Ann Lee | 4242" });
    const b = resolvePersonKey({ participantId: "z9", rawName: "A. Lee | 4242 | Austin" });
    expect(a).toBe(b);
  });

  it("falls through to participantId when the name is empty", () => {
    expect(resolvePersonKey({ participantId: "z1", rawName: "" })).toBe("id:z1");
  });

  it("falls through to participantId when the name is only whitespace", () => {
    expect(resolvePersonKey({ participantId: "z1", rawName: "   " })).toBe("id:z1");
  });

  it("keeps tiers distinct so a participantId cannot collide with a name", () => {
    const byName = resolvePersonKey({ participantId: "z1", rawName: "bob" });
    const byId = resolvePersonKey({ participantId: "bob", rawName: "" });
    expect(byName).not.toBe(byId);
  });

  it("keeps tiers distinct so a PIN cannot collide with a name", () => {
    const byPin = resolvePersonKey({ participantId: "z1", rawName: "Ann | 4242" });
    const byName = resolvePersonKey({ participantId: "z2", rawName: "4242x" });
    expect(byPin).not.toBe(byName);
  });

  it("gives two different people with the same name the same key, by design", () => {
    const a = resolvePersonKey({ participantId: "z1", rawName: "John Smith" });
    const b = resolvePersonKey({ participantId: "z2", rawName: "John Smith" });
    expect(a).toBe(b);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./personKey.js`.

- [ ] **Step 3: Implement**

Write `personKey.ts` per the Interfaces and Behavior above.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS, with every existing test still green.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/personKey.ts show-engine/src/personKey.test.ts
git commit -m "feat(show-engine): person key resolution for role identity"
```

---

### Task 2: Capability contracts and config

**Files:**
- Modify: `show-engine/src/contracts.ts`, `show-engine/src/config.ts`
- Test: `show-engine/src/contracts.test.ts` (append), `show-engine/src/config.test.ts` (append)

**Interfaces:**
- Consumes: existing `parseShowEngineConfig`, `ShowEngineConfig`, and the file's private validation helpers (`asRecord`, `requireString`, `optionalPositiveInt`, and the boolean/enum helpers added for `plateTone` and `tallySource` — read them and follow that idiom).
- Produces, from `contracts.ts`:
  - `type CapabilityState = "available" | "unavailable" | "disabled"`
  - `type Capability = { state: CapabilityState; detail: string | null }`
  - `type ShowCapabilities = { registry: Capability; handsQueue: Capability; questionFeed: Capability }`
  - `type BoxFill = "queue" | "manual"`, `const BOX_FILLS: readonly BoxFill[]`, `function isBoxFill(value: unknown): value is BoxFill`
  - `LookDefinition` gains `boxFill: BoxFill`
- Produces, from `config.ts`: `ShowEngineConfig` gains
  `integrations: { registry: boolean; handsQueue: boolean; questionFeed: boolean }`.
- **Consuming call sites (verified):** Task 5 (`capabilities.resolveCapabilities`) reads `config.integrations` and produces `ShowCapabilities`. Task 6 (`lookDirector`) reads `LookDefinition.boxFill`. Task 7 exports all of these. `isBoxFill` is consumed by `config.ts`'s own validation and by Task 7's barrel for host adapters building look config.

**Behavior:**

`integrations` declares which optional inputs this show is set up for. All three default to
`false` — a show that says nothing gets none of them, which is the safe direction: an
un-configured integration that silently defaulted on would poll a URL nobody set. Each
entry must be a boolean if present; a non-boolean is an error, not a coercion.

`boxFill` on a look is **required on the type** but **optional in config parsing**,
defaulting to `"queue"`. That default preserves existing behavior for every look already
written. Follow the `plateTone`/`tallySource` precedent exactly — read how those are handled
before writing this.

`isBoxFill` narrows only the two listed strings. `BOX_FILLS` lists them in the order given.

`Capability` and `ShowCapabilities` are pure data shapes with no behavior; Task 5 produces
them.

**Expect this to break existing `LookDefinition` fixtures.** `lookDirector.test.ts`,
`directionPipeline.test.ts`, `outputsPipeline.test.ts`, `tallyPublisher.test.ts`, and
`config.test.ts` all build look objects and will fail to typecheck. Add `boxFill: "queue"`
to those fixtures — fixture-only edits, no assertion changes. Say so in your report.

- [ ] **Step 1: Write the failing tests**

Append to `show-engine/src/contracts.test.ts` (extend the import to include `BOX_FILLS` and `isBoxFill`):

```ts
describe("box fill strategies", () => {
  it("lists the two strategies", () => {
    expect(BOX_FILLS).toEqual(["queue", "manual"]);
  });

  it("recognises valid strategies and rejects others", () => {
    expect(isBoxFill("queue")).toBe(true);
    expect(isBoxFill("manual")).toBe(true);
    expect(isBoxFill("auto")).toBe(false);
    expect(isBoxFill(0)).toBe(false);
    expect(isBoxFill(undefined)).toBe(false);
  });
});
```

Append to `show-engine/src/config.test.ts`:

```ts
describe("parseShowEngineConfig integrations", () => {
  it("defaults every integration to off", () => {
    expect(parseShowEngineConfig(minimal).integrations).toEqual({
      registry: false,
      handsQueue: false,
      questionFeed: false
    });
  });

  it("keeps explicitly enabled integrations", () => {
    const config = parseShowEngineConfig({
      ...minimal,
      integrations: { registry: true, handsQueue: true }
    });
    expect(config.integrations).toEqual({
      registry: true,
      handsQueue: true,
      questionFeed: false
    });
  });

  it("rejects a non-boolean integration flag", () => {
    expect(() =>
      parseShowEngineConfig({ ...minimal, integrations: { registry: "yes" } })
    ).toThrow(/integrations\.registry/);
  });

  it("rejects a non-object integrations value", () => {
    expect(() => parseShowEngineConfig({ ...minimal, integrations: true })).toThrow(
      /integrations/
    );
  });
});

describe("parseShowEngineConfig boxFill", () => {
  it("defaults an omitted boxFill to queue", () => {
    const { boxFill: _omitted, ...withoutFill } = look;
    const config = parseShowEngineConfig({ ...minimal, looks: [withoutFill] });
    expect(config.looks[0]?.boxFill).toBe("queue");
  });

  it("keeps an explicit boxFill", () => {
    const config = parseShowEngineConfig({
      ...minimal,
      looks: [{ ...look, boxFill: "manual" }]
    });
    expect(config.looks[0]?.boxFill).toBe("manual");
  });

  it("rejects an unknown boxFill", () => {
    expect(() =>
      parseShowEngineConfig({ ...minimal, looks: [{ ...look, boxFill: "auto" }] })
    ).toThrow(/boxFill/);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `npm run test --workspace show-engine`
Expected: FAIL — `BOX_FILLS` is not exported; `config.integrations` is undefined.

- [ ] **Step 3: Implement**

Extend `contracts.ts` and `config.ts` per the Interfaces and Behavior, then add
`boxFill: "queue"` to the broken fixtures.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/contracts.ts show-engine/src/contracts.test.ts show-engine/src/config.ts show-engine/src/config.test.ts show-engine/src/lookDirector.test.ts show-engine/src/directionPipeline.test.ts show-engine/src/outputsPipeline.test.ts show-engine/src/tallyPublisher.test.ts
git commit -m "feat(show-engine): capability contracts, integration flags, boxFill"
```

---

### Task 3: Carry the person key on the panelist

**Files:**
- Modify: `show-engine/src/contracts.ts`, `show-engine/src/panelistDb.ts`
- Test: `show-engine/src/panelistDb.test.ts` (append)

**Interfaces:**
- Consumes: `resolvePersonKey` from `./personKey.js` (Task 1).
- Produces: `Panelist` gains `personKey: PersonKey`. `buildPanelistDb`'s signature is unchanged.
- **Consuming call sites (verified):** Task 4 (`overrideDb`) is keyed by this value; Task 6 (`lookDirector`) does not read it; Task 8's integration test reads it to assign a role. Every existing consumer of `Panelist` continues to compile because this is an addition.

**Behavior:**

`buildPanelistDb` computes each panelist's key once, via `resolvePersonKey`, and stores it on
the record. Every downstream consumer reads the stored value rather than re-deriving it —
that is the point of storing it, and it guarantees the override lookup and the assignment
action agree on the same key.

The override lookup inside `buildPanelistDb` changes from PIN-keyed to key-keyed:
`overrides[personKey]` rather than `identity.pin === null ? undefined : overrides[identity.pin]`.
That single change is what makes roles assignable in a registry-less show.

Precedence for the other fields is unchanged: override, then registry, then the name-parsed
fallback, field by field.

- [ ] **Step 1: Write the failing tests**

Append to `show-engine/src/panelistDb.test.ts`:

```ts
describe("buildPanelistDb person keys", () => {
  it("stores a PIN-derived key when the name carries a PIN", () => {
    const db = buildPanelistDb([participant("p1", "JJ | 1383")], mukana, {});
    expect(db.get("p1")?.personKey).toBe("pin:1383");
  });

  it("stores a name-derived key when there is no PIN", () => {
    const db = buildPanelistDb([participant("p2", "Guest User")], mukana, {});
    expect(db.get("p2")?.personKey).toBe("name:guest user");
  });

  it("applies an override keyed by the person key, with no PIN present", () => {
    const db = buildPanelistDb([participant("p2", "Guest User")], mukana, {
      "name:guest user": {
        personKey: "name:guest user",
        displayName: "Guest User",
        location: "Remote",
        role: "host"
      }
    });
    expect(db.get("p2")).toMatchObject({ role: "host", location: "Remote" });
  });

  it("applies an override keyed by PIN when one is present", () => {
    const db = buildPanelistDb([participant("p1", "JJ | 1383")], mukana, {
      "pin:1383": {
        personKey: "pin:1383",
        displayName: "",
        location: "",
        role: "reader"
      }
    });
    expect(db.get("p1")).toMatchObject({ role: "reader", displayName: "J.J. Mc Kenna" });
  });

  it("gives a reconnecting participant the same key under a new participant id", () => {
    const first = buildPanelistDb([participant("p1", "Guest User")], mukana, {});
    const second = buildPanelistDb([participant("p9", "Guest User")], mukana, {});
    expect(second.get("p9")?.personKey).toBe(first.get("p1")?.personKey);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `npm run test --workspace show-engine`
Expected: FAIL — `personKey` is undefined on the panelist, and `OverrideRecord` has no
`personKey` field yet. The override-keyed tests will not typecheck until Task 4 lands; that
is expected and is why Task 4 follows immediately.

**If the type error blocks you from running the suite at all**, implement Task 3's
production change first (adding `personKey` to `Panelist` and switching the lookup), leave
the two override tests written but failing, and report that as your RED state — do not
delete them.

- [ ] **Step 3: Implement**

Extend `contracts.ts` and `panelistDb.ts` per the Interfaces and Behavior.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine`
Expected: the three non-override tests PASS. The two override tests may still fail on
`OverrideRecord`'s shape until Task 4 — say which state you ended in.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/contracts.ts show-engine/src/panelistDb.ts show-engine/src/panelistDb.test.ts
git commit -m "feat(show-engine): carry the person key on the panelist"
```

---

### Task 4: Re-key the override table

**Files:**
- Modify: `show-engine/src/overrideDb.ts`
- Test: `show-engine/src/overrideDb.test.ts`

**Interfaces:**
- Consumes: `PersonKey` from `./personKey.js`; `MukanaDb`, `Role` from `./contracts.js`.
- Produces: `OverrideRecord` becomes `{ personKey: PersonKey; displayName: string; location: string; role: Role }`. `OverrideDb`'s methods re-key:
  - `set(record: OverrideRecord): void`
  - `delete(personKey: PersonKey): void`
  - `roleFor(personKey: PersonKey): Role | undefined`
  - `entries(): Record<PersonKey, OverrideRecord>`
  - `assignExclusiveRole(personKey: PersonKey, role: "host" | "reader", registry: MukanaDb): void`
  - `clear()` and `restore(entries)` unchanged in shape
- **Consuming call sites (verified):** Task 3 (`buildPanelistDb`) reads `entries()` and indexes it by `Panelist.personKey`. Task 8's integration test calls `assignExclusiveRole`. Plan 5's orchestrator will expose it as `ohg.panelist.role.set`; that action's parameter becomes a person key rather than a PIN, which Plan 5 must account for.

**Behavior:**

Straight re-key: everywhere the table said "pin", it now says "person key". The `pin` field
on `OverrideRecord` becomes `personKey`.

`assignExclusiveRole` keeps its guarantee — exactly one holder of an exclusive role across
both the override table and the registry — and keeps its demote-then-assign algorithm
unchanged. Only the key type moves. Note the registry parameter is still a `MukanaDb` keyed
by PIN, because that is what the registry is; when demoting a registry-declared holder, its
override row is written under `pin:<PIN>`, which is exactly the key
`resolvePersonKey` produces for a participant carrying that PIN. **That correspondence is
the reason the `pin:` prefix exists** — without it, a demotion written by the registry path
would never match the key computed from a participant.

With no registry the method enforces across the override table alone, which is already its
behavior with an empty registry. The guarantee is unchanged.

Identity fallback on assignment is unchanged: registry record, then any pre-existing
override row, then empty strings.

- [ ] **Step 1: Update the existing tests and add the new ones**

Rewrite `overrideDb.test.ts`'s fixtures and calls to use person keys. Every existing
assertion stays; only the keys and the `pin` field name change. The registry fixture keeps
its PIN keys — it is a registry, not an override table. Then append:

```ts
describe("OverrideDb without a registry", () => {
  it("assigns a role to a name-keyed person", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("name:guest user", "host", {});
    expect(db.roleFor("name:guest user")).toBe("host");
  });

  it("still enforces one holder with no registry present", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("name:ann lee", "host", {});
    db.assignExclusiveRole("name:bo diaz", "host", {});
    const hosts = Object.values(db.entries()).filter((entry) => entry.role === "host");
    expect(hosts.map((entry) => entry.personKey)).toEqual(["name:bo diaz"]);
  });

  it("writes a demoted registry holder under its pin-prefixed key", () => {
    const registry: MukanaDb = {
      "1383": {
        pin: "1383",
        displayName: "J.J. Mc Kenna",
        location: "CA",
        role: "host",
        online: true
      }
    };
    const db = new OverrideDb();
    db.assignExclusiveRole("name:ann lee", "host", registry);
    expect(db.entries()["pin:1383"]).toEqual({
      personKey: "pin:1383",
      displayName: "J.J. Mc Kenna",
      location: "CA",
      role: "panelist"
    });
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `npm run test --workspace show-engine`
Expected: FAIL — `OverrideRecord` still has `pin`, and the new tests reference `personKey`.

- [ ] **Step 3: Implement**

Re-key `overrideDb.ts` per the Interfaces and Behavior.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS — including Task 3's two override tests, which should now compile and pass.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/overrideDb.ts show-engine/src/overrideDb.test.ts
git commit -m "feat(show-engine): re-key role overrides by person key"
```

---

### Task 5: Capability resolution

**Files:**
- Create: `show-engine/src/capabilities.ts`
- Test: `show-engine/src/capabilities.test.ts`

**Interfaces:**
- Consumes: `ShowEngineConfig` from `./config.js`; `Capability`, `CapabilityState`, `ShowCapabilities` from `./contracts.js`; `MukanaHealth`, `MukanaEndpoint` from `./mukanaClient.js`.
- Produces:
  - `type HealthByEndpoint = Record<MukanaEndpoint, MukanaHealth>`
  - `function resolveCapabilities(config: ShowEngineConfig, health: HealthByEndpoint): ShowCapabilities`
  - `function canUse(capability: Capability): boolean`
- **Consuming call sites (verified):** Task 6 (`lookDirector.effectiveBoxFill`) calls `canUse` on the `handsQueue` capability. Plan 5's orchestrator calls `resolveCapabilities` once per tick and publishes the result. Task 7 exports both. Nothing constructs a `Capability` by hand outside this module and tests.

**Behavior:**

Pure function, no I/O, no state.

Each of the three capabilities maps to one Mukana endpoint: `registry` → `panelists`,
`handsQueue` → `hands`, `questionFeed` → `question`.

Resolution:
- Not enabled in `config.integrations` → `{ state: "disabled", detail: null }`.
- Enabled, endpoint health `ok` → `{ state: "available", detail: null }`.
- Enabled, endpoint health `failing` → `{ state: "unavailable", detail: <the health record's detail> }`.
- Enabled, endpoint health `dormant` → `{ state: "unavailable", detail: <the health record's detail> }`.

Dormant resolving to `unavailable` is correct rather than a compromise: outside show hours
the registry genuinely is not there, and the show should behave as though it has none until
it appears.

`detail` is `null` for `available` and `disabled`, and carries the health record's detail
for `unavailable` — falling back to a short generic string when the health record's detail
is `null`, so an operator never sees an empty explanation.

`canUse(capability)` returns `true` only for `available`. It exists so consumers read
`canUse(caps.handsQueue)` rather than `caps.handsQueue.state === "available"` — the point is
that no consumer distinguishes `unavailable` from `disabled`, and a named predicate makes
that discipline visible at every call site.

`resolveCapabilities` returns a fresh object each call.

- [ ] **Step 1: Write the failing test**

`show-engine/src/capabilities.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { canUse, resolveCapabilities, type HealthByEndpoint } from "./capabilities.js";
import { parseShowEngineConfig } from "./config.js";

const base = {
  capacity: 8,
  statePath: "/show/state.json",
  mukana: { baseUrl: "https://example.com/rest.php", event: "officehours" }
};

function health(overrides: Partial<HealthByEndpoint> = {}): HealthByEndpoint {
  const ok = { state: "ok" as const, consecutiveFailures: 0, detail: null };
  return { panelists: ok, hands: ok, question: ok, ...overrides };
}

const allOn = parseShowEngineConfig({
  ...base,
  integrations: { registry: true, handsQueue: true, questionFeed: true }
});

describe("resolveCapabilities", () => {
  it("reports every capability disabled when nothing is configured", () => {
    const caps = resolveCapabilities(parseShowEngineConfig(base), health());
    expect(caps.registry).toEqual({ state: "disabled", detail: null });
    expect(caps.handsQueue).toEqual({ state: "disabled", detail: null });
    expect(caps.questionFeed).toEqual({ state: "disabled", detail: null });
  });

  it("reports a configured, healthy integration as available", () => {
    const caps = resolveCapabilities(allOn, health());
    expect(caps.registry).toEqual({ state: "available", detail: null });
  });

  it("maps each capability to its own endpoint", () => {
    const caps = resolveCapabilities(
      allOn,
      health({ hands: { state: "failing", consecutiveFailures: 2, detail: "HTTP 503" } })
    );
    expect(caps.handsQueue.state).toBe("unavailable");
    expect(caps.registry.state).toBe("available");
    expect(caps.questionFeed.state).toBe("available");
  });

  it("carries the health detail on an unavailable capability", () => {
    const caps = resolveCapabilities(
      allOn,
      health({ hands: { state: "failing", consecutiveFailures: 1, detail: "HTTP 503" } })
    );
    expect(caps.handsQueue.detail).toBe("HTTP 503");
  });

  it("treats dormant as unavailable and keeps its detail", () => {
    const caps = resolveCapabilities(
      allOn,
      health({
        panelists: { state: "dormant", consecutiveFailures: 0, detail: "outside show hours" }
      })
    );
    expect(caps.registry).toEqual({ state: "unavailable", detail: "outside show hours" });
  });

  it("supplies a detail when the health record has none", () => {
    const caps = resolveCapabilities(
      allOn,
      health({ hands: { state: "failing", consecutiveFailures: 1, detail: null } })
    );
    expect(caps.handsQueue.state).toBe("unavailable");
    expect(caps.handsQueue.detail).not.toBeNull();
    expect(caps.handsQueue.detail).not.toBe("");
  });

  it("reports disabled regardless of health when not configured", () => {
    const caps = resolveCapabilities(
      parseShowEngineConfig(base),
      health({ hands: { state: "failing", consecutiveFailures: 9, detail: "HTTP 503" } })
    );
    expect(caps.handsQueue).toEqual({ state: "disabled", detail: null });
  });

  it("returns a fresh object each call", () => {
    const first = resolveCapabilities(allOn, health());
    first.registry.state = "disabled";
    expect(resolveCapabilities(allOn, health()).registry.state).toBe("available");
  });
});

describe("canUse", () => {
  it("is true only for available", () => {
    expect(canUse({ state: "available", detail: null })).toBe(true);
    expect(canUse({ state: "unavailable", detail: "HTTP 503" })).toBe(false);
    expect(canUse({ state: "disabled", detail: null })).toBe(false);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./capabilities.js`.

- [ ] **Step 3: Implement**

Write `capabilities.ts` per the Interfaces and Behavior.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/capabilities.ts show-engine/src/capabilities.test.ts
git commit -m "feat(show-engine): capability resolution from config and health"
```

---

### Task 6: Box fill strategy and manual assignments

**Files:**
- Modify: `show-engine/src/lookDirector.ts`
- Test: `show-engine/src/lookDirector.test.ts` (append)

**Interfaces:**
- Consumes: `BoxFill`, `Capability`, `LookDefinition`, `QueueState`, `Slot` from `./contracts.js`; `canUse` from `./capabilities.js`; existing `queueOrder`, `pageCountFor`, `clampPage`, `resolveLook`.
- Produces:
  - `function effectiveBoxFill(look: LookDefinition, handsQueue: Capability): BoxFill`
  - `type ManualBoxAssignments = Record<number, number>` — box number → roster slot
  - `resolveLook`'s context gains two optional fields: `handsQueue?: Capability` and `manualBoxes?: ManualBoxAssignments`
  - `LookResolution` gains `boxFill: BoxFill` — the *effective* strategy, not the declared one
- **Consuming call sites (verified):** Plan 5's orchestrator passes `handsQueue` and `manualBoxes` on every `resolveLook` call and reads `resolution.boxFill` to decide whether paging actions are live. Task 8's integration test does the same. `pageCountFor` and `clampPage` are unchanged and still take only the look and the queue.

**Behavior:**

`effectiveBoxFill` returns `"queue"` only when the look declares `"queue"` **and**
`canUse(handsQueue)` is true. Otherwise it returns `"manual"`. A look that declares
`"manual"` is always `"manual"`. When `handsQueue` is omitted from the context, treat it as
unusable — an absent capability is not an available one.

That single rule is the whole degradation story: a hands feed that dies turns a queue-filled
look into a manually-filled one on the next tick, and the operator keeps working.

**Under `"queue"`**, `resolveLook` behaves exactly as it does today: boxes fill from
`queueOrder(queue)` windowed by `page`, and `pageCount` comes from `pageCountFor`.

**Under `"manual"`**:
- Box *n* takes `manualBoxes[n]` when present and that slot is occupied in the roster;
  otherwise the box is `null`.
- A manual assignment naming an unoccupied or out-of-range slot yields `null` for that box
  rather than throwing. Manual assignments outlive roster churn — the person in slot 3 may
  leave — and a stale assignment must not take the engine down mid-show.
- `page` is `0` and `pageCount` is `1`. A non-zero `page` under manual fill throws, exactly
  as an out-of-range page does under queue fill: the operator's page controls should be
  inert, and a caller passing a page anyway is a bug worth surfacing.
- Nameplates are emitted for filled boxes on the same rule as today — one per occupied
  position, none for an empty box.

`LookResolution.boxFill` carries the *effective* strategy so a surface can render paging
controls correctly without recomputing the decision.

- [ ] **Step 1: Write the failing tests**

Append to `show-engine/src/lookDirector.test.ts` (extend the import to include
`effectiveBoxFill`):

```ts
const available = { state: "available" as const, detail: null };
const unavailable = { state: "unavailable" as const, detail: "HTTP 503" };
const disabled = { state: "disabled" as const, detail: null };

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
```

**Note on the existing tests:** every current `resolveLook` test omits `handsQueue`, so
under the new rule they all resolve to `"manual"` and their box expectations will break.
Add `handsQueue: available` to those existing calls — a fixture-only edit that preserves
their intent, since they were written to test queue-filling. Say so in your report.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `npm run test --workspace show-engine`
Expected: FAIL — `effectiveBoxFill` is not exported and `LookResolution` has no `boxFill`.

- [ ] **Step 3: Implement**

Extend `lookDirector.ts` per the Interfaces and Behavior.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/lookDirector.ts show-engine/src/lookDirector.test.ts
git commit -m "feat(show-engine): box fill strategy with manual fallback"
```

---

### Task 7: Exports

**Files:**
- Modify: `show-engine/src/index.ts`
- Test: `show-engine/src/index.test.ts` (create)

**Interfaces:**
- Consumes: everything built in Tasks 1–6.
- Produces: the package's public surface for the capability layer.

**Behavior:**

`index.ts` gains: `PersonKey`, `resolvePersonKey`; `CapabilityState`, `Capability`,
`ShowCapabilities`, `BoxFill`, `BOX_FILLS`, `isBoxFill`; `HealthByEndpoint`,
`resolveCapabilities`, `canUse`; `effectiveBoxFill`, `ManualBoxAssignments`.

**Verify every name against the actual source before writing the barrel.** Plan 3 shipped a
type that consumers outside the package could not name; this test exists so that cannot
happen again silently.

The new test asserts the barrel is *complete* rather than merely non-empty: it imports each
name and asserts it is defined. A type-only export cannot be asserted at runtime, so import
types in a type position and let the typecheck carry them — a file that fails to typecheck
is a failing test.

- [ ] **Step 1: Write the failing test**

`show-engine/src/index.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import {
  BOX_FILLS,
  canUse,
  effectiveBoxFill,
  isBoxFill,
  resolveCapabilities,
  resolvePersonKey,
  type Capability,
  type CapabilityState,
  type HealthByEndpoint,
  type ManualBoxAssignments,
  type PersonKey,
  type ShowCapabilities,
  type BoxFill
} from "./index.js";

describe("capability layer exports", () => {
  it("exports every runtime value the capability layer needs", () => {
    expect(resolvePersonKey).toBeTypeOf("function");
    expect(resolveCapabilities).toBeTypeOf("function");
    expect(canUse).toBeTypeOf("function");
    expect(effectiveBoxFill).toBeTypeOf("function");
    expect(isBoxFill).toBeTypeOf("function");
    expect(BOX_FILLS).toEqual(["queue", "manual"]);
  });

  it("exports every type a host adapter needs to name", () => {
    const key: PersonKey = "pin:1383";
    const state: CapabilityState = "available";
    const capability: Capability = { state, detail: null };
    const caps: ShowCapabilities = {
      registry: capability,
      handsQueue: capability,
      questionFeed: capability
    };
    const fill: BoxFill = "manual";
    const manual: ManualBoxAssignments = { 1: 3 };
    const health: HealthByEndpoint = {
      panelists: { state: "ok", consecutiveFailures: 0, detail: null },
      hands: { state: "ok", consecutiveFailures: 0, detail: null },
      question: { state: "ok", consecutiveFailures: 0, detail: null }
    };
    expect([key, caps, fill, manual, health]).toHaveLength(5);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — several names are not exported from `./index.js`.

- [ ] **Step 3: Extend the barrel**

Add the exports per the Behavior above. If a name does not exist under the spelling listed,
**stop and report NEEDS_CONTEXT** rather than renaming anything in another module.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS, no type errors.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/index.ts show-engine/src/index.test.ts
git commit -m "feat(show-engine): export the capability layer"
```

---

### Task 8: The degradation-equivalence property, and the pipeline test

**Files:**
- Test: `show-engine/src/capabilityPipeline.test.ts` (create)

**Interfaces:**
- Consumes: the full public surface via `./index.js`.
- Produces: nothing. This task is tests only.

**Behavior:**

This is the plan's real deliverable. Two things get proven.

**1. The degradation-equivalence property.** For the same roster, look, queue, and program
state, engine output with a capability `unavailable` must be **identical** to output with
that capability `disabled`. This is the design's load-bearing guarantee — if it ever fails,
a mid-show outage stops behaving like a show that never had the integration, and the
third-party risk this plan exists to remove is back.

**The invariant this test must break on:** any code that branches on `state === "disabled"`
versus `state === "unavailable"` outside the operator-facing `detail` field. If someone adds
such a branch, this test fails.

**2. The composed degradation scenario.** A registry-backed show loses its hands feed
mid-tick and keeps working.

**The invariant this test must break on:** `resolveLook` emptying the guest boxes when the
hands capability stops being usable, rather than falling back to the operator's manual
assignments. If the fallback is removed or inverted, the boxes go empty and the test fails.

- [ ] **Step 1: Write the failing test**

`show-engine/src/capabilityPipeline.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import {
  buildPanelistDb,
  deriveTally,
  LiveSlots,
  OverlayDirector,
  OverrideDb,
  parseHandsPayload,
  resolveLook,
  resolvePersonKey,
  ZoomIngest,
  type Capability,
  type LookDefinition,
  type ManualBoxAssignments,
  type MukanaDb,
  type Participant
} from "./index.js";

function participant(participantId: string, rawName: string): Participant {
  return {
    participantId,
    rawName,
    online: true,
    videoOn: true,
    audioOn: false,
    handRaised: false,
    zoomRole: 3
  };
}

const registry: MukanaDb = {
  "1383": { pin: "1383", displayName: "J.J. Mc Kenna", location: "CA", role: "host", online: true },
  "4242": { pin: "4242", displayName: "Ann Lee", location: "TX", role: "panelist", online: true },
  "5555": { pin: "5555", displayName: "Bo Diaz", location: "PE", role: "panelist", online: true }
};

const teatime: LookDefinition = {
  id: "teatime",
  label: "Teatime",
  scenePreset: "scene-teatime",
  boxes: 2,
  includesHost: true,
  includesReader: false,
  plateTone: "accent",
  tallySource: "boxes",
  boxFill: "queue"
};

const unavailable: Capability = { state: "unavailable", detail: "HTTP 503" };
const disabled: Capability = { state: "disabled", detail: null };
const available: Capability = { state: "available", detail: null };

function roster(names: readonly (readonly [string, string])[]): LiveSlots {
  const ingest = new ZoomIngest();
  for (const [id, name] of names) {
    ingest.apply({ kind: "joined", participant: participant(id, name) });
  }
  ingest.commit();
  const slots = new LiveSlots({ capacity: 8, utilityPinBase: 9000 });
  slots.rebuild([
    ...buildPanelistDb(ingest.snapshot(), registry, new OverrideDb().entries()).values()
  ]);
  return slots;
}

const manualBoxes: ManualBoxAssignments = { 1: 1, 2: 2 };

describe("degradation equivalence", () => {
  const slots = roster([
    ["z-ann", "Ann | 4242"],
    ["z-bo", "Bo | 5555"],
    ["z-host", "JJ | 1383"]
  ]);
  const parsed = parseHandsPayload("5555\n4242\nNONE");

  it("resolves a look identically whether hands are unavailable or disabled", () => {
    if (parsed.kind !== "data") throw new Error("fixture");
    const context = { queue: parsed.queue, slots: slots.slots(), page: 0, manualBoxes };
    expect(resolveLook(teatime, { ...context, handsQueue: unavailable })).toEqual(
      resolveLook(teatime, { ...context, handsQueue: disabled })
    );
  });

  it("derives tally identically whether hands are unavailable or disabled", () => {
    if (parsed.kind !== "data") throw new Error("fixture");
    const context = { queue: parsed.queue, slots: slots.slots(), page: 0, manualBoxes };
    const tallyFor = (handsQueue: Capability) =>
      deriveTally({
        source: { kind: "look", lookId: "teatime" },
        slots: slots.slots(),
        gallery: [],
        look: resolveLook(teatime, { ...context, handsQueue }),
        activeSpeakerSlot: null
      });
    expect(tallyFor(unavailable)).toEqual(tallyFor(disabled));
  });

  it("produces identical overlays whether hands are unavailable or disabled", () => {
    if (parsed.kind !== "data") throw new Error("fixture");
    const context = { queue: parsed.queue, slots: slots.slots(), page: 0, manualBoxes };
    const stateFor = (handsQueue: Capability) => {
      const director = new OverlayDirector();
      director.update({
        look: resolveLook(teatime, { ...context, handsQueue }),
        question: null,
        questionVisible: false
      });
      return director.state();
    };
    expect(stateFor(unavailable)).toEqual(stateFor(disabled));
  });
});

describe("surviving a hands outage mid-show", () => {
  const slots = roster([
    ["z-ann", "Ann | 4242"],
    ["z-bo", "Bo | 5555"],
    ["z-host", "JJ | 1383"]
  ]);

  it("keeps guests on screen when the hands feed dies", () => {
    const parsed = parseHandsPayload("5555\n4242\nNONE");
    if (parsed.kind !== "data") throw new Error("fixture");
    const context = { queue: parsed.queue, slots: slots.slots(), page: 0 };

    const before = resolveLook(teatime, { ...context, handsQueue: available });
    expect(before.boxFill).toBe("queue");
    expect(before.boxes.every((box) => box.slot !== null)).toBe(true);

    const after = resolveLook(teatime, {
      ...context,
      handsQueue: unavailable,
      manualBoxes
    });
    expect(after.boxFill).toBe("manual");
    expect(after.boxes.every((box) => box.slot !== null)).toBe(true);
  });

  it("keeps the host chair and its nameplate through the outage", () => {
    const parsed = parseHandsPayload("5555\n4242\nNONE");
    if (parsed.kind !== "data") throw new Error("fixture");
    const after = resolveLook(teatime, {
      queue: parsed.queue,
      slots: slots.slots(),
      page: 0,
      handsQueue: unavailable,
      manualBoxes
    });
    expect(after.hostSlot).toBe(slots.slotOf("z-host"));
    expect(after.nameplates.some((plate) => plate.position.kind === "host")).toBe(true);
  });
});

describe("a show with no registry at all", () => {
  const slots = roster([
    ["z-ann", "Ann Lee"],
    ["z-bo", "Bo Diaz"]
  ]);

  it("still seats everyone with names parsed from Zoom", () => {
    expect(slots.occupiedCount()).toBe(2);
    expect(slots.slots()[0]?.panelist?.displayName).toBe("Ann Lee");
  });

  it("lets the operator assign a host with no PIN present", () => {
    const key = resolvePersonKey({ participantId: "z-ann", rawName: "Ann Lee" });
    const overrides = new OverrideDb();
    overrides.assignExclusiveRole(key, "host", {});

    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant("z-ann", "Ann Lee") });
    ingest.apply({ kind: "joined", participant: participant("z-bo", "Bo Diaz") });
    ingest.commit();

    const db = buildPanelistDb(ingest.snapshot(), {}, overrides.entries());
    expect(db.get("z-ann")?.role).toBe("host");
    expect(db.get("z-bo")?.role).toBe("panelist");
  });

  it("resolves the host chair from that assignment", () => {
    const key = resolvePersonKey({ participantId: "z-ann", rawName: "Ann Lee" });
    const overrides = new OverrideDb();
    overrides.assignExclusiveRole(key, "host", {});

    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant("z-ann", "Ann Lee") });
    ingest.apply({ kind: "joined", participant: participant("z-bo", "Bo Diaz") });
    ingest.commit();

    const seated = new LiveSlots({ capacity: 8, utilityPinBase: 9000 });
    seated.rebuild([...buildPanelistDb(ingest.snapshot(), {}, overrides.entries()).values()]);

    const resolution = resolveLook(teatime, {
      queue: { previous: [], current: null, upcoming: [] },
      slots: seated.slots(),
      page: 0,
      manualBoxes: { 1: seated.slotOf("z-bo") ?? 0 }
    });
    expect(resolution.hostSlot).toBe(seated.slotOf("z-ann"));
    expect(resolution.boxFill).toBe("manual");
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — the file cannot resolve several names until Task 7's barrel is in place; if
Task 7 is done, expect failures only where the composition is genuinely wrong.

- [ ] **Step 3: Fix what the test exposes**

There is no new production code in this task. If a test fails, the defect is in Tasks 1–6 —
fix it there, and say which task and why in your report. Do not weaken a test to make it
pass.

- [ ] **Step 4: Run the full suite, typecheck, and build**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine && npm run build --workspace show-engine`
Expected: PASS, no type errors, `dist/` emitted with declarations.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/capabilityPipeline.test.ts
git commit -m "test(show-engine): degradation equivalence and outage survival"
```

---

## Definition of Done

- [ ] `npm run test --workspace show-engine` passes, including every Plan 1–3 test.
- [ ] `npm run typecheck --workspace show-engine` is clean.
- [ ] `npm run build --workspace show-engine` emits `dist/` with declarations.
- [ ] The package remains registered in the root `workspaces` array **and** in the `test:gate` chain.
- [ ] The degradation-equivalence property is tested for at least look resolution, tally, and overlays.
- [ ] A registry-less show can seat a roster, assign a host, and resolve a look — proven by Task 8.
- [ ] All work is committed on `plan/show-engine-capabilities`.

## Spec amendments this plan requires

Documentation edits to `docs/superpowers/specs/2026-08-04-ohg-show-engine-design.md`, per
§7 of the capabilities spec. Fold them into the final task's commit — Plan 3 skipped its
equivalent section and needed a fix round for it.

- **§3.4** — `overrideDb` is keyed by `PersonKey`, not PIN.
- **§3.5** — `panelistDb` computes and carries `personKey` on each `Panelist`.
- **§3.9** — `lookDirector` honours `boxFill`; `handsQueue` is an optional input.
- **§4.3** — the state snapshot gains a `capabilities` node.
- **§4.2** — add `ohg.look.box.assign {box, slot}` and `ohg.look.box.clear {box}`; note that
  `nextGuest`/`prevGuest` refuse under manual fill.
- **§7** — record that the registry is optional at runtime and that no integration failure
  may block a tick.

## What Plan 5 picks up

The `HostAdapter` port with its mock and conformance suite, and the orchestrator — which is
where the capability model actually gets *used*: resolving capabilities once per tick,
publishing the `capabilities` node, gating active-speaker events through
`shouldFollowSpeaker` before dispatch (the wiring that finally closes Plan 2's gap #2),
clamping look pages, holding manual box assignments, and registering the `ohg.*` actions
including the two new box actions and the typed refusals.

Two decisions from the capabilities spec that Plan 5 owns: `ohg.panelist.role.set` takes a
person key rather than a PIN, and manual box assignments are **not** reclaimed when an
integration recovers — they persist until the look changes.

Also still carried from Plan 3's outcomes: the parked `mukanaClient` doc comment claiming
`applyHealth` is the only health writer; the `ohg.gfx.headline.*` actions that have no
referent in `OverlayDirector`; the `TallyState` parallel-array shape; `ProgramBus.onActiveSpeaker`
taking `Role` where the orchestrator will hold `Role | null`; and extracting the duplicated
recency helpers in `speakerRecency.ts`.
