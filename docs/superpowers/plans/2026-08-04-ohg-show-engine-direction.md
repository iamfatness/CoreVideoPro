# OHG Show Engine — Plan 2: Direction

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the show engine's decision layer — given a live roster, active-speaker events, and the hands-raised queue, decide who occupies each gallery cell and each look box, and what is on program and preview.

**Architecture:** Pure logic modules extending the `show-engine/` package built in Plan 1. Every module in this plan is deterministic and I/O-free: they consume state and emit decisions. Turning decisions into host commands, graphics calls, and tally publishes is Plan 3.

**Tech Stack:** TypeScript 5.9 (strict, ES2022, NodeNext), vitest 4, Node 24.

**Source documents:**
- Spec: `docs/superpowers/specs/2026-08-04-ohg-show-engine-design.md`
- Algorithm reference: `docs/superpowers/specs/2026-08-04-ohg-isadora-actor-reference.md`
- **Plan 1 outcomes — read before starting:** `docs/superpowers/plans/2026-08-04-ohg-show-engine-core-outcomes.md`

**Plan series (this is Plan 2 of 7):**
1. Core identity & roster — **shipped** (PR #363)
2. **Direction** ← this plan
3. Outputs & host integration (tally, overlay/nameplate rendering, `HostAdapter` port + conformance suite, orchestrator)
4. CVP Windows host integration
5. mac-shell control server + panel
6. OBS plugin adapter
7. Migration tooling

## A note on this plan's format

Plan 1 supplied complete implementation bodies. Five review findings originated in those
bodies — implementers transcribed them faithfully, bugs included, while reviewers caught
the bugs by reading the *prose*. This plan therefore supplies:

- **Interfaces** — exact names, parameter types, and return types. Binding.
- **Behavior** — what the code must do, including every rule and edge case. Binding.
- **Test code** — complete and binding. The tests are the executable specification.
- **No implementation bodies.** Write the implementation yourself from the interface and
  the behavior. If the behavior text seems to contradict a test, stop and report
  NEEDS_CONTEXT rather than guessing.

## Global Constraints

- **Branch:** `plan/ohg-show-engine-direction` (stacked on `spec/ohg-show-engine`, which is PR #363 and may not be modified). Commit after every task.
- **Module system:** NodeNext. **Every relative import MUST end in `.js`** (e.g. `from "./contracts.js"`) even though the source file is `.ts`.
- **vitest runs with `globals: false`** — every test file must explicitly `import { describe, expect, it } from "vitest";`.
- **Strict TypeScript.** No `any`. No non-null assertions (`!`) where a guard will do.
- **No I/O anywhere in this plan.** Every module is pure logic over in-memory state. No network, no filesystem, no timers.
- **Copy, do not alias.** Accessors return structures the caller cannot use to reach internal state; mutators clone what they are given. This discipline is uniform across the package and five Plan 1 review findings came from breaking it. `Panelist`, `Slot`, `GalleryCell`, and every type introduced here are flat, so a shallow per-object copy suffices.
- **Loud, never silent.** Invalid arguments throw with actionable messages; parse failures return typed outcomes. Nothing is silently clamped, truncated, or swallowed.
- **Test file placement:** adjacent to the module, `foo.test.ts` beside `foo.ts`.
- **Module header:** every source file opens with a `/** ... */` block comment describing its responsibility.
- **Do NOT use `git stash`.** The stash stack is shared with other working trees on this machine and the owner works in them concurrently.
- **`OverrideDb` is authoritative for editorial roles.** Nothing in this plan may assign or mutate a role. Modules here read `Panelist.role` and never write it.

---

## File Structure

```
show-engine/src/
  contracts.ts         — MODIFY: add LookDefinition, PlateTone, GalleryCell, QueueState,
                         ProgramSource and their guards
  config.ts            — MODIFY: add skipRoles and looks to ShowEngineConfig
  zoomIngest.ts        — MODIFY: add a monotonic revision counter (carried from Plan 1)
  mukanaClient.ts      — MODIFY: per-endpoint health; add hands and question fetches
                         (carried from Plan 1)
  handsQueue.ts        — NEW: parse the hands payload; strip the host/reader chairs
  speakerRecency.ts    — NEW: FiloAssigner, VisibleSetAssigner, RecencyScores
  galleryDirector.ts   — NEW: the 16-cell gallery and its smart-gallery variant
  lookDirector.ts      — NEW: resolve a look + queue + roster into box assignments
  programBus.ts        — NEW: program/preview state machine and active-speaker follow
  persistence.ts       — MODIFY: ShowState gains a gallery node
  index.ts             — MODIFY: export everything new
```

---

### Task 1: Contracts and config for direction

**Files:**
- Modify: `show-engine/src/contracts.ts`
- Modify: `show-engine/src/config.ts`
- Test: `show-engine/src/contracts.test.ts` (append), `show-engine/src/config.test.ts` (append)

**Interfaces:**
- Consumes: existing `Role`, `ROLES`, `isRole`, `coerceRole` from `contracts.ts`; `parseShowEngineConfig`, `ShowEngineConfig` from `config.ts`.
- Produces, from `contracts.ts`:
  - `type PlateTone = "neutral" | "accent" | "guest" | "breaking"`
  - `const PLATE_TONES: readonly PlateTone[]`
  - `function isPlateTone(value: unknown): value is PlateTone`
  - `type LookDefinition = { id: string; label: string; scenePreset: string; boxes: number; includesHost: boolean; includesReader: boolean; plateTone: PlateTone }`
  - `type GalleryCell = { cell: number; slot: number }` — `slot: 0` means blank
  - `type QueueState = { previous: string[]; current: string | null; upcoming: string[] }` — all entries are 4-digit PIN strings
  - `type ProgramSource = { kind: "look"; lookId: string } | { kind: "gallery" } | { kind: "slot"; slot: number } | { kind: "activeSpeaker" } | { kind: "black" }`
  - `function programSourcesEqual(a: ProgramSource, b: ProgramSource): boolean`
- Produces, from `config.ts`: `ShowEngineConfig` gains `skipRoles: Role[]` and `looks: LookDefinition[]`.

**Behavior:**

`isPlateTone` narrows only the four listed strings. `PLATE_TONES` lists them in the order given above. These four mirror `PlateTone` in the app's existing `src/engine/lowerThird.ts` so the host adapter maps them 1:1 onto CVP's lower-third renderer; the show engine keeps its own copy rather than importing across the package boundary.

`programSourcesEqual` compares discriminant and payload — two `slot` sources are equal only when their slot numbers match; two `look` sources only when their `lookId` matches; `gallery`, `activeSpeaker`, and `black` are equal to themselves.

`parseShowEngineConfig` gains two fields:
- `skipRoles` — optional, defaults to `["aslinterpreter"]`. Every entry must be a valid `Role`; an unknown role is an error, not a coercion, because a typo here silently disables the protection it exists to provide.
- `looks` — optional, defaults to `[]`. When present it must be an array; each entry must carry a non-empty `id`, a non-empty `label`, a non-empty `scenePreset`, an integer `boxes` in `0..4`, booleans `includesHost`/`includesReader`, and a valid `plateTone` (optional, defaulting to `"neutral"`). Duplicate `id` values are an error.

All new validation errors follow the existing `show-engine config.<path>: <problem>` message shape.

- [ ] **Step 1: Write the failing tests**

Append to `show-engine/src/contracts.test.ts`:

```ts
describe("plate tones", () => {
  it("lists the four tones", () => {
    expect(PLATE_TONES).toEqual(["neutral", "accent", "guest", "breaking"]);
  });

  it("recognises valid tones and rejects others", () => {
    expect(isPlateTone("guest")).toBe(true);
    expect(isPlateTone("chartreuse")).toBe(false);
    expect(isPlateTone(2)).toBe(false);
    expect(isPlateTone(undefined)).toBe(false);
  });
});

describe("programSourcesEqual", () => {
  it("matches identical simple sources", () => {
    expect(programSourcesEqual({ kind: "gallery" }, { kind: "gallery" })).toBe(true);
    expect(programSourcesEqual({ kind: "black" }, { kind: "black" })).toBe(true);
  });

  it("distinguishes different kinds", () => {
    expect(programSourcesEqual({ kind: "gallery" }, { kind: "black" })).toBe(false);
  });

  it("compares slot numbers", () => {
    expect(programSourcesEqual({ kind: "slot", slot: 3 }, { kind: "slot", slot: 3 })).toBe(true);
    expect(programSourcesEqual({ kind: "slot", slot: 3 }, { kind: "slot", slot: 4 })).toBe(false);
  });

  it("compares look ids", () => {
    expect(
      programSourcesEqual({ kind: "look", lookId: "banter" }, { kind: "look", lookId: "banter" })
    ).toBe(true);
    expect(
      programSourcesEqual({ kind: "look", lookId: "banter" }, { kind: "look", lookId: "teatime" })
    ).toBe(false);
  });
});
```

Update that file's import to include `PLATE_TONES`, `isPlateTone`, and `programSourcesEqual`.

Append to `show-engine/src/config.test.ts`:

```ts
const look = {
  id: "hr",
  label: "Host + Reader",
  scenePreset: "scene-hr",
  boxes: 2,
  includesHost: true,
  includesReader: true,
  plateTone: "accent"
};

describe("parseShowEngineConfig direction fields", () => {
  it("defaults skipRoles to the ASL interpreter and looks to empty", () => {
    const config = parseShowEngineConfig(minimal);
    expect(config.skipRoles).toEqual(["aslinterpreter"]);
    expect(config.looks).toEqual([]);
  });

  it("keeps explicitly provided skipRoles", () => {
    const config = parseShowEngineConfig({ ...minimal, skipRoles: ["aslinterpreter", "reader"] });
    expect(config.skipRoles).toEqual(["aslinterpreter", "reader"]);
  });

  it("rejects an unknown role rather than coercing it", () => {
    expect(() => parseShowEngineConfig({ ...minimal, skipRoles: ["moderator"] })).toThrow(
      /skipRoles/
    );
  });

  it("accepts a well-formed look", () => {
    const config = parseShowEngineConfig({ ...minimal, looks: [look] });
    expect(config.looks).toEqual([look]);
  });

  it("rejects a look with an out-of-range box count", () => {
    expect(() => parseShowEngineConfig({ ...minimal, looks: [{ ...look, boxes: 5 }] })).toThrow(
      /boxes/
    );
  });

  it("rejects a look with an unknown plateTone", () => {
    expect(() =>
      parseShowEngineConfig({ ...minimal, looks: [{ ...look, plateTone: "chartreuse" }] })
    ).toThrow(/plateTone/);
  });

  it("defaults an omitted plateTone to neutral", () => {
    const { plateTone: _omitted, ...withoutTone } = look;
    const config = parseShowEngineConfig({ ...minimal, looks: [withoutTone] });
    expect(config.looks[0]?.plateTone).toBe("neutral");
  });

  it("rejects duplicate look ids", () => {
    expect(() => parseShowEngineConfig({ ...minimal, looks: [look, { ...look, label: "Other" }] })
    ).toThrow(/duplicate/i);
  });

  it("rejects a non-array looks value", () => {
    expect(() => parseShowEngineConfig({ ...minimal, looks: {} })).toThrow(/looks/);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `npm run test --workspace show-engine`
Expected: FAIL — `PLATE_TONES` is not exported; `config.skipRoles` is undefined.

- [ ] **Step 3: Implement**

Extend `contracts.ts` and `config.ts` per the Interfaces and Behavior above. Keep the existing validation-helper style in `config.ts` (`requireString`, `requirePositiveInt`, `optionalPositiveInt`, and the `label` argument used for error paths) rather than introducing a second idiom.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS, no type errors. Plan 1's existing tests must all still pass.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/contracts.ts show-engine/src/contracts.test.ts show-engine/src/config.ts show-engine/src/config.test.ts
git commit -m "feat(show-engine): direction contracts and config"
```

---

### Task 2: ZoomIngest revision counter

**Files:**
- Modify: `show-engine/src/zoomIngest.ts`
- Test: `show-engine/src/zoomIngest.test.ts` (append)

**Interfaces:**
- Consumes: the existing `ZoomIngest` class.
- Produces: `ZoomIngest` gains `get revision(): number`.

**Behavior:**

This closes a Plan 1 deferred finding. `snapshot()` deep-copies on every call, which defeats referential-equality memoization downstream — and Plan 3's orchestrator will poll `snapshot()` on a loop. **Do not remove the copy**; that would make `snapshot()` the only accessor in the package that aliases. Add a monotonic counter instead, so callers memoize on an integer.

`revision` starts at `0` and increments by exactly one each time `commit()` actually publishes a new snapshot — that is, each time `commit()` returns `true`. A `commit()` that returns `false` leaves it unchanged. Nothing else moves it; it never decreases and never resets.

- [ ] **Step 1: Write the failing tests**

Append to `show-engine/src/zoomIngest.test.ts`:

```ts
describe("ZoomIngest revision", () => {
  it("starts at zero", () => {
    expect(new ZoomIngest().revision).toBe(0);
  });

  it("increments once per publishing commit", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    ingest.commit();
    expect(ingest.revision).toBe(1);

    ingest.apply({ kind: "video", participantId: "a", on: true });
    ingest.commit();
    expect(ingest.revision).toBe(2);
  });

  it("does not move when a commit publishes nothing", () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant({ participantId: "a" }) });
    ingest.commit();
    const before = ingest.revision;
    expect(ingest.commit()).toBe(false);
    expect(ingest.revision).toBe(before);
  });

  it("does not move for an event that changes nothing", () => {
    const ingest = new ZoomIngest();
    ingest.apply({
      kind: "joined",
      participant: participant({ participantId: "a", videoOn: true })
    });
    ingest.commit();
    const before = ingest.revision;
    ingest.apply({ kind: "video", participantId: "a", on: true });
    ingest.commit();
    expect(ingest.revision).toBe(before);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `npm run test --workspace show-engine`
Expected: FAIL — `revision` is undefined.

- [ ] **Step 3: Implement**

Add the counter per the Behavior above. Do not change `snapshot()`, `commit()`'s return contract, or the copy discipline.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/zoomIngest.ts show-engine/src/zoomIngest.test.ts
git commit -m "feat(show-engine): monotonic ingest revision for downstream memoization"
```

---

### Task 3: Per-endpoint Mukana health and the hands/question fetches

**Files:**
- Modify: `show-engine/src/mukanaClient.ts`
- Test: `show-engine/src/mukanaClient.test.ts` (append and adjust)

**Interfaces:**
- Consumes: `MukanaConfig` from `./config.js`; `parseMukanaPanelists`, `MukanaOutcome` from `./mukanaParse.js`.
- Produces:
  - `type MukanaEndpoint = "panelists" | "hands" | "question"`
  - `MukanaClient` gains `fetchHands(): Promise<MukanaOutcome>` and `fetchQuestion(): Promise<MukanaOutcome>`
  - `MukanaClient.health` changes shape to `Record<MukanaEndpoint, MukanaHealth>`
  - `MukanaClient.nextDelayMs(endpoint: MukanaEndpoint): number`
  - `healthFor(endpoint: MukanaEndpoint): MukanaHealth` as a convenience accessor
- `MukanaHealth` itself is unchanged: `{ state: "ok" | "dormant" | "failing"; consecutiveFailures: number; detail: string | null }`.

**Behavior:**

This closes a Plan 1 deferred finding: one health record and a hardcoded `panelistsIntervalMs` cannot serve three endpoints at three intervals.

Each endpoint keeps its own independent health record and failure counter. A failure on `hands` must not affect `panelists`' backoff or health, and vice versa. All three start at `{ state: "ok", consecutiveFailures: 0, detail: null }`.

`nextDelayMs(endpoint)` uses that endpoint's configured interval — `panelistsIntervalMs`, `handsIntervalMs`, `questionIntervalMs` — as both the base delay and the backoff multiplicand, clamped to the shared `maxBackoffMs`. The backoff curve is unchanged: `interval * 2 ** consecutiveFailures`, capped.

`fetchHands` and `fetchQuestion` build their URLs exactly as `fetchPanelists` does, varying only the `req` value: `?event=<encoded>&req=hands` and `?event=<encoded>&req=question`.

All three endpoints share the response handling already in place: a thrown error or non-2xx becomes `invalid` and counts as a failure; a `status`-enveloped body becomes `dormant` and does **not** count as a failure; success resets to `ok`. The response body of `hands` and `question` is parsed with the same `parseMukanaPanelists` shape-gate for now — Plan 3 introduces payload-specific parsing, and this task must not anticipate it.

`health` returns copies; a caller mutating the returned record cannot reach internal state.

**Migration note:** Plan 1's tests read `client.health` as a single record. Update those existing assertions to `client.healthFor("panelists")` — this is a deliberate breaking change to a pre-release internal API, not a compatibility shim. Do not keep a single-record alias.

- [ ] **Step 1: Write the failing tests**

Adjust the existing `mukanaClient.test.ts` assertions from `client.health` to `client.healthFor("panelists")`, and from `client.nextDelayMs()` to `client.nextDelayMs("panelists")`. Then append:

```ts
describe("MukanaClient per-endpoint behaviour", () => {
  it("builds the hands and question URLs", async () => {
    const urls: string[] = [];
    const client = new MukanaClient(config, {
      fetch: async (url) => {
        urls.push(url);
        return { ok: true, status: 200, text: async () => panelistsBody };
      }
    });
    await client.fetchHands();
    await client.fetchQuestion();
    expect(urls).toEqual([
      "https://hoka.example.com/php-panel-rest.php?event=officehours&req=hands",
      "https://hoka.example.com/php-panel-rest.php?event=officehours&req=question"
    ]);
  });

  it("starts every endpoint healthy", () => {
    const client = new MukanaClient(config, { fetch: respondWith(panelistsBody) });
    for (const endpoint of ["panelists", "hands", "question"] as const) {
      expect(client.healthFor(endpoint)).toEqual({
        state: "ok",
        consecutiveFailures: 0,
        detail: null
      });
    }
  });

  it("uses each endpoint's own interval for the base delay", () => {
    const client = new MukanaClient(config, { fetch: respondWith(panelistsBody) });
    expect(client.nextDelayMs("panelists")).toBe(5000);
    expect(client.nextDelayMs("hands")).toBe(2000);
    expect(client.nextDelayMs("question")).toBe(2000);
  });

  it("keeps failure state independent per endpoint", async () => {
    let failHands = true;
    const client = new MukanaClient(config, {
      fetch: async (url) => {
        const broken = url.includes("req=hands") && failHands;
        return {
          ok: !broken,
          status: broken ? 503 : 200,
          text: async () => (broken ? "nope" : panelistsBody)
        };
      }
    });

    await client.fetchHands();
    await client.fetchPanelists();

    expect(client.healthFor("hands").state).toBe("failing");
    expect(client.healthFor("hands").consecutiveFailures).toBe(1);
    expect(client.nextDelayMs("hands")).toBe(4000);

    expect(client.healthFor("panelists").state).toBe("ok");
    expect(client.nextDelayMs("panelists")).toBe(5000);

    failHands = false;
    await client.fetchHands();
    expect(client.healthFor("hands").state).toBe("ok");
    expect(client.nextDelayMs("hands")).toBe(2000);
  });

  it("caps each endpoint's backoff at maxBackoffMs", async () => {
    const client = new MukanaClient(config, { fetch: respondWith("nope", false, 503) });
    for (let i = 0; i < 6; i += 1) await client.fetchHands();
    expect(client.nextDelayMs("hands")).toBe(60000);
  });

  it("treats a dormant hands response as healthy-but-dormant", async () => {
    const client = new MukanaClient(config, {
      fetch: respondWith(JSON.stringify({ status: 200, detail: "outside show hours" }))
    });
    const outcome = await client.fetchHands();
    expect(outcome.kind).toBe("dormant");
    expect(client.healthFor("hands")).toEqual({
      state: "dormant",
      consecutiveFailures: 0,
      detail: "outside show hours"
    });
    expect(client.nextDelayMs("hands")).toBe(2000);
  });

  it("returns copies of health records", () => {
    const client = new MukanaClient(config, { fetch: respondWith(panelistsBody) });
    const record = client.healthFor("panelists");
    record.consecutiveFailures = 99;
    expect(client.healthFor("panelists").consecutiveFailures).toBe(0);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `npm run test --workspace show-engine`
Expected: FAIL — `healthFor` is not a function; `nextDelayMs` takes no argument.

- [ ] **Step 3: Implement**

Extend `MukanaClient` per the Interfaces and Behavior. The existing private request path already handles outcome classification — generalize it over the endpoint rather than duplicating it three times.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS. `pipeline.test.ts` also reads client health — update it if the typecheck flags it.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/mukanaClient.ts show-engine/src/mukanaClient.test.ts show-engine/src/pipeline.test.ts
git commit -m "feat(show-engine): per-endpoint Mukana health, hands and question fetches"
```

---

### Task 4: Hands queue

**Files:**
- Create: `show-engine/src/handsQueue.ts`
- Test: `show-engine/src/handsQueue.test.ts`

**Interfaces:**
- Consumes: `QueueState` from `./contracts.js`.
- Produces:
  - `type HandsOutcome = { kind: "data"; queue: QueueState } | { kind: "invalid"; reason: string }`
  - `function parseHandsPayload(body: string): HandsOutcome`
  - `function stripChairs(queue: QueueState, chairs: { hostPin: string | null; readerPin: string | null }): QueueState`
  - `function queueOrder(queue: QueueState): string[]`

**Behavior:**

The hands endpoint reports who is waiting to ask a question. The legacy payload is three lines — upcoming PINs (comma-separated), the current PIN, then previously-shown PINs (comma-separated) — with the sentinel `NONE` standing in for an empty list or absent current speaker. `parseHandsPayload` accepts that legacy three-line text form.

Parsing rules: split on newlines; a body with fewer than three lines is `invalid`. Each list splits on commas; entries are trimmed; empty entries and `NONE` are dropped. A `current` of `NONE` or empty becomes `null`. Entries that are not exactly four digits are dropped — a malformed PIN cannot join to anyone, and keeping it would put a phantom in the queue. Duplicate PINs are removed, keeping the first occurrence, scanning `current` then `upcoming` then `previous`.

`stripChairs` removes the host and reader PINs from every list. Those two have dedicated boxes in every look that includes them, so leaving them in the guest queue would double-book them. A `null` chair PIN removes nothing. If the *current* PIN is a chair, `current` becomes `null` — it does not promote from `upcoming`; promotion is the operator's paging decision, not an automatic one.

`queueOrder` returns the display order: `current` first when present, then `upcoming` in order. `previous` is excluded — it exists to record who has already been shown, which Plan 2's look paging uses as an offset. The returned array is a fresh array on every call.

- [ ] **Step 1: Write the failing tests**

`show-engine/src/handsQueue.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { parseHandsPayload, queueOrder, stripChairs } from "./handsQueue.js";
import type { QueueState } from "./contracts.js";

describe("parseHandsPayload", () => {
  it("parses upcoming, current and previous", () => {
    const outcome = parseHandsPayload("4242,5555\n1383\n9999,8888");
    expect(outcome).toEqual({
      kind: "data",
      queue: { previous: ["9999", "8888"], current: "1383", upcoming: ["4242", "5555"] }
    });
  });

  it("treats NONE as an empty list", () => {
    const outcome = parseHandsPayload("NONE\n1383\nNONE");
    expect(outcome).toEqual({
      kind: "data",
      queue: { previous: [], current: "1383", upcoming: [] }
    });
  });

  it("treats a NONE current as null", () => {
    const outcome = parseHandsPayload("4242\nNONE\nNONE");
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.queue.current).toBeNull();
  });

  it("drops entries that are not four digits", () => {
    const outcome = parseHandsPayload("4242,abcd,12345,777\nNONE\nNONE");
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.queue.upcoming).toEqual(["4242"]);
  });

  it("removes duplicates, keeping the first occurrence", () => {
    const outcome = parseHandsPayload("1383,4242\n1383\n4242");
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.queue).toEqual({ previous: [], current: "1383", upcoming: ["4242"] });
  });

  it("trims whitespace around entries", () => {
    const outcome = parseHandsPayload(" 4242 , 5555 \n 1383 \nNONE");
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.queue.upcoming).toEqual(["4242", "5555"]);
    expect(outcome.queue.current).toBe("1383");
  });

  it("rejects a body with fewer than three lines", () => {
    expect(parseHandsPayload("4242\n1383").kind).toBe("invalid");
    expect(parseHandsPayload("").kind).toBe("invalid");
  });
});

describe("stripChairs", () => {
  const queue: QueueState = {
    previous: ["9999", "1383"],
    current: "4242",
    upcoming: ["1383", "5555", "7777"]
  };

  it("removes the host and reader from every list", () => {
    expect(stripChairs(queue, { hostPin: "1383", readerPin: "5555" })).toEqual({
      previous: ["9999"],
      current: "4242",
      upcoming: ["7777"]
    });
  });

  it("nulls the current entry when it is a chair", () => {
    expect(stripChairs(queue, { hostPin: "4242", readerPin: null })).toEqual({
      previous: ["9999", "1383"],
      current: null,
      upcoming: ["1383", "5555", "7777"]
    });
  });

  it("does not promote from upcoming when the current entry is removed", () => {
    const result = stripChairs(queue, { hostPin: "4242", readerPin: null });
    expect(result.current).toBeNull();
    expect(result.upcoming[0]).toBe("1383");
  });

  it("removes nothing when both chairs are null", () => {
    expect(stripChairs(queue, { hostPin: null, readerPin: null })).toEqual(queue);
  });

  it("does not mutate its input", () => {
    const input: QueueState = { previous: ["1383"], current: "1383", upcoming: ["1383"] };
    stripChairs(input, { hostPin: "1383", readerPin: null });
    expect(input).toEqual({ previous: ["1383"], current: "1383", upcoming: ["1383"] });
  });
});

describe("queueOrder", () => {
  it("puts the current entry first, then upcoming", () => {
    expect(
      queueOrder({ previous: ["9999"], current: "1383", upcoming: ["4242", "5555"] })
    ).toEqual(["1383", "4242", "5555"]);
  });

  it("omits a null current", () => {
    expect(queueOrder({ previous: [], current: null, upcoming: ["4242"] })).toEqual(["4242"]);
  });

  it("excludes previously shown entries", () => {
    expect(queueOrder({ previous: ["9999"], current: null, upcoming: [] })).toEqual([]);
  });

  it("returns a fresh array each call", () => {
    const queue: QueueState = { previous: [], current: "1383", upcoming: [] };
    const first = queueOrder(queue);
    first.push("nope");
    expect(queueOrder(queue)).toEqual(["1383"]);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./handsQueue.js`.

- [ ] **Step 3: Implement**

Write `handsQueue.ts` per the Interfaces and Behavior.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/handsQueue.ts show-engine/src/handsQueue.test.ts
git commit -m "feat(show-engine): hands queue parsing and chair stripping"
```

---

### Task 5: Speaker recency — FILO assigner

**Files:**
- Create: `show-engine/src/speakerRecency.ts`
- Test: `show-engine/src/speakerRecency.test.ts`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `type PlacementChange = { position: number; participantId: string | null }`
  - `interface PositionAssigner { onActiveSpeaker(participantId: string): PlacementChange[]; positions(): Map<number, string>; reset(participantIds: readonly string[]): void }`
  - `class FiloAssigner implements PositionAssigner` with `constructor(options: { capacity: number })`

Task 6 adds `VisibleSetAssigner` and `RecencyScores` to this same file.

**Behavior:**

A fixed pool of `capacity` positions numbered `1..capacity`. Positions are filled in ascending order while the pool has room; once full, the *least recently active* occupant is evicted and the newcomer takes their position. This is the classic first-in-last-out router: it keeps whoever spoke most recently, and it never moves someone who is already placed.

`onActiveSpeaker(participantId)`:
- If the participant already holds a position: refresh their recency so they become the most recent, and return an **empty** array. Nothing on screen changes, so nothing downstream should be told to change.
- If the pool has a free position: assign the lowest free position and return one change for it.
- If the pool is full: evict the least recently active occupant and give the newcomer that exact position. Return one change — the position with the new participant. The eviction is implied by the reassignment; do not emit a separate `null` change for it.

`positions()` returns a fresh `Map` of position → participantId, containing only occupied positions.

`reset(participantIds)` clears everything and seats the given participants in order from position 1, stopping at `capacity`. Recency follows the given order, with the last entry treated as most recent. Passing an empty array empties the pool.

A `capacity` below 1 throws. Position numbers are 1-based throughout — this package never emits 0-based indices; that translation belongs to whichever adapter talks to hardware.

- [ ] **Step 1: Write the failing tests**

`show-engine/src/speakerRecency.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { FiloAssigner } from "./speakerRecency.js";

describe("FiloAssigner", () => {
  it("rejects a capacity below one", () => {
    expect(() => new FiloAssigner({ capacity: 0 })).toThrow(/capacity/);
  });

  it("starts empty", () => {
    expect(new FiloAssigner({ capacity: 3 }).positions().size).toBe(0);
  });

  it("fills free positions in ascending order", () => {
    const filo = new FiloAssigner({ capacity: 3 });
    expect(filo.onActiveSpeaker("a")).toEqual([{ position: 1, participantId: "a" }]);
    expect(filo.onActiveSpeaker("b")).toEqual([{ position: 2, participantId: "b" }]);
    expect([...filo.positions().entries()]).toEqual([
      [1, "a"],
      [2, "b"]
    ]);
  });

  it("returns no changes when an already-placed speaker speaks again", () => {
    const filo = new FiloAssigner({ capacity: 3 });
    filo.onActiveSpeaker("a");
    expect(filo.onActiveSpeaker("a")).toEqual([]);
    expect(filo.positions().get(1)).toBe("a");
  });

  it("evicts the least recently active occupant when full", () => {
    const filo = new FiloAssigner({ capacity: 2 });
    filo.onActiveSpeaker("a");
    filo.onActiveSpeaker("b");
    expect(filo.onActiveSpeaker("c")).toEqual([{ position: 1, participantId: "c" }]);
    expect([...filo.positions().entries()]).toEqual([
      [1, "c"],
      [2, "b"]
    ]);
  });

  it("protects a speaker who refreshed their recency from the next eviction", () => {
    const filo = new FiloAssigner({ capacity: 2 });
    filo.onActiveSpeaker("a");
    filo.onActiveSpeaker("b");
    filo.onActiveSpeaker("a");
    expect(filo.onActiveSpeaker("c")).toEqual([{ position: 2, participantId: "c" }]);
    expect([...filo.positions().entries()]).toEqual([
      [1, "a"],
      [2, "c"]
    ]);
  });

  it("never evicts the current speaker", () => {
    const filo = new FiloAssigner({ capacity: 1 });
    filo.onActiveSpeaker("a");
    filo.onActiveSpeaker("b");
    expect(filo.positions().get(1)).toBe("b");
    expect(filo.onActiveSpeaker("b")).toEqual([]);
    expect(filo.positions().get(1)).toBe("b");
  });

  it("seats a roster in order on reset, treating the last as most recent", () => {
    const filo = new FiloAssigner({ capacity: 3 });
    filo.reset(["a", "b", "c"]);
    expect([...filo.positions().entries()]).toEqual([
      [1, "a"],
      [2, "b"],
      [3, "c"]
    ]);
    expect(filo.onActiveSpeaker("d")).toEqual([{ position: 1, participantId: "d" }]);
  });

  it("drops entries past capacity on reset", () => {
    const filo = new FiloAssigner({ capacity: 2 });
    filo.reset(["a", "b", "c"]);
    expect([...filo.positions().entries()]).toEqual([
      [1, "a"],
      [2, "b"]
    ]);
  });

  it("empties the pool on an empty reset", () => {
    const filo = new FiloAssigner({ capacity: 2 });
    filo.onActiveSpeaker("a");
    filo.reset([]);
    expect(filo.positions().size).toBe(0);
  });

  it("returns a fresh map so callers cannot mutate internal state", () => {
    const filo = new FiloAssigner({ capacity: 2 });
    filo.onActiveSpeaker("a");
    filo.positions().set(1, "hacked");
    expect(filo.positions().get(1)).toBe("a");
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./speakerRecency.js`.

- [ ] **Step 3: Implement**

Write `speakerRecency.ts` with `PlacementChange`, `PositionAssigner`, and `FiloAssigner` per the Interfaces and Behavior.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/speakerRecency.ts show-engine/src/speakerRecency.test.ts
git commit -m "feat(show-engine): FILO speaker position assigner"
```

---

### Task 6: Speaker recency — visible-set assigner and recency scores

**Files:**
- Modify: `show-engine/src/speakerRecency.ts`
- Test: `show-engine/src/speakerRecency.test.ts` (append)

**Interfaces:**
- Consumes: `PlacementChange`, `PositionAssigner` from Task 5.
- Produces:
  - `class VisibleSetAssigner implements PositionAssigner` with `constructor(options: { capacity: number; visible: number })`
  - `class RecencyScores` with `onActiveSpeaker(participantId: string): void`, `order(participantIds: readonly string[]): string[]`, `reset(): void`

**Behavior:**

**`VisibleSetAssigner`** models a larger roster than the display can show: `capacity` positions exist, but only positions `1..visible` are on screen. Its job is to keep the most recently active people in the visible window by *swapping*, never by evicting — everyone keeps a position, they just move.

- Constructor throws when `capacity < 1`, when `visible < 1`, or when `visible > capacity`.
- `onActiveSpeaker(participantId)`:
  - Unknown participant and a free position exists → seat them at the lowest free position; if that position is visible, return one change; if it is not visible, immediately apply the swap rule below and return both changes.
  - Unknown participant and no free position → the least recently active occupant of a **visible** position is displaced to the newcomer's would-be slot. Concretely: seat the newcomer at the stalest visible position and move its previous occupant to the position the newcomer would otherwise have taken. Since the pool is full there is no such position, so in that case the two simply exchange places and the displaced participant leaves the visible window entirely — return both changes.
  - Known participant already visible → refresh recency, return an empty array.
  - Known participant not visible → swap them with the least recently active visible occupant and return two changes, one per affected position.
- Recency ties are broken by lower position number, so results are deterministic.
- `positions()` and `reset()` behave exactly as `FiloAssigner`'s, including returning a fresh `Map`.

**`RecencyScores`** is the pure-ordering strategy used for gallery arrangement, where nothing is evicted and nothing swaps — the whole set is simply re-sorted. It reproduces the legacy "speaking score" bucket sort without its fixed-size array.

- `onActiveSpeaker(participantId)` marks that participant as the most recent. Participants never seen are unranked.
- `order(participantIds)` returns the given participants sorted most-recently-active first. Participants who have never spoken sort last, preserving their relative order from the input — a stable sort, so an unranked roster comes back in exactly the order it was given.
- `order` never invents or drops participants: the result is a permutation of the input.
- `reset()` clears all history; every participant becomes unranked.

- [ ] **Step 1: Write the failing tests**

Append to `show-engine/src/speakerRecency.test.ts` (extend the import to include `RecencyScores` and `VisibleSetAssigner`):

```ts
describe("VisibleSetAssigner", () => {
  it("rejects invalid geometry", () => {
    expect(() => new VisibleSetAssigner({ capacity: 4, visible: 0 })).toThrow(/visible/);
    expect(() => new VisibleSetAssigner({ capacity: 4, visible: 5 })).toThrow(/visible/);
    expect(() => new VisibleSetAssigner({ capacity: 0, visible: 1 })).toThrow(/capacity/);
  });

  it("seats newcomers into visible positions first", () => {
    const set = new VisibleSetAssigner({ capacity: 4, visible: 2 });
    expect(set.onActiveSpeaker("a")).toEqual([{ position: 1, participantId: "a" }]);
    expect(set.onActiveSpeaker("b")).toEqual([{ position: 2, participantId: "b" }]);
  });

  it("returns no changes when a visible speaker speaks again", () => {
    const set = new VisibleSetAssigner({ capacity: 4, visible: 2 });
    set.onActiveSpeaker("a");
    expect(set.onActiveSpeaker("a")).toEqual([]);
  });

  it("swaps an off-screen speaker into the stalest visible position", () => {
    const set = new VisibleSetAssigner({ capacity: 4, visible: 2 });
    set.reset(["a", "b", "c", "d"]);
    set.onActiveSpeaker("b");

    const changes = set.onActiveSpeaker("d");
    expect(changes).toHaveLength(2);
    expect(set.positions().get(1)).toBe("d");
    expect(set.positions().get(4)).toBe("a");
    expect(set.positions().get(2)).toBe("b");
  });

  it("keeps everyone seated — a swap never drops a participant", () => {
    const set = new VisibleSetAssigner({ capacity: 3, visible: 1 });
    set.reset(["a", "b", "c"]);
    set.onActiveSpeaker("c");
    expect([...set.positions().values()].sort()).toEqual(["a", "b", "c"]);
  });

  it("breaks recency ties by lower position number", () => {
    const set = new VisibleSetAssigner({ capacity: 4, visible: 2 });
    set.reset(["a", "b", "c", "d"]);
    set.onActiveSpeaker("d");
    expect(set.positions().get(1)).toBe("d");
  });

  it("returns a fresh map so callers cannot mutate internal state", () => {
    const set = new VisibleSetAssigner({ capacity: 2, visible: 1 });
    set.onActiveSpeaker("a");
    set.positions().set(1, "hacked");
    expect(set.positions().get(1)).toBe("a");
  });
});

describe("RecencyScores", () => {
  it("returns an unranked roster in its given order", () => {
    const scores = new RecencyScores();
    expect(scores.order(["a", "b", "c"])).toEqual(["a", "b", "c"]);
  });

  it("puts the most recent speaker first", () => {
    const scores = new RecencyScores();
    scores.onActiveSpeaker("c");
    expect(scores.order(["a", "b", "c"])).toEqual(["c", "a", "b"]);
  });

  it("orders several speakers most-recent-first", () => {
    const scores = new RecencyScores();
    scores.onActiveSpeaker("a");
    scores.onActiveSpeaker("b");
    scores.onActiveSpeaker("c");
    expect(scores.order(["a", "b", "c"])).toEqual(["c", "b", "a"]);
  });

  it("re-promotes a repeat speaker", () => {
    const scores = new RecencyScores();
    scores.onActiveSpeaker("a");
    scores.onActiveSpeaker("b");
    scores.onActiveSpeaker("a");
    expect(scores.order(["a", "b"])).toEqual(["a", "b"]);
  });

  it("sorts never-spoken participants last, stably", () => {
    const scores = new RecencyScores();
    scores.onActiveSpeaker("c");
    expect(scores.order(["a", "b", "c", "d"])).toEqual(["c", "a", "b", "d"]);
  });

  it("returns a permutation of the input, ignoring unknown history", () => {
    const scores = new RecencyScores();
    scores.onActiveSpeaker("z");
    expect(scores.order(["a", "b"]).sort()).toEqual(["a", "b"]);
  });

  it("clears history on reset", () => {
    const scores = new RecencyScores();
    scores.onActiveSpeaker("c");
    scores.reset();
    expect(scores.order(["a", "b", "c"])).toEqual(["a", "b", "c"]);
  });

  it("returns a fresh array each call", () => {
    const scores = new RecencyScores();
    const first = scores.order(["a", "b"]);
    first.push("nope");
    expect(scores.order(["a", "b"])).toEqual(["a", "b"]);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `npm run test --workspace show-engine`
Expected: FAIL — `VisibleSetAssigner` and `RecencyScores` are not exported.

- [ ] **Step 3: Implement**

Extend `speakerRecency.ts` per the Interfaces and Behavior. `FiloAssigner`'s tests from Task 5 must keep passing unchanged.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS, no type errors.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/speakerRecency.ts show-engine/src/speakerRecency.test.ts
git commit -m "feat(show-engine): visible-set assigner and recency scores"
```

---

### Task 7: Gallery director

**Files:**
- Create: `show-engine/src/galleryDirector.ts`
- Test: `show-engine/src/galleryDirector.test.ts`

**Interfaces:**
- Consumes: `GalleryCell` from `./contracts.js`; `Slot` from `./contracts.js`.
- Produces:
  - `type GalleryState = { version: 1; cells: number; assignments: GalleryCell[] }`
  - `class GalleryError extends Error`
  - `class GalleryDirector` with:
    - `constructor(options: { cells: number })`
    - `cells(): GalleryCell[]`
    - `smartCells(): GalleryCell[]`
    - `empty(): void`
    - `resetFromSlots(slots: readonly Slot[]): void`
    - `replace(cell: number, slot: number): void`
    - `remove(cell: number): void`
    - `applyOrder(slotOrder: readonly number[]): void`
    - `occupiedCount(): number`
    - `toJSON(): GalleryState`
    - `static fromJSON(state: GalleryState, options: { cells: number }): GalleryDirector`

**Behavior:**

The gallery is a fixed grid of `cells` positions, numbered `1..cells`, each showing one roster slot. `GalleryCell.slot` is the roster slot number on display there; `0` means the cell is blank.

- A new director starts with every cell blank.
- `empty()` blanks every cell.
- `resetFromSlots(slots)` packs the occupied roster slots into the gallery in ascending cell order — the first occupied slot goes to cell 1, the second to cell 2, and so on. Slots whose `panelist` is `null` are skipped. Packing stops when cells run out; the remaining roster slots simply do not appear. Any cells left over are blanked.
- `replace(cell, slot)` puts a roster slot on a specific cell, replacing whatever was there. The same roster slot may legitimately appear on two cells — the operator may want someone duplicated — so this does not deduplicate.
- `remove(cell)` blanks that one cell and leaves every other cell exactly where it is. The gallery never compacts, for the same reason the roster never does: the arrangement is an editorial choice.
- `applyOrder(slotOrder)` rearranges the gallery to the given roster-slot order, filling cells from 1 upward and blanking any cells past the end of the order. This is how a recency strategy drives the gallery. Slot `0` entries in the order are skipped rather than written as blanks.
- `occupiedCount()` counts cells whose slot is not `0`.

**Smart gallery.** `smartCells()` returns a parallel view used by hosts that render a compacted grid: it is `cells()` with blanks removed and the remaining assignments renumbered to consecutive cells from 1. The main `cells()` view keeps its holes. Both views derive from the same state; `smartCells()` computes on demand and stores nothing.

**Validation.** A `cells` count below 1 throws. A cell number outside `1..cells` throws. A negative slot number throws; slot `0` is the legitimate blank value. `fromJSON` throws a `GalleryError` when the persisted `cells` count disagrees with the configured one, when `assignments.length` does not equal `cells`, when an entry's `cell` field does not match its position, or when `version` is not `1`. This mirrors `LiveSlots.fromJSON`'s contract from Plan 1 — a restore failure must be catchable and downgradable to a clean start, never a crash before air.

All accessors return fresh structures.

- [ ] **Step 1: Write the failing tests**

`show-engine/src/galleryDirector.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { GalleryDirector, GalleryError } from "./galleryDirector.js";
import type { Panelist, Slot } from "./contracts.js";

function panelist(participantId: string): Panelist {
  return {
    participantId,
    rawName: participantId,
    online: true,
    videoOn: true,
    audioOn: false,
    handRaised: false,
    zoomRole: 3,
    displayName: participantId,
    location: "",
    pin: null,
    hasMukana: false,
    role: "panelist"
  };
}

function slots(occupied: (string | null)[]): Slot[] {
  return occupied.map((id, index) => ({
    slot: index + 1,
    panelist: id === null ? null : panelist(id)
  }));
}

function slotNumbers(gallery: GalleryDirector): number[] {
  return gallery.cells().map((cell) => cell.slot);
}

describe("GalleryDirector", () => {
  it("rejects a cell count below one", () => {
    expect(() => new GalleryDirector({ cells: 0 })).toThrow(/cells/);
  });

  it("starts with every cell blank", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    expect(gallery.cells()).toEqual([
      { cell: 1, slot: 0 },
      { cell: 2, slot: 0 },
      { cell: 3, slot: 0 },
      { cell: 4, slot: 0 }
    ]);
    expect(gallery.occupiedCount()).toBe(0);
  });

  it("packs occupied roster slots in ascending cell order", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    gallery.resetFromSlots(slots(["a", null, "c", "d"]));
    expect(slotNumbers(gallery)).toEqual([1, 3, 4, 0]);
    expect(gallery.occupiedCount()).toBe(3);
  });

  it("stops packing when cells run out", () => {
    const gallery = new GalleryDirector({ cells: 2 });
    gallery.resetFromSlots(slots(["a", "b", "c"]));
    expect(slotNumbers(gallery)).toEqual([1, 2]);
  });

  it("blanks leftover cells on reset", () => {
    const gallery = new GalleryDirector({ cells: 3 });
    gallery.replace(3, 9);
    gallery.resetFromSlots(slots(["a"]));
    expect(slotNumbers(gallery)).toEqual([1, 0, 0]);
  });

  it("replaces a single cell", () => {
    const gallery = new GalleryDirector({ cells: 3 });
    gallery.replace(2, 7);
    expect(slotNumbers(gallery)).toEqual([0, 7, 0]);
  });

  it("allows the same roster slot on two cells", () => {
    const gallery = new GalleryDirector({ cells: 3 });
    gallery.replace(1, 5);
    gallery.replace(2, 5);
    expect(slotNumbers(gallery)).toEqual([5, 5, 0]);
  });

  it("blanks one cell on remove without compacting", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    gallery.resetFromSlots(slots(["a", "b", "c"]));
    gallery.remove(2);
    expect(slotNumbers(gallery)).toEqual([1, 0, 3, 0]);
  });

  it("blanks everything on empty", () => {
    const gallery = new GalleryDirector({ cells: 3 });
    gallery.resetFromSlots(slots(["a", "b"]));
    gallery.empty();
    expect(slotNumbers(gallery)).toEqual([0, 0, 0]);
  });

  it("rearranges to a given slot order", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    gallery.resetFromSlots(slots(["a", "b", "c"]));
    gallery.applyOrder([3, 1, 2]);
    expect(slotNumbers(gallery)).toEqual([3, 1, 2, 0]);
  });

  it("skips blank entries in an applied order", () => {
    const gallery = new GalleryDirector({ cells: 3 });
    gallery.applyOrder([2, 0, 5]);
    expect(slotNumbers(gallery)).toEqual([2, 5, 0]);
  });

  it("blanks cells past the end of an applied order", () => {
    const gallery = new GalleryDirector({ cells: 3 });
    gallery.resetFromSlots(slots(["a", "b", "c"]));
    gallery.applyOrder([2]);
    expect(slotNumbers(gallery)).toEqual([2, 0, 0]);
  });

  it("rejects out-of-range cells and negative slots", () => {
    const gallery = new GalleryDirector({ cells: 2 });
    expect(() => gallery.replace(0, 1)).toThrow(/cell/);
    expect(() => gallery.replace(3, 1)).toThrow(/cell/);
    expect(() => gallery.remove(3)).toThrow(/cell/);
    expect(() => gallery.replace(1, -1)).toThrow(/slot/);
  });

  it("compacts blanks away in the smart view", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    gallery.resetFromSlots(slots(["a", "b", "c"]));
    gallery.remove(2);
    expect(gallery.smartCells()).toEqual([
      { cell: 1, slot: 1 },
      { cell: 2, slot: 3 }
    ]);
    expect(slotNumbers(gallery)).toEqual([1, 0, 3, 0]);
  });

  it("returns an empty smart view when nothing is assigned", () => {
    expect(new GalleryDirector({ cells: 3 }).smartCells()).toEqual([]);
  });

  it("returns copies so callers cannot mutate internal state", () => {
    const gallery = new GalleryDirector({ cells: 2 });
    gallery.replace(1, 4);
    const view = gallery.cells();
    view[0] = { cell: 1, slot: 99 };
    expect(gallery.cells()[0]).toEqual({ cell: 1, slot: 4 });
  });

  it("round-trips through JSON", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    gallery.resetFromSlots(slots(["a", "b", "c"]));
    gallery.remove(2);
    const restored = GalleryDirector.fromJSON(gallery.toJSON(), { cells: 4 });
    expect(restored.cells()).toEqual(gallery.cells());
  });

  it("rejects a persisted state that disagrees with the configuration", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    expect(() => GalleryDirector.fromJSON(gallery.toJSON(), { cells: 8 })).toThrow(GalleryError);
  });

  it("rejects a persisted state with a bad assignment list", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    const state = gallery.toJSON();
    expect(() =>
      GalleryDirector.fromJSON({ ...state, assignments: state.assignments.slice(1) }, { cells: 4 })
    ).toThrow(GalleryError);
    expect(() =>
      GalleryDirector.fromJSON(
        { ...state, assignments: [{ cell: 9, slot: 0 }, ...state.assignments.slice(1)] },
        { cells: 4 }
      )
    ).toThrow(GalleryError);
  });

  it("rejects a persisted state of a foreign version", () => {
    const gallery = new GalleryDirector({ cells: 2 });
    const state = { ...gallery.toJSON(), version: 2 } as unknown as ReturnType<
      GalleryDirector["toJSON"]
    >;
    expect(() => GalleryDirector.fromJSON(state, { cells: 2 })).toThrow(GalleryError);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./galleryDirector.js`.

- [ ] **Step 3: Implement**

Write `galleryDirector.ts` per the Interfaces and Behavior.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS, no type errors.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/galleryDirector.ts show-engine/src/galleryDirector.test.ts
git commit -m "feat(show-engine): gallery director with smart-gallery view"
```

---

### Task 8: Look director

**Files:**
- Create: `show-engine/src/lookDirector.ts`
- Test: `show-engine/src/lookDirector.test.ts`

**Interfaces:**
- Consumes: `LookDefinition`, `QueueState`, `Slot`, `PlateTone` from `./contracts.js`; `queueOrder` from `./handsQueue.js`.
- Produces:
  - `type BoxAssignment = { box: number; slot: number | null }`
  - `type NameplatePosition = { kind: "host" } | { kind: "reader" } | { kind: "box"; box: number }`
  - `type Nameplate = { position: NameplatePosition; slot: number; name: string; location: string; tone: PlateTone }`
  - `type LookResolution = { lookId: string; scenePreset: string; plateTone: PlateTone; hostSlot: number | null; readerSlot: number | null; boxes: BoxAssignment[]; nameplates: Nameplate[]; page: number; pageCount: number }`
  - `function findChairSlots(slots: readonly Slot[]): { hostSlot: number | null; readerSlot: number | null }`
  - `function resolveLook(look: LookDefinition, context: { queue: QueueState; slots: readonly Slot[]; page: number }): LookResolution`
  - `function pageCountFor(look: LookDefinition, queue: QueueState): number`

**Behavior:**

A *look* is a named on-screen arrangement — the successor to the legacy SuperSource presets. It declares how many guest boxes it has, whether it carries the host and reader chairs, and which graphics layout goes with it.

`findChairSlots` scans the roster for the slot holding the `host` role and the slot holding the `reader` role, returning `null` for either when nobody holds it. The roster guarantees at most one of each (Plan 1's `LiveSlots` enforces it), so the first match wins; scan in ascending slot order for determinism.

`resolveLook` fills the look's boxes from the hands queue:
- The candidate list is `queueOrder(queue)` — the current asker first, then upcoming.
- `page` selects a window into that list: page `0` shows candidates `0..boxes-1`, page `1` shows `boxes..2*boxes-1`, and so on. This replaces the legacy substring-window trick with explicit paging, and it is what `ohg.look.nextGuest` / `.prevGuest` move.
- Each candidate PIN is resolved to a roster slot by matching `Panelist.pin`. A candidate with no seated match yields a `null` slot for that box rather than shifting the others along — the box is simply empty, which is what the operator sees and can fix.
- Boxes are numbered `1..look.boxes`. A look with `boxes: 0` resolves to an empty box list.
- Boxes past the end of the candidate window are `null`.
- `hostSlot` is populated only when `look.includesHost`; `readerSlot` only when `look.includesReader`. Otherwise they are `null` even if those chairs are seated.

**Nameplates.** `nameplates` carries one entry per *occupied* position, in a stable order: host first (when the look includes it and the chair is seated), then reader, then guest boxes in ascending box number. Each entry names the position, the roster slot behind it, and the `displayName` / `location` read from that slot's panelist. An unoccupied position contributes no entry — a blank box gets no plate rather than an empty one. Every entry takes the look's `plateTone`. This replaces the legacy SPX `f0..f12` field payload: the layout is implied by which positions appear, so there is no layout name to send and no template to pre-author.
- A `page` below 0 throws. A `page` at or beyond `pageCount` throws — silently clamping would leave the operator pressing "next" with nothing happening and no explanation.

`pageCountFor` returns how many pages the current queue spans for that look: `ceil(candidates / boxes)`, with a minimum of 1 so page 0 is always valid, and exactly 1 when `boxes` is 0.

`LookResolution.pageCount` carries that same value so a surface can render "2 of 3" without a second call.

Nothing in this module mutates the roster, the queue, or the look definition.

- [ ] **Step 1: Write the failing tests**

`show-engine/src/lookDirector.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { findChairSlots, pageCountFor, resolveLook } from "./lookDirector.js";
import type { LookDefinition, Panelist, QueueState, Role, Slot } from "./contracts.js";

function panelist(pin: string | null, role: Role = "panelist"): Panelist {
  return {
    participantId: `p-${pin ?? "none"}`,
    rawName: `Name ${pin ?? ""}`,
    online: true,
    videoOn: true,
    audioOn: false,
    handRaised: false,
    zoomRole: 3,
    displayName: `Name ${pin ?? ""}`,
    location: "",
    pin,
    hasMukana: pin !== null,
    role
  };
}

function roster(entries: (Panelist | null)[]): Slot[] {
  return entries.map((p, index) => ({ slot: index + 1, panelist: p }));
}

const look: LookDefinition = {
  id: "teatime",
  label: "Teatime",
  scenePreset: "scene-teatime",
  boxes: 2,
  includesHost: true,
  includesReader: true,
  plateTone: "accent"
};

const queue: QueueState = {
  previous: [],
  current: "4242",
  upcoming: ["5555", "7777", "8888"]
};

const slots = roster([
  panelist("1383", "host"),
  panelist("2001", "reader"),
  panelist("4242"),
  panelist("5555"),
  panelist("7777")
]);

describe("findChairSlots", () => {
  it("finds the host and reader slots", () => {
    expect(findChairSlots(slots)).toEqual({ hostSlot: 1, readerSlot: 2 });
  });

  it("returns null for an unseated chair", () => {
    expect(findChairSlots(roster([panelist("4242")]))).toEqual({
      hostSlot: null,
      readerSlot: null
    });
  });

  it("ignores empty slots", () => {
    expect(findChairSlots(roster([null, panelist("1383", "host")]))).toEqual({
      hostSlot: 2,
      readerSlot: null
    });
  });
});

describe("pageCountFor", () => {
  it("counts pages across the candidate list", () => {
    expect(pageCountFor(look, queue)).toBe(2);
  });

  it("returns one page for an empty queue", () => {
    expect(pageCountFor(look, { previous: [], current: null, upcoming: [] })).toBe(1);
  });

  it("returns one page for a look with no boxes", () => {
    expect(pageCountFor({ ...look, boxes: 0 }, queue)).toBe(1);
  });
});

describe("resolveLook", () => {
  it("fills boxes from the front of the queue", () => {
    const resolution = resolveLook(look, { queue, slots, page: 0 });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: 3 },
      { box: 2, slot: 4 }
    ]);
  });

  it("carries the look's identity and layout through", () => {
    const resolution = resolveLook(look, { queue, slots, page: 0 });
    expect(resolution.lookId).toBe("teatime");
    expect(resolution.scenePreset).toBe("scene-teatime");
    expect(resolution.plateTone).toBe("accent");
    expect(resolution.page).toBe(0);
    expect(resolution.pageCount).toBe(2);
  });

  it("pages forward through the queue", () => {
    const resolution = resolveLook(look, { queue, slots, page: 1 });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: 5 },
      { box: 2, slot: null }
    ]);
  });

  it("seats the chairs when the look includes them", () => {
    const resolution = resolveLook(look, { queue, slots, page: 0 });
    expect(resolution.hostSlot).toBe(1);
    expect(resolution.readerSlot).toBe(2);
  });

  it("omits the reader when the look excludes it", () => {
    const resolution = resolveLook(
      { ...look, includesReader: false },
      { queue, slots, page: 0 }
    );
    expect(resolution.hostSlot).toBe(1);
    expect(resolution.readerSlot).toBeNull();
  });

  it("omits both chairs when the look excludes them", () => {
    const resolution = resolveLook(
      { ...look, includesHost: false, includesReader: false },
      { queue, slots, page: 0 }
    );
    expect(resolution.hostSlot).toBeNull();
    expect(resolution.readerSlot).toBeNull();
  });

  it("leaves a box empty for a candidate who is not seated", () => {
    const resolution = resolveLook(look, {
      queue: { previous: [], current: "9999", upcoming: ["5555"] },
      slots,
      page: 0
    });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: null },
      { box: 2, slot: 4 }
    ]);
  });

  it("resolves to no boxes for a look with none", () => {
    const resolution = resolveLook({ ...look, boxes: 0 }, { queue, slots, page: 0 });
    expect(resolution.boxes).toEqual([]);
  });

  it("blanks every box for an empty queue", () => {
    const resolution = resolveLook(look, {
      queue: { previous: [], current: null, upcoming: [] },
      slots,
      page: 0
    });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: null },
      { box: 2, slot: null }
    ]);
  });

  it("rejects a negative page", () => {
    expect(() => resolveLook(look, { queue, slots, page: -1 })).toThrow(/page/);
  });

  it("rejects a page past the end", () => {
    expect(() => resolveLook(look, { queue, slots, page: 2 })).toThrow(/page/);
  });

  it("emits one nameplate per occupied position, host and reader first", () => {
    const resolution = resolveLook(look, { queue, slots, page: 0 });
    expect(resolution.nameplates).toEqual([
      {
        position: { kind: "host" },
        slot: 1,
        name: "Name 1383",
        location: "",
        tone: "accent"
      },
      {
        position: { kind: "reader" },
        slot: 2,
        name: "Name 2001",
        location: "",
        tone: "accent"
      },
      {
        position: { kind: "box", box: 1 },
        slot: 3,
        name: "Name 4242",
        location: "",
        tone: "accent"
      },
      {
        position: { kind: "box", box: 2 },
        slot: 4,
        name: "Name 5555",
        location: "",
        tone: "accent"
      }
    ]);
  });

  it("emits no nameplate for an empty box", () => {
    const resolution = resolveLook(look, {
      queue: { previous: [], current: "9999", upcoming: [] },
      slots,
      page: 0
    });
    expect(resolution.nameplates.map((plate) => plate.position)).toEqual([
      { kind: "host" },
      { kind: "reader" }
    ]);
  });

  it("emits no chair nameplates when the look excludes the chairs", () => {
    const resolution = resolveLook(
      { ...look, includesHost: false, includesReader: false },
      { queue, slots, page: 0 }
    );
    expect(resolution.nameplates.every((plate) => plate.position.kind === "box")).toBe(true);
  });

  it("carries the look's tone onto every plate", () => {
    const resolution = resolveLook({ ...look, plateTone: "guest" }, { queue, slots, page: 0 });
    expect(resolution.nameplates.every((plate) => plate.tone === "guest")).toBe(true);
  });

  it("does not mutate the roster or the queue", () => {
    const queueCopy: QueueState = {
      previous: [...queue.previous],
      current: queue.current,
      upcoming: [...queue.upcoming]
    };
    resolveLook(look, { queue: queueCopy, slots, page: 0 });
    expect(queueCopy).toEqual(queue);
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./lookDirector.js`.

- [ ] **Step 3: Implement**

Write `lookDirector.ts` per the Interfaces and Behavior. Reuse `queueOrder` from `./handsQueue.js` rather than re-deriving the candidate order.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS, no type errors.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/lookDirector.ts show-engine/src/lookDirector.test.ts
git commit -m "feat(show-engine): look director with queue paging and chair resolution"
```

---

### Task 9: Program bus

**Files:**
- Create: `show-engine/src/programBus.ts`
- Test: `show-engine/src/programBus.test.ts`

**Interfaces:**
- Consumes: `ProgramSource`, `programSourcesEqual`, `Role` from `./contracts.js`.
- Produces:
  - `type ProgramState = { program: ProgramSource; preview: ProgramSource; activeSpeakerFollow: boolean; activeSpeakerId: string | null }`
  - `class ProgramBus` with:
    - `constructor(options?: { skipRoles?: readonly Role[] })`
    - `state(): ProgramState`
    - `setPreview(source: ProgramSource): void`
    - `cut(): void`
    - `auto(): void`
    - `directCut(source: ProgramSource): void`
    - `setActiveSpeakerFollow(on: boolean): void`
    - `onActiveSpeaker(participantId: string, role: Role): boolean`

**Behavior:**

A software model of a program/preview bus — the replacement for the hardware switcher's mix/effect stage. It holds *what* is on air; rendering it is the host's job in Plan 3.

- Both buses start at `{ kind: "black" }`. `activeSpeakerFollow` starts off and `activeSpeakerId` starts `null`.
- `setPreview(source)` sets the preview bus and leaves program alone.
- `cut()` and `auto()` both **swap** program and preview. They are distinguished only by the transition the host performs, which is not this module's concern — model them identically, and say so in a comment so nobody later "fixes" one of them.
- `directCut(source)` takes a source straight to program: the outgoing program falls to preview, and the new source becomes program. This matches the legacy direct-cut behavior and is what a one-touch source button does.
- `setActiveSpeakerFollow(on)` toggles the follow mode. Turning it **on** does not itself change either bus — the next qualifying speaker event does.
- `onActiveSpeaker(participantId, role)` records the current speaker and returns whether it changed the program bus:
  - A speaker whose `role` is in `skipRoles` is ignored entirely: `activeSpeakerId` does not move, the buses do not move, and it returns `false`. This is what keeps an ASL interpreter from taking the shot. `skipRoles` defaults to `["aslinterpreter"]`.
  - Otherwise `activeSpeakerId` updates.
  - When follow is on and program is not already `{ kind: "activeSpeaker" }`, program becomes `{ kind: "activeSpeaker" }` — the outgoing program falls to preview, exactly as `directCut` does — and it returns `true`.
  - When follow is on and program is already `{ kind: "activeSpeaker" }`, the buses do not move and it returns `false`: the source has not changed, only who is behind it. Downstream reacts to `activeSpeakerId` for that.
  - When follow is off, the buses never move and it returns `false`.
- `state()` returns a fresh object; a caller mutating it cannot reach internal state.

- [ ] **Step 1: Write the failing tests**

`show-engine/src/programBus.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { ProgramBus } from "./programBus.js";

describe("ProgramBus", () => {
  it("starts black on both buses with follow off", () => {
    const bus = new ProgramBus();
    expect(bus.state()).toEqual({
      program: { kind: "black" },
      preview: { kind: "black" },
      activeSpeakerFollow: false,
      activeSpeakerId: null
    });
  });

  it("sets preview without touching program", () => {
    const bus = new ProgramBus();
    bus.setPreview({ kind: "gallery" });
    expect(bus.state().preview).toEqual({ kind: "gallery" });
    expect(bus.state().program).toEqual({ kind: "black" });
  });

  it("swaps the buses on cut", () => {
    const bus = new ProgramBus();
    bus.setPreview({ kind: "gallery" });
    bus.cut();
    expect(bus.state().program).toEqual({ kind: "gallery" });
    expect(bus.state().preview).toEqual({ kind: "black" });
  });

  it("swaps the buses on auto, identically to cut", () => {
    const bus = new ProgramBus();
    bus.setPreview({ kind: "look", lookId: "banter" });
    bus.auto();
    expect(bus.state().program).toEqual({ kind: "look", lookId: "banter" });
    expect(bus.state().preview).toEqual({ kind: "black" });
  });

  it("drops the outgoing program to preview on a direct cut", () => {
    const bus = new ProgramBus();
    bus.setPreview({ kind: "gallery" });
    bus.cut();
    bus.directCut({ kind: "slot", slot: 3 });
    expect(bus.state().program).toEqual({ kind: "slot", slot: 3 });
    expect(bus.state().preview).toEqual({ kind: "gallery" });
  });

  it("does not move the buses when follow is switched on", () => {
    const bus = new ProgramBus();
    bus.setActiveSpeakerFollow(true);
    expect(bus.state().program).toEqual({ kind: "black" });
    expect(bus.state().activeSpeakerFollow).toBe(true);
  });

  it("takes the active-speaker source when follow is on", () => {
    const bus = new ProgramBus();
    bus.setPreview({ kind: "gallery" });
    bus.cut();
    bus.setActiveSpeakerFollow(true);
    expect(bus.onActiveSpeaker("z1", "panelist")).toBe(true);
    expect(bus.state().program).toEqual({ kind: "activeSpeaker" });
    expect(bus.state().preview).toEqual({ kind: "gallery" });
    expect(bus.state().activeSpeakerId).toBe("z1");
  });

  it("does not re-take when already on the active-speaker source", () => {
    const bus = new ProgramBus();
    bus.setActiveSpeakerFollow(true);
    bus.onActiveSpeaker("z1", "panelist");
    expect(bus.onActiveSpeaker("z2", "panelist")).toBe(false);
    expect(bus.state().program).toEqual({ kind: "activeSpeaker" });
    expect(bus.state().activeSpeakerId).toBe("z2");
  });

  it("tracks the speaker but never cuts when follow is off", () => {
    const bus = new ProgramBus();
    expect(bus.onActiveSpeaker("z1", "panelist")).toBe(false);
    expect(bus.state().activeSpeakerId).toBe("z1");
    expect(bus.state().program).toEqual({ kind: "black" });
  });

  it("ignores a skipped role entirely", () => {
    const bus = new ProgramBus();
    bus.setActiveSpeakerFollow(true);
    bus.onActiveSpeaker("z1", "panelist");
    expect(bus.onActiveSpeaker("interpreter", "aslinterpreter")).toBe(false);
    expect(bus.state().activeSpeakerId).toBe("z1");
  });

  it("does not let a skipped role take program from black", () => {
    const bus = new ProgramBus();
    bus.setActiveSpeakerFollow(true);
    expect(bus.onActiveSpeaker("interpreter", "aslinterpreter")).toBe(false);
    expect(bus.state().program).toEqual({ kind: "black" });
    expect(bus.state().activeSpeakerId).toBeNull();
  });

  it("honours a configured skip list", () => {
    const bus = new ProgramBus({ skipRoles: ["reader"] });
    bus.setActiveSpeakerFollow(true);
    expect(bus.onActiveSpeaker("r", "reader")).toBe(false);
    expect(bus.onActiveSpeaker("i", "aslinterpreter")).toBe(true);
    expect(bus.state().activeSpeakerId).toBe("i");
  });

  it("returns a fresh state object", () => {
    const bus = new ProgramBus();
    const state = bus.state();
    state.program = { kind: "gallery" };
    expect(bus.state().program).toEqual({ kind: "black" });
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./programBus.js`.

- [ ] **Step 3: Implement**

Write `programBus.ts` per the Interfaces and Behavior. Use `programSourcesEqual` from `./contracts.js` for the "already on this source" check rather than comparing by reference or by `JSON.stringify`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS, no type errors.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/programBus.ts show-engine/src/programBus.test.ts
git commit -m "feat(show-engine): program bus with active-speaker follow"
```

---

### Task 10: Persistence, exports, and the direction pipeline test

**Files:**
- Modify: `show-engine/src/persistence.ts`
- Modify: `show-engine/src/index.ts`
- Test: `show-engine/src/persistence.test.ts` (append), `show-engine/src/directionPipeline.test.ts` (create)

**Interfaces:**
- Consumes: everything built in Tasks 1–9, plus Plan 1's `LiveSlots`, `buildPanelistDb`, `ZoomIngest`, `MukanaRegistry`, `OverrideDb`, `StateStore`.
- Produces: `ShowState` gains `gallery: GalleryState`; `index.ts` exports every new module's public surface.

**Behavior:**

`ShowState` becomes `{ version: 1; slots: LiveSlotsState; overrides: Record<string, OverrideRecord>; gallery: GalleryState }`. `load()`'s shallow shape check extends to require `gallery` to be a non-null object carrying a numeric `cells` and an array `assignments` — the same depth it already applies to `slots`, and no deeper. Roster and gallery coherence stay the responsibility of `LiveSlots.fromJSON` and `GalleryDirector.fromJSON`, which throw their own named errors.

**Version handling is a deliberate decision, not an oversight:** a state file written before this plan has no `gallery` key. Such a file is rejected by `load()` (returning `null`, so the engine starts clean) rather than being migrated with a default gallery. This is pre-release software with no deployed state files, and a silent migration path is a liability that would have to be maintained forever. Do not add one.

`index.ts` gains exports for: `PlateTone`, `PLATE_TONES`, `isPlateTone`, `LookDefinition`, `GalleryCell`, `QueueState`, `ProgramSource`, `programSourcesEqual`, `MukanaEndpoint`, `HandsOutcome`, `parseHandsPayload`, `stripChairs`, `queueOrder`, `PlacementChange`, `PositionAssigner`, `FiloAssigner`, `VisibleSetAssigner`, `RecencyScores`, `GalleryState`, `GalleryError`, `GalleryDirector`, `BoxAssignment`, `NameplatePosition`, `Nameplate`, `LookResolution`, `findChairSlots`, `resolveLook`, `pageCountFor`, `ProgramState`, `ProgramBus`.

The new integration test proves the direction layer composes with Plan 1's roster layer. It must assert behavior that emerges only from the composition — not restate the unit tests through the barrel. The four scenarios below each do that.

- [ ] **Step 1: Write the failing tests**

Append to `show-engine/src/persistence.test.ts`:

```ts
describe("StateStore gallery node", () => {
  const withGallery: ShowState = {
    version: 1,
    slots: { version: 1, capacity: 2, seats: [null, null] },
    overrides: {},
    gallery: { version: 1, cells: 2, assignments: [{ cell: 1, slot: 0 }, { cell: 2, slot: 0 }] }
  };

  it("round-trips a state carrying a gallery", async () => {
    const store = new StateStore("/show/state.json", { fs: fakeFs() });
    await store.save(withGallery);
    expect(await store.load()).toEqual(withGallery);
  });

  it("returns null for a state file with no gallery node", async () => {
    const legacy = JSON.stringify({
      version: 1,
      slots: withGallery.slots,
      overrides: {}
    });
    const store = new StateStore("/show/state.json", {
      fs: fakeFs({ "/show/state.json": legacy })
    });
    expect(await store.load()).toBeNull();
  });

  it("returns null when the gallery node is malformed", async () => {
    const store = new StateStore("/show/state.json", {
      fs: fakeFs({
        "/show/state.json": JSON.stringify({ ...withGallery, gallery: { cells: 2 } })
      })
    });
    expect(await store.load()).toBeNull();
  });
});
```

Create `show-engine/src/directionPipeline.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import {
  buildPanelistDb,
  FiloAssigner,
  GalleryDirector,
  LiveSlots,
  OverrideDb,
  ProgramBus,
  RecencyScores,
  parseHandsPayload,
  resolveLook,
  stripChairs,
  findChairSlots,
  ZoomIngest,
  type LookDefinition,
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

const mukana: MukanaDb = {
  "1383": {
    pin: "1383",
    displayName: "J.J. Mc Kenna",
    location: "CA",
    role: "host",
    online: true
  },
  "2001": { pin: "2001", displayName: "Reader Rose", location: "NY", role: "reader", online: true },
  "4242": { pin: "4242", displayName: "Ann Lee", location: "TX", role: "panelist", online: true },
  "5555": { pin: "5555", displayName: "Bo Diaz", location: "PE", role: "panelist", online: true },
  "7777": {
    pin: "7777",
    displayName: "Sam Signer",
    location: "WA",
    role: "aslinterpreter",
    online: true
  }
};

function seatedRoster(): LiveSlots {
  const ingest = new ZoomIngest();
  for (const [id, pin] of [
    ["z-host", "1383"],
    ["z-reader", "2001"],
    ["z-ann", "4242"],
    ["z-bo", "5555"],
    ["z-asl", "7777"]
  ] as const) {
    ingest.apply({ kind: "joined", participant: participant(id, `Name | ${pin}`) });
  }
  ingest.commit();

  // ZoomIngest publishes sorted by participantId, and buildPanelistDb preserves that
  // order, so rebuild seats: z-ann, z-asl, z-bo, z-host, z-reader. Assertions below
  // derive slot numbers via slotOf rather than hardcoding that order.
  const slots = new LiveSlots({ capacity: 8, utilityPinBase: 9000 });
  slots.rebuild([...buildPanelistDb(ingest.snapshot(), mukana, new OverrideDb().entries()).values()]);
  return slots;
}

const teatime: LookDefinition = {
  id: "teatime",
  label: "Teatime",
  scenePreset: "scene-teatime",
  boxes: 2,
  includesHost: true,
  includesReader: true,
  plateTone: "accent"
};

describe("direction pipeline", () => {
  it("resolves a look against a roster built from the registry", () => {
    const slots = seatedRoster();
    const parsed = parseHandsPayload("5555\n4242\nNONE");
    expect(parsed.kind).toBe("data");
    if (parsed.kind !== "data") return;

    const chairs = findChairSlots(slots.slots());
    const queue = stripChairs(parsed.queue, {
      hostPin: "1383",
      readerPin: "2001"
    });

    const resolution = resolveLook(teatime, { queue, slots: slots.slots(), page: 0 });
    expect(resolution.hostSlot).toBe(slots.slotOf("z-host"));
    expect(resolution.readerSlot).toBe(slots.slotOf("z-reader"));
    expect(chairs).toEqual({
      hostSlot: slots.slotOf("z-host"),
      readerSlot: slots.slotOf("z-reader")
    });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: slots.slotOf("z-ann") },
      { box: 2, slot: slots.slotOf("z-bo") }
    ]);
    expect(resolution.nameplates.map((plate) => plate.name)).toEqual([
      "J.J. Mc Kenna",
      "Reader Rose",
      "Ann Lee",
      "Bo Diaz"
    ]);
  });

  it("keeps a chair out of the guest boxes even when they raise a hand", () => {
    const slots = seatedRoster();
    const parsed = parseHandsPayload("1383,4242\n2001\nNONE");
    expect(parsed.kind).toBe("data");
    if (parsed.kind !== "data") return;

    const queue = stripChairs(parsed.queue, { hostPin: "1383", readerPin: "2001" });
    const resolution = resolveLook(teatime, { queue, slots: slots.slots(), page: 0 });

    const boxedSlots = resolution.boxes.map((box) => box.slot);
    expect(boxedSlots).not.toContain(resolution.hostSlot);
    expect(boxedSlots).not.toContain(resolution.readerSlot);
    expect(boxedSlots).toEqual([slots.slotOf("z-ann"), null]);
  });

  it("never lets an ASL interpreter take program, even as the newest speaker", () => {
    const slots = seatedRoster();
    const bus = new ProgramBus();
    bus.setActiveSpeakerFollow(true);

    const aslSlot = slots.slotOf("z-asl");
    expect(aslSlot).not.toBeNull();
    const asl = slots.slots()[(aslSlot ?? 1) - 1]?.panelist;
    expect(asl?.role).toBe("aslinterpreter");

    expect(bus.onActiveSpeaker("z-ann", "panelist")).toBe(true);
    expect(bus.state().activeSpeakerId).toBe("z-ann");

    expect(bus.onActiveSpeaker("z-asl", asl?.role ?? "panelist")).toBe(false);
    expect(bus.state().activeSpeakerId).toBe("z-ann");
    expect(bus.state().program).toEqual({ kind: "activeSpeaker" });
  });

  it("reorders the gallery by recency without disturbing the roster", () => {
    const slots = seatedRoster();
    const gallery = new GalleryDirector({ cells: 4 });
    gallery.resetFromSlots(slots.slots());
    const before = gallery.cells().map((cell) => cell.slot);

    const scores = new RecencyScores();
    scores.onActiveSpeaker("z-bo");
    const order = scores
      .order(["z-host", "z-reader", "z-ann", "z-bo"])
      .map((id) => slots.slotOf(id))
      .filter((slot): slot is number => slot !== null);

    gallery.applyOrder(order);
    expect(gallery.cells()[0]?.slot).toBe(slots.slotOf("z-bo"));
    expect(gallery.cells().map((cell) => cell.slot)).not.toEqual(before);
    expect(order[0]).toBe(slots.slotOf("z-bo"));
  });

  it("keeps the most recent speakers in a limited position pool", () => {
    const slots = seatedRoster();
    const filo = new FiloAssigner({ capacity: 2 });
    for (const id of ["z-host", "z-ann", "z-bo"]) filo.onActiveSpeaker(id);

    const occupants = [...filo.positions().values()];
    expect(occupants).toContain("z-bo");
    expect(occupants).toContain("z-ann");
    expect(occupants).not.toContain("z-host");
    expect(slots.slotOf("z-host")).not.toBeNull();
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `npm run test --workspace show-engine`
Expected: FAIL — `ShowState` has no `gallery`; `directionPipeline.test.ts` cannot resolve several exports from `./index.js`.

- [ ] **Step 3: Implement**

Extend `persistence.ts` and `index.ts` per the Interfaces and Behavior. If a name the test imports does not exist under that spelling, fix the barrel — never rename another module's export to match.

- [ ] **Step 4: Run the full suite, typecheck, and build**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine && npm run build --workspace show-engine`
Expected: PASS, no type errors, `dist/` emitted with declarations. Every Plan 1 test must still pass.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/persistence.ts show-engine/src/persistence.test.ts show-engine/src/index.ts show-engine/src/directionPipeline.test.ts
git commit -m "feat(show-engine): persist gallery state, export direction modules"
```

---

## Definition of Done

- [ ] `npm run test --workspace show-engine` passes, including every Plan 1 test.
- [ ] `npm run typecheck --workspace show-engine` is clean.
- [ ] `npm run build --workspace show-engine` emits `dist/` with declarations.
- [ ] The package remains registered in the root `workspaces` array **and** in the `test:gate` chain (Plan 1 added both; confirm neither regressed).
- [ ] Every new module carries its header comment and has an adjacent test file.
- [ ] Both Plan 1 deferred items scheduled here are closed: the `ZoomIngest` revision counter (Task 2) and per-endpoint `MukanaHealth` (Task 3).
- [ ] All work is committed on `plan/ohg-show-engine-direction`.

## What Plan 3 picks up

`tallyPublisher` (derive on-air PIN lists from program source, look resolution, and gallery
state), `overlayDirector` (feeding the nameplates this plan resolves, plus the current
question, to CVP's integrated lower-third/overlay engine through the adapter, with
change-detection so a plate only re-renders when its text actually changes), the
`HostAdapter` port with its mock and conformance suite, and
the orchestrator that owns the polling loop, debounced persistence, and the `ohg.*` control
registration. Plan 3 also inherits the remaining Plan 1 deferrals listed in the outcomes
doc — notably `readPin` shape validation, the dormant-gate type check, and documenting that
`StateFs.mkdir` must be recursive and idempotent before a real filesystem is wired in.
