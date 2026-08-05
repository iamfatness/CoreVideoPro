# OHG Show Engine — Plan 3: Outputs

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the four contract gaps Plan 2 left between modules, then derive the show's two outputs — who is on air (tally) and what text is on screen (nameplates and the current question).

**Architecture:** Continues the `show-engine/` package. Everything here is still pure derivation over in-memory state; sending these outputs anywhere is Plan 4's job. Two of the gap fixes touch existing modules (`mukanaClient`, `programBus`, `lookDirector`, `config`); the rest is new.

**Tech Stack:** TypeScript 5.9 (strict, ES2022, NodeNext), vitest 4, Node 24.

**Source documents:**
- Spec: `docs/superpowers/specs/2026-08-04-ohg-show-engine-design.md`
- Algorithm reference: `docs/superpowers/specs/2026-08-04-ohg-isadora-actor-reference.md`
- **Read first:** `docs/superpowers/plans/2026-08-04-ohg-show-engine-direction-outcomes.md`

**Plan series (this is Plan 3 of 8):**
1. Core identity & roster — shipped (PR #363)
2. Direction — shipped (PR #364)
3. **Outputs** ← this plan
4. Host port & orchestrator (`HostAdapter` + mock + conformance suite; the orchestrator owning the polling loop, debounced persistence, and the `ohg.*` action surface)
5. CVP Windows host integration
6. mac-shell control server + panel
7. OBS plugin adapter
8. Migration tooling

## Plan format

As in Plan 2: **interfaces, behavior prose, and complete tests — no implementation
bodies.** Write the implementation yourself from the interface and the behavior. Tests are
the executable specification and are binding. If behavior text seems to contradict a test,
stop and report NEEDS_CONTEXT rather than guessing.

Two refinements carried from Plan 2's outcomes:
- **Where two tasks touch one file, the shared surface is stated explicitly.** Task 1
  refactors `programBus` to consume a new predicate; Task 1 owns that edit and Task 7 must
  not re-litigate it.
- **A cross-task contract pass was run while authoring.** For each new module: what
  produces its input, what consumes its output, do the types meet? Findings are recorded
  inline in the task that owns them.

## Global Constraints

- **Branch:** `plan/ohg-show-engine-outputs`, stacked on `plan/ohg-show-engine-direction` (PR #364). Commit after every task.
- **Module system:** NodeNext. **Every relative import MUST end in `.js`.**
- **vitest runs with `globals: false`** — test files must explicitly `import { describe, expect, it } from "vitest";`.
- **Strict TypeScript.** No `any`. No non-null assertions where a guard will do.
- **No I/O in this plan.** Pure logic over in-memory state; no network, filesystem, or timers. `MukanaClient` keeps its injected `fetch` and gains no other I/O.
- **Copy, do not alias** — on both read and write. Accessors return fresh structures; mutators clone what they are given. This is uniform across the package and is the single most-repeated review finding across Plans 1–2.
- **Loud, never silent.** Invalid arguments throw with actionable messages; parse failures return typed outcomes. Nothing is silently clamped, truncated, or swallowed — with one sanctioned exception, `clampPage`, whose entire purpose is clamping and which is named so callers cannot use it by accident.
- **`OverrideDb` is authoritative for editorial roles.** Nothing in this plan assigns or mutates a role.
- **Slot, cell, box, and position numbers are 1-based.**
- **Test files sit adjacent to their module**; every source file opens with a `/** ... */` block comment describing its responsibility.
- **Do NOT use `git stash`.** The stash stack is shared with other working trees on this machine and the owner works in them concurrently.

---

## File Structure

```
show-engine/src/
  speakerGate.ts       — NEW: the single shared active-speaker skip predicate
  programBus.ts        — MODIFY: consume speakerGate instead of its private check
  config.ts            — MODIFY: add galleryCells
  lookDirector.ts      — MODIFY: add clampPage; add tallySource to look resolution
  contracts.ts         — MODIFY: add LookDefinition.tallySource, MukanaQuestion
  mukanaParse.ts       — MODIFY: add parseMukanaQuestion
  mukanaClient.ts      — MODIFY: per-endpoint parse strategy (stop running the panelist
                         parser over hands and question bodies)
  tallyPublisher.ts    — NEW: derive who is on air from the program source
  overlayDirector.ts   — NEW: derive on-screen text, with change detection
  index.ts             — MODIFY: export the new surface
```

---

### Task 1: The shared active-speaker gate

**Files:**
- Create: `show-engine/src/speakerGate.ts`
- Modify: `show-engine/src/programBus.ts`
- Test: `show-engine/src/speakerGate.test.ts`

**Interfaces:**
- Consumes: `Role`, `DEFAULT_SKIP_ROLES` from `./contracts.js`.
- Produces: `function shouldFollowSpeaker(role: Role | null, skipRoles: readonly Role[]): boolean`.

**Behavior:**

This closes Plan 2's second contract gap. The accessibility rule — an ASL interpreter signs
continuously while a panelist talks, so they must never take the shot — was enforced only
inside `ProgramBus`. The three position assigners in `speakerRecency.ts` take a bare
participant id and know nothing about roles, so an interpreter was blocked from program but
still evicted a panelist from the FILO pool, still swapped into the visible gallery window,
and still sorted to the front of the gallery.

The spec (§3.7) said the rule "lives in `speakerRecency`". It does not, and putting it there
would mean threading a role through three classes that otherwise deal only in ids. The
resolution is a **single shared predicate applied at dispatch**: every consumer of an
active-speaker event asks `shouldFollowSpeaker` first. `ProgramBus` uses it internally
(replacing its private check), and Plan 4's orchestrator uses it before fanning the event
out to any assigner. One rule, one definition, no API churn on the assigners.

`shouldFollowSpeaker` returns `false` when `role` is in `skipRoles`, and `true` otherwise.
A `null` role means the speaker is not seated and therefore has no editorial role; it
returns **`true`**. That is deliberate and is the safer default: an unseated speaker is an
ordinary participant until the roster says otherwise, and returning `false` would silently
freeze active-speaker follow whenever someone unseated talks.

`ProgramBus` keeps its `skipRoles` constructor option and its existing behavior exactly —
only the internal check is replaced. Its existing tests must pass unchanged.

**Cross-task note:** Task 1 owns the `programBus.ts` edit. No later task in this plan
modifies that file.

- [ ] **Step 1: Write the failing test**

`show-engine/src/speakerGate.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { shouldFollowSpeaker } from "./speakerGate.js";
import { DEFAULT_SKIP_ROLES } from "./contracts.js";

describe("shouldFollowSpeaker", () => {
  it("blocks a role in the skip list", () => {
    expect(shouldFollowSpeaker("aslinterpreter", DEFAULT_SKIP_ROLES)).toBe(false);
  });

  it("allows a role not in the skip list", () => {
    expect(shouldFollowSpeaker("panelist", DEFAULT_SKIP_ROLES)).toBe(true);
    expect(shouldFollowSpeaker("host", DEFAULT_SKIP_ROLES)).toBe(true);
  });

  it("allows an unseated speaker with no role", () => {
    expect(shouldFollowSpeaker(null, DEFAULT_SKIP_ROLES)).toBe(true);
  });

  it("honours a custom skip list", () => {
    expect(shouldFollowSpeaker("reader", ["reader"])).toBe(false);
    expect(shouldFollowSpeaker("aslinterpreter", ["reader"])).toBe(true);
  });

  it("allows everything when the skip list is empty", () => {
    expect(shouldFollowSpeaker("aslinterpreter", [])).toBe(true);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./speakerGate.js`.

- [ ] **Step 3: Implement**

Write `speakerGate.ts`, then refactor `programBus.ts` to call it in place of its private
skip check. Do not change `ProgramBus`'s public interface or any of its behavior.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS. `programBus.test.ts` must be green **without edits** — if you needed to
change one of its assertions, you changed behavior; stop and report.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/speakerGate.ts show-engine/src/speakerGate.test.ts show-engine/src/programBus.ts
git commit -m "feat(show-engine): shared active-speaker skip predicate"
```

---

### Task 2: Gallery dimensions in config

**Files:**
- Modify: `show-engine/src/config.ts`
- Test: `show-engine/src/config.test.ts` (append)

**Interfaces:**
- Consumes: existing `parseShowEngineConfig`, `ShowEngineConfig`.
- Produces: `ShowEngineConfig` gains `galleryCells: number`.

**Behavior:**

Closes Plan 2's third contract gap. The gallery is persisted, and
`GalleryDirector.fromJSON(state, { cells })` throws `GalleryError` when the persisted cell
count disagrees with the configured one — but nothing configured it. Plan 4's orchestrator
owns restore and has nowhere to read the number from.

`galleryCells` is optional and defaults to **16**, matching the legacy multiview grid the
show ran on. It must be an integer of at least 1, validated with the file's existing
`optionalPositiveInt` helper and its `(source, key, label)` shape.

**A decision Plan 4 inherits, recorded here so it is not re-argued:** config is the
authority for gallery dimensions. The adapter's `maxGalleryCells` capability (spec §6)
*validates* it — an adapter that cannot deliver the configured count is a startup error, not
a silent resize. Taking the number from the adapter instead would turn every host swap into
a `GalleryError` on the next restore.

- [ ] **Step 1: Write the failing test**

Append to `show-engine/src/config.test.ts`:

```ts
describe("parseShowEngineConfig gallery dimensions", () => {
  it("defaults galleryCells to sixteen", () => {
    expect(parseShowEngineConfig(minimal).galleryCells).toBe(16);
  });

  it("keeps an explicit galleryCells", () => {
    expect(parseShowEngineConfig({ ...minimal, galleryCells: 9 }).galleryCells).toBe(9);
  });

  it("rejects a galleryCells below one", () => {
    expect(() => parseShowEngineConfig({ ...minimal, galleryCells: 0 })).toThrow(
      /galleryCells/
    );
  });

  it("rejects a non-integer galleryCells", () => {
    expect(() => parseShowEngineConfig({ ...minimal, galleryCells: 4.5 })).toThrow(
      /galleryCells/
    );
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — `galleryCells` is undefined.

- [ ] **Step 3: Implement**

Add the field per the Behavior above, following the file's existing validation idiom.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/config.ts show-engine/src/config.test.ts
git commit -m "feat(show-engine): configurable gallery cell count"
```

---

### Task 3: Page clamping for look resolution

**Files:**
- Modify: `show-engine/src/lookDirector.ts`
- Test: `show-engine/src/lookDirector.test.ts` (append)

**Interfaces:**
- Consumes: `LookDefinition`, `QueueState` from `./contracts.js`; existing `pageCountFor`.
- Produces: `function clampPage(look: LookDefinition, queue: QueueState, page: number): number`.

**Behavior:**

Closes Plan 2's fourth contract gap. `resolveLook` throws on an out-of-range page, which is
right for an *operator action* — pressing "next guest" past the end should say so rather
than do nothing — and wrong for a *re-resolve at an unchanged page*, which Plan 4's
orchestrator does every tick. `pageCount` shrinks when hands are lowered, so a page that was
valid a second ago can throw mid-show.

`clampPage` returns the nearest valid page: at least 0, at most `pageCountFor(...) - 1`. A
non-integer or `NaN` page throws rather than being rounded — that is a caller bug, not a
stale value, and silently rounding it would hide the mistake.

`resolveLook` is **unchanged**: it still throws. The two callers are different and now have
different tools. Document that division on `clampPage` and in `resolveLook`'s doc comment,
so a future reader does not "unify" them: the orchestrator clamps then resolves; an operator
action resolves directly and lets the throw surface.

This is the plan's one sanctioned exception to "loud, never silent", and the name is the
guardrail — a caller reaching for `clampPage` is asking for clamping explicitly.

- [ ] **Step 1: Write the failing test**

Append to `show-engine/src/lookDirector.test.ts`:

```ts
describe("clampPage", () => {
  it("leaves a valid page alone", () => {
    expect(clampPage(look, queue, 0)).toBe(0);
    expect(clampPage(look, queue, 1)).toBe(1);
  });

  it("clamps a page past the end to the last valid page", () => {
    expect(clampPage(look, queue, 5)).toBe(1);
  });

  it("clamps a negative page to zero", () => {
    expect(clampPage(look, queue, -3)).toBe(0);
  });

  it("clamps to zero when the queue empties", () => {
    const empty: QueueState = { previous: [], current: null, upcoming: [] };
    expect(clampPage(look, empty, 3)).toBe(0);
  });

  it("clamps to zero for a look with no boxes", () => {
    expect(clampPage({ ...look, boxes: 0 }, queue, 2)).toBe(0);
  });

  it("throws on a non-integer page", () => {
    expect(() => clampPage(look, queue, 1.5)).toThrow(/page/);
    expect(() => clampPage(look, queue, Number.NaN)).toThrow(/page/);
  });

  it("produces a page resolveLook accepts, for any integer input", () => {
    for (const candidate of [-10, 0, 1, 2, 99]) {
      const page = clampPage(look, queue, candidate);
      expect(() => resolveLook(look, { queue, slots, page })).not.toThrow();
    }
  });
});
```

Extend that file's import to include `clampPage`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — `clampPage` is not exported.

- [ ] **Step 3: Implement**

Add `clampPage` per the Behavior above and update both doc comments. Do not change
`resolveLook`'s throwing.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS, with `lookDirector`'s existing tests unchanged.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/lookDirector.ts show-engine/src/lookDirector.test.ts
git commit -m "feat(show-engine): clampPage for orchestrator re-resolves"
```

---

### Task 4: Question payload parsing

**Files:**
- Modify: `show-engine/src/contracts.ts`
- Modify: `show-engine/src/mukanaParse.ts`
- Test: `show-engine/src/mukanaParse.test.ts` (append)

**Interfaces:**
- Consumes: existing `MukanaOutcome` shape conventions.
- Produces:
  - from `contracts.ts`: `type MukanaQuestion = { key: string; askerName: string; text: string; tag: string; votes: number; timestampMs: number }`
  - from `mukanaParse.ts`: `type QuestionOutcome = { kind: "data"; question: MukanaQuestion | null } | { kind: "dormant"; detail: string } | { kind: "invalid"; reason: string }`, and `function parseMukanaQuestion(body: string): QuestionOutcome`

**Behavior:**

Half of Plan 2's first contract gap: the question endpoint has no parser, and Task 5 is
about to stop feeding it the *panelist* parser.

The endpoint returns the record captured in the algorithm reference:

```json
{"q":{"key":"-Mms66PcbK_9cAj550wX","n":"Douglas Carmichael","q":"Do you think that …",
      "tag":"Zoom ISO","ts":1635176445667,"v":-1},
 "hands":{"prev":[],"curr":[],"next":[]}}
```

Only the `q` node is this parser's business; `hands` in the same body is ignored here
(Task 5 explains why the two endpoints are read separately).

Rules:
- Non-JSON, or JSON that is not an object, → `invalid` with a reason.
- A body carrying a `status` key is the off-hours envelope → `dormant`, carrying `detail`
  when present, exactly as `parseMukanaPanelists` already does. Reuse that gate's logic
  rather than re-deriving it.
- No `q` node, or a `q` that is not an object, → `{ kind: "data", question: null }`. That is
  "no question is currently up", which is a normal show state, not an error.
- Otherwise map: `key`←`key`, `askerName`←`n`, `text`←`q`, `tag`←`tag`, `votes`←`v`,
  `timestampMs`←`ts`. Strings are trimmed and default to `""` when absent or non-string;
  `votes` and `timestampMs` default to `0` when absent or non-numeric.
- **Newlines in the question text collapse to single spaces and the result is trimmed.** The
  legacy graphics path did this (`value.replace(/[\n\r]+/g,' ').trim()`) because a pasted
  multi-line question breaks the lower-third layout. Keep it — the same reason applies to
  our own renderer.
- A question whose `text` is empty after normalization is still returned; deciding whether
  an empty question is worth showing belongs to `overlayDirector` (Task 7), not here.

- [ ] **Step 1: Write the failing test**

Append to `show-engine/src/mukanaParse.test.ts`:

```ts
const questionBody = JSON.stringify({
  q: {
    key: "-Mms66PcbK_9cAj550wX",
    n: "Douglas Carmichael",
    q: "Do you think that adding\nslickness to an event\r\naffects community?",
    tag: "Zoom ISO",
    ts: 1635176445667,
    v: -1
  },
  hands: { prev: [], curr: [], next: [] }
});

describe("parseMukanaQuestion", () => {
  it("maps the question record", () => {
    const outcome = parseMukanaQuestion(questionBody);
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.question).toEqual({
      key: "-Mms66PcbK_9cAj550wX",
      askerName: "Douglas Carmichael",
      text: "Do you think that adding slickness to an event affects community?",
      tag: "Zoom ISO",
      votes: -1,
      timestampMs: 1635176445667
    });
  });

  it("collapses newlines in the question text", () => {
    const outcome = parseMukanaQuestion(
      JSON.stringify({ q: { q: "one\n\ntwo\r\nthree  " } })
    );
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.question?.text).toBe("one two three");
  });

  it("returns a null question when no q node is present", () => {
    const outcome = parseMukanaQuestion(JSON.stringify({ hands: { prev: [] } }));
    expect(outcome).toEqual({ kind: "data", question: null });
  });

  it("returns a null question when q is not an object", () => {
    expect(parseMukanaQuestion(JSON.stringify({ q: "nope" }))).toEqual({
      kind: "data",
      question: null
    });
  });

  it("defaults missing fields", () => {
    const outcome = parseMukanaQuestion(JSON.stringify({ q: { n: "Ann" } }));
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.question).toEqual({
      key: "",
      askerName: "Ann",
      text: "",
      tag: "",
      votes: 0,
      timestampMs: 0
    });
  });

  it("treats the off-hours envelope as dormant", () => {
    const outcome = parseMukanaQuestion(
      JSON.stringify({ status: 200, detail: "outside show hours" })
    );
    expect(outcome).toEqual({ kind: "dormant", detail: "outside show hours" });
  });

  it("reports an unparseable body as invalid", () => {
    expect(parseMukanaQuestion("<html>502</html>").kind).toBe("invalid");
  });

  it("reports a non-object body as invalid", () => {
    expect(parseMukanaQuestion("[1,2,3]").kind).toBe("invalid");
  });
});
```

Extend that file's import to include `parseMukanaQuestion`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — `parseMukanaQuestion` is not exported.

- [ ] **Step 3: Implement**

Add `MukanaQuestion` to `contracts.ts` and `parseMukanaQuestion` to `mukanaParse.ts`. Share
the JSON-decode and dormant-envelope logic with `parseMukanaPanelists` rather than
duplicating it.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/contracts.ts show-engine/src/mukanaParse.ts show-engine/src/mukanaParse.test.ts
git commit -m "feat(show-engine): question payload parsing"
```

---

### Task 5: Per-endpoint parse strategy

**Files:**
- Modify: `show-engine/src/mukanaClient.ts`
- Test: `show-engine/src/mukanaClient.test.ts` (append and adjust)

**Interfaces:**
- Consumes: `parseMukanaPanelists`, `parseMukanaQuestion`, `MukanaOutcome`, `QuestionOutcome` from `./mukanaParse.js`; `parseHandsPayload`, `HandsOutcome` from `./handsQueue.js`.
- Produces: `MukanaClient.fetchHands(): Promise<HandsOutcome | { kind: "dormant"; detail: string }>` and `MukanaClient.fetchQuestion(): Promise<QuestionOutcome>`. `fetchPanelists(): Promise<MukanaOutcome>` is unchanged.

**Behavior:**

The other half of Plan 2's first contract gap, and the one that actually bites.

`request()` currently runs `parseMukanaPanelists` over **every** endpoint's body and
discards the raw text, so `parseHandsPayload` can never be reached and the hands response —
which is plain text, not JSON — makes `JSON.parse` throw. The outcome becomes `invalid`,
that endpoint's health goes `failing`, and it backs off exponentially against a perfectly
healthy server.

**The hands wire format is settled; do not change `parseHandsPayload`.** It was verified
against the raw patch extraction: the patch contains a native "Get URL Text" actor named
"Get Mukana Hands" pointed at `?req=hands` whose captured output is the three-line form
(`1039,1726` / `NONE` / `NONE`), matching the input pin of the actor that consumes it, and
**no actor anywhere parses a JSON hands shape**. The outcomes doc records the full evidence.

Fix: give `request()` a per-endpoint parse strategy. Each endpoint keeps its own parser;
everything else about the request path — URL construction, the injected `fetch`, thrown
errors and non-2xx becoming `invalid`, the health/backoff bookkeeping, dormant not counting
as a failure — stays exactly as it is and stays shared. Do not fork the request path into
three copies.

Two shape details that fall out:
- `parseHandsPayload` returns `HandsOutcome`, which has no `dormant` arm — but the endpoint
  can still return the off-hours envelope. The client detects that envelope **before**
  delegating to the endpoint's parser (it is a transport-level condition, not a payload
  concern) and returns the dormant outcome directly, leaving `parseHandsPayload` untouched.
  That is why `fetchHands`'s return type is a union with the dormant arm.
- Health classification is unchanged for all three endpoints: `dormant` resets the failure
  count; `invalid` increments it; success clears it.

**Cross-task contract check (recorded during authoring):** `fetchHands()` now yields a
`HandsOutcome`, whose `data` arm carries a `QueueState` — which is exactly what
`stripChairs` and `queueOrder` consume, and what `resolveLook` needs. `fetchQuestion()`
yields a `QuestionOutcome` carrying `MukanaQuestion | null`, which is what `overlayDirector`
(Task 7) consumes. Both ends meet.

- [ ] **Step 1: Write the failing test**

Append to `show-engine/src/mukanaClient.test.ts`:

```ts
const handsBody = "4242,5555\n1383\nNONE";

describe("MukanaClient per-endpoint parsing", () => {
  it("parses a hands body with the hands parser", async () => {
    const client = new MukanaClient(config, { fetch: respondWith(handsBody) });
    const outcome = await client.fetchHands();
    expect(outcome).toEqual({
      kind: "data",
      queue: { previous: [], current: "1383", upcoming: ["4242", "5555"] }
    });
    expect(client.healthFor("hands").state).toBe("ok");
  });

  it("does not mark a healthy plain-text hands response as failing", async () => {
    const client = new MukanaClient(config, { fetch: respondWith(handsBody) });
    await client.fetchHands();
    expect(client.healthFor("hands").consecutiveFailures).toBe(0);
    expect(client.nextDelayMs("hands")).toBe(2000);
  });

  it("reports a malformed hands body as invalid and counts a failure", async () => {
    const client = new MukanaClient(config, { fetch: respondWith("only one line") });
    const outcome = await client.fetchHands();
    expect(outcome.kind).toBe("invalid");
    expect(client.healthFor("hands").state).toBe("failing");
  });

  it("still treats the off-hours envelope on hands as dormant", async () => {
    const client = new MukanaClient(config, {
      fetch: respondWith(JSON.stringify({ status: 200, detail: "outside show hours" }))
    });
    const outcome = await client.fetchHands();
    expect(outcome).toEqual({ kind: "dormant", detail: "outside show hours" });
    expect(client.healthFor("hands").consecutiveFailures).toBe(0);
  });

  it("parses a question body with the question parser", async () => {
    const body = JSON.stringify({ q: { n: "Ann Lee", q: "Why?", v: 3, ts: 12, tag: "T", key: "k" } });
    const client = new MukanaClient(config, { fetch: respondWith(body) });
    const outcome = await client.fetchQuestion();
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.question?.askerName).toBe("Ann Lee");
    expect(client.healthFor("question").state).toBe("ok");
  });

  it("keeps using the panelist parser for panelists", async () => {
    const client = new MukanaClient(config, { fetch: respondWith(panelistsBody) });
    const outcome = await client.fetchPanelists();
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(Object.keys(outcome.db)).toEqual(["4242"]);
  });

  it("keeps endpoint health independent across the three parsers", async () => {
    const client = new MukanaClient(config, {
      fetch: async (url) => {
        if (url.includes("req=hands")) return { ok: true, status: 200, text: async () => "bad" };
        return { ok: true, status: 200, text: async () => panelistsBody };
      }
    });
    await client.fetchHands();
    await client.fetchPanelists();
    expect(client.healthFor("hands").state).toBe("failing");
    expect(client.healthFor("panelists").state).toBe("ok");
  });
});
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `npm run test --workspace show-engine`
Expected: FAIL — `fetchHands` returns a panelist-shaped outcome, and the plain-text hands
body is reported `invalid`.

- [ ] **Step 3: Implement**

Give `request()` a per-endpoint parser per the Behavior above.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS. Plan 2's existing `mukanaClient` tests stay green except where the brief's
new expectations supersede them — if an old assertion now contradicts a new one, the new one
wins and you should say so in your report.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/mukanaClient.ts show-engine/src/mukanaClient.test.ts
git commit -m "feat(show-engine): per-endpoint parse strategy for hands and question"
```

---

### Task 6: Tally derivation

**Files:**
- Modify: `show-engine/src/contracts.ts`, `show-engine/src/config.ts`
- Create: `show-engine/src/tallyPublisher.ts`
- Test: `show-engine/src/tallyPublisher.test.ts`, `show-engine/src/config.test.ts` (append)
- Fixture-only edits: `show-engine/src/lookDirector.test.ts`, `show-engine/src/directionPipeline.test.ts`

**Interfaces:**
- Consumes: `ProgramSource`, `Slot`, `GalleryCell` from `./contracts.js`; `LookResolution` from `./lookDirector.js`.
- Produces:
  - from `contracts.ts`: `LookDefinition` gains `tallySource: "boxes" | "activeSpeaker"`
  - from `tallyPublisher.ts`:
    - `type TallyMode = "none" | "slot" | "activeSpeaker" | "gallery" | "look"`
    - `type TallyState = { mode: TallyMode; onAirSlots: number[]; onAirPins: string[]; onAirParticipantIds: string[] }`
    - `function deriveTally(input: { source: ProgramSource; slots: readonly Slot[]; gallery: readonly GalleryCell[]; look: LookResolution | null; activeSpeakerSlot: number | null }): TallyState`
    - `function tallyEquals(a: TallyState, b: TallyState): boolean`

**Behavior:**

Tally answers one question for every panelist: *am I on air right now?* The legacy system
derived it by parsing the hardware switcher's full state and mapping source IDs back to
people. We are the switcher, so we derive it from what we already know.

`deriveTally` maps the program source to the slots visible on it:

| `source.kind` | mode | on-air slots |
|---|---|---|
| `black` | `none` | none |
| `slot` | `slot` | that slot, if it is occupied |
| `activeSpeaker` | `activeSpeaker` | `activeSpeakerSlot`, if non-null and occupied |
| `gallery` | `gallery` | every gallery cell whose `slot` is non-zero and occupied |
| `look` | `look` | depends on the look's `tallySource`, below |

For a `look` source with `tallySource: "boxes"` (the default): the host slot when the
resolution has one, the reader slot when it has one, and every box whose slot is non-null.
For `tallySource: "activeSpeaker"`: only `activeSpeakerSlot`. That second mode exists because
the legacy "Panel Checks" state showed the active speaker full-frame while the operator
checked panels — its own boxes were not on air, and telling those people they were live
would be wrong on air.

A `look` source with a `null` `look` resolution yields mode `look` and no slots — the
operator has selected a look the engine cannot resolve yet, and claiming anyone is live
would be worse than claiming nobody is.

Output rules, uniform across every mode:
- `onAirSlots` is ascending, deduplicated, and contains only slots that are **occupied** in
  the roster. A gallery cell or box pointing at an empty slot contributes nothing.
- `onAirPins` and `onAirParticipantIds` are parallel to `onAirSlots` in the same order.
  A seated panelist with a `null` PIN contributes to `onAirParticipantIds` and
  `onAirSlots` but **not** to `onAirPins` — they are on air, they simply have no PIN to
  publish. `onAirPins` is therefore not always the same length as the other two, and that is
  deliberate: an unregistered walk-in guest is genuinely on air.
- Out-of-range slot numbers are ignored rather than throwing. Tally derivation runs every
  tick from state other modules own, and a stale gallery cell must not take the engine down
  mid-show.

`tallyEquals` compares mode and all three arrays element-wise, for change detection. It is
the caller's tool for "has anything about who is live changed" — Plan 4 uses it to avoid
republishing.

Nothing here mutates its inputs; the returned arrays are fresh.

- [ ] **Step 1: Write the failing test**

`show-engine/src/tallyPublisher.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { deriveTally, tallyEquals } from "./tallyPublisher.js";
import type { GalleryCell, Panelist, Slot } from "./contracts.js";
import type { LookResolution } from "./lookDirector.js";

function panelist(id: string, pin: string | null): Panelist {
  return {
    participantId: id,
    rawName: id,
    online: true,
    videoOn: true,
    audioOn: false,
    handRaised: false,
    zoomRole: 3,
    displayName: id,
    location: "",
    pin,
    hasMukana: pin !== null,
    role: "panelist"
  };
}

const slots: Slot[] = [
  { slot: 1, panelist: panelist("host", "1383") },
  { slot: 2, panelist: panelist("reader", "2001") },
  { slot: 3, panelist: panelist("ann", "4242") },
  { slot: 4, panelist: null },
  { slot: 5, panelist: panelist("walkin", null) }
];

const gallery: GalleryCell[] = [
  { cell: 1, slot: 3 },
  { cell: 2, slot: 0 },
  { cell: 3, slot: 4 },
  { cell: 4, slot: 1 }
];

const look: LookResolution = {
  lookId: "hr",
  scenePreset: "scene-hr",
  plateTone: "accent",
  hostSlot: 1,
  readerSlot: 2,
  boxes: [
    { box: 1, slot: 3 },
    { box: 2, slot: null }
  ],
  nameplates: [],
  page: 0,
  pageCount: 1
};

function base(overrides: Partial<Parameters<typeof deriveTally>[0]>) {
  return deriveTally({
    source: { kind: "black" },
    slots,
    gallery,
    look: null,
    activeSpeakerSlot: null,
    ...overrides
  });
}

describe("deriveTally", () => {
  it("reports nobody on black", () => {
    expect(base({})).toEqual({
      mode: "none",
      onAirSlots: [],
      onAirPins: [],
      onAirParticipantIds: []
    });
  });

  it("reports the single slot on a slot source", () => {
    expect(base({ source: { kind: "slot", slot: 3 } })).toEqual({
      mode: "slot",
      onAirSlots: [3],
      onAirPins: ["4242"],
      onAirParticipantIds: ["ann"]
    });
  });

  it("reports nobody when the selected slot is empty", () => {
    const tally = base({ source: { kind: "slot", slot: 4 } });
    expect(tally.mode).toBe("slot");
    expect(tally.onAirSlots).toEqual([]);
  });

  it("ignores an out-of-range slot rather than throwing", () => {
    expect(() => base({ source: { kind: "slot", slot: 99 } })).not.toThrow();
    expect(base({ source: { kind: "slot", slot: 99 } }).onAirSlots).toEqual([]);
  });

  it("follows the active speaker", () => {
    expect(base({ source: { kind: "activeSpeaker" }, activeSpeakerSlot: 3 })).toEqual({
      mode: "activeSpeaker",
      onAirSlots: [3],
      onAirPins: ["4242"],
      onAirParticipantIds: ["ann"]
    });
  });

  it("reports nobody when there is no active speaker", () => {
    expect(base({ source: { kind: "activeSpeaker" } }).onAirSlots).toEqual([]);
  });

  it("reports every occupied gallery cell, ascending and deduped", () => {
    const tally = base({ source: { kind: "gallery" } });
    expect(tally.mode).toBe("gallery");
    expect(tally.onAirSlots).toEqual([1, 3]);
    expect(tally.onAirPins).toEqual(["1383", "4242"]);
  });

  it("reports chairs and filled boxes for a boxes look", () => {
    const tally = base({ source: { kind: "look", lookId: "hr" }, look });
    expect(tally.mode).toBe("look");
    expect(tally.onAirSlots).toEqual([1, 2, 3]);
    expect(tally.onAirPins).toEqual(["1383", "2001", "4242"]);
  });

  it("reports nobody for an unresolved look", () => {
    const tally = base({ source: { kind: "look", lookId: "hr" }, look: null });
    expect(tally.mode).toBe("look");
    expect(tally.onAirSlots).toEqual([]);
  });

  it("includes a seated panelist with no PIN in slots but not in pins", () => {
    const tally = base({ source: { kind: "slot", slot: 5 } });
    expect(tally.onAirSlots).toEqual([5]);
    expect(tally.onAirParticipantIds).toEqual(["walkin"]);
    expect(tally.onAirPins).toEqual([]);
  });

  it("does not mutate its inputs", () => {
    const cells = [...gallery];
    base({ source: { kind: "gallery" }, gallery: cells });
    expect(cells).toEqual(gallery);
  });
});

describe("tallyEquals", () => {
  it("matches identical states", () => {
    const a = base({ source: { kind: "slot", slot: 3 } });
    const b = base({ source: { kind: "slot", slot: 3 } });
    expect(tallyEquals(a, b)).toBe(true);
  });

  it("distinguishes different modes", () => {
    expect(
      tallyEquals(base({}), base({ source: { kind: "slot", slot: 3 } }))
    ).toBe(false);
  });

  it("distinguishes different slot sets", () => {
    expect(
      tallyEquals(
        base({ source: { kind: "slot", slot: 3 } }),
        base({ source: { kind: "slot", slot: 1 } })
      )
    ).toBe(false);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./tallyPublisher.js`.

- [ ] **Step 3: Implement**

Add `tallySource` to `LookDefinition` in `contracts.ts` (**required on the type**, optional
in config parsing where it defaults to `"boxes"` — this is exactly the `plateTone` precedent
already in `config.ts`). Add a config test for the default and for rejecting an unknown
value. Then write `tallyPublisher.ts` per the Behavior above.

**Expect this to break existing `LookDefinition` fixtures.** Plan 2's `lookDirector.test.ts`
and `directionPipeline.test.ts` build look objects without the new field, so they will fail
to typecheck. Add `tallySource: "boxes"` to those fixtures — a fixture-only edit, no
assertion changes. That is a necessary consequence of the type change, not scope creep;
note it in your report.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/contracts.ts show-engine/src/config.ts show-engine/src/config.test.ts show-engine/src/tallyPublisher.ts show-engine/src/tallyPublisher.test.ts
git commit -m "feat(show-engine): tally derivation from the program source"
```

---

### Task 7: Overlay director

**Files:**
- Create: `show-engine/src/overlayDirector.ts`
- Test: `show-engine/src/overlayDirector.test.ts`

**Interfaces:**
- Consumes: `MukanaQuestion` from `./contracts.js`; `LookResolution`, `Nameplate` from `./lookDirector.js`.
- Produces:
  - `type QuestionOverlay = { askerName: string; text: string; tag: string; votes: number }`
  - `type OverlayState = { nameplates: Nameplate[]; question: QuestionOverlay | null }`
  - `class OverlayDirector` with `state(): OverlayState`, `update(input: { look: LookResolution | null; question: MukanaQuestion | null; questionVisible: boolean }): boolean`, `reset(): void`

**Behavior:**

Derives everything textual that goes on screen: a nameplate under each occupied position,
and the current question when the operator has it up. This replaces the legacy SPX client —
CoreVideo Pro renders these through its own lower-third/overlay engine, so there is no
template name, no field payload, and no rundown transport.

`update` recomputes the state and returns **whether it changed**. That boolean is the whole
point of the class: Plan 4's orchestrator calls it every tick, and re-rendering an identical
lower third would restart its animation on air. Everything else about the module is
derivation.

- `nameplates` comes straight from `look.nameplates`, copied. A `null` look yields an empty
  array — no look resolved means nothing to label.
- `question` is `null` when `questionVisible` is `false`, when `question` is `null`, or when
  the question's `text` is empty after the parser's normalization. An empty question is not
  worth an overlay, and this is the module that gets to decide that (Task 4 deliberately
  passed it through).
- When shown, `QuestionOverlay` carries `askerName`, `text`, `tag`, and `votes` from the
  `MukanaQuestion`. It deliberately drops `key` and `timestampMs`: those identify and order
  the question, they are not drawn.
- Change detection compares the full derived state — every nameplate field and every
  question field — not object identity. Two structurally identical states are not a change.
  The first `update` after construction reports `true` when it produces anything non-empty,
  and `false` when it produces the empty state, because the empty state is what the director
  already had.
- `reset()` returns to the empty state. It reports nothing; a caller that needs to know
  should compare before and after. It exists for show teardown.

`state()` returns a deep copy — a caller mutating a returned nameplate cannot reach internal
state.

**Cross-task contract check (recorded during authoring):** `look.nameplates` is produced by
`resolveLook` (Plan 2) and carries `position`, `slot`, `name`, `location`, `tone` — already
everything a renderer needs, so this module passes them through rather than reshaping.
`MukanaQuestion` is produced by `parseMukanaQuestion` (Task 4). Both ends meet.

- [ ] **Step 1: Write the failing test**

`show-engine/src/overlayDirector.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import { OverlayDirector } from "./overlayDirector.js";
import type { MukanaQuestion } from "./contracts.js";
import type { LookResolution } from "./lookDirector.js";

const look: LookResolution = {
  lookId: "hr",
  scenePreset: "scene-hr",
  plateTone: "accent",
  hostSlot: 1,
  readerSlot: null,
  boxes: [{ box: 1, slot: 3 }],
  nameplates: [
    { position: { kind: "host" }, slot: 1, name: "J.J.", location: "CA", tone: "accent" },
    { position: { kind: "box", box: 1 }, slot: 3, name: "Ann", location: "TX", tone: "accent" }
  ],
  page: 0,
  pageCount: 1
};

const question: MukanaQuestion = {
  key: "-abc",
  askerName: "Douglas",
  text: "Why does it work?",
  tag: "General",
  votes: 4,
  timestampMs: 1635176445667
};

const hidden = { look: null, question: null, questionVisible: false };

describe("OverlayDirector", () => {
  it("starts empty", () => {
    expect(new OverlayDirector().state()).toEqual({ nameplates: [], question: null });
  });

  it("reports no change when the first update is empty", () => {
    expect(new OverlayDirector().update(hidden)).toBe(false);
  });

  it("publishes the look's nameplates", () => {
    const director = new OverlayDirector();
    expect(director.update({ ...hidden, look })).toBe(true);
    expect(director.state().nameplates).toEqual(look.nameplates);
  });

  it("clears nameplates when the look goes away", () => {
    const director = new OverlayDirector();
    director.update({ ...hidden, look });
    expect(director.update(hidden)).toBe(true);
    expect(director.state().nameplates).toEqual([]);
  });

  it("reports no change when nothing moved", () => {
    const director = new OverlayDirector();
    director.update({ ...hidden, look });
    expect(director.update({ ...hidden, look })).toBe(false);
  });

  it("detects a changed nameplate field", () => {
    const director = new OverlayDirector();
    director.update({ ...hidden, look });
    const [firstPlate, secondPlate] = look.nameplates;
    if (firstPlate === undefined || secondPlate === undefined) throw new Error("fixture");
    const renamed: LookResolution = {
      ...look,
      nameplates: [{ ...firstPlate, name: "J.J. Mc Kenna" }, secondPlate]
    };
    expect(director.update({ ...hidden, look: renamed })).toBe(true);
  });

  it("shows a question when visible", () => {
    const director = new OverlayDirector();
    expect(director.update({ look: null, question, questionVisible: true })).toBe(true);
    expect(director.state().question).toEqual({
      askerName: "Douglas",
      text: "Why does it work?",
      tag: "General",
      votes: 4
    });
  });

  it("hides the question when not visible", () => {
    const director = new OverlayDirector();
    director.update({ look: null, question, questionVisible: true });
    expect(director.update({ look: null, question, questionVisible: false })).toBe(true);
    expect(director.state().question).toBeNull();
  });

  it("shows no question when there is none", () => {
    const director = new OverlayDirector();
    expect(director.update({ look: null, question: null, questionVisible: true })).toBe(false);
    expect(director.state().question).toBeNull();
  });

  it("shows no question when its text is empty", () => {
    const director = new OverlayDirector();
    expect(
      director.update({ look: null, question: { ...question, text: "" }, questionVisible: true })
    ).toBe(false);
    expect(director.state().question).toBeNull();
  });

  it("detects a changed question", () => {
    const director = new OverlayDirector();
    director.update({ look: null, question, questionVisible: true });
    expect(
      director.update({
        look: null,
        question: { ...question, text: "A different question?" },
        questionVisible: true
      })
    ).toBe(true);
  });

  it("detects a changed vote count", () => {
    const director = new OverlayDirector();
    director.update({ look: null, question, questionVisible: true });
    expect(
      director.update({ look: null, question: { ...question, votes: 5 }, questionVisible: true })
    ).toBe(true);
  });

  it("ignores fields it does not draw", () => {
    const director = new OverlayDirector();
    director.update({ look: null, question, questionVisible: true });
    expect(
      director.update({
        look: null,
        question: { ...question, key: "-different", timestampMs: 999 },
        questionVisible: true
      })
    ).toBe(false);
  });

  it("empties on reset", () => {
    const director = new OverlayDirector();
    director.update({ look, question, questionVisible: true });
    director.reset();
    expect(director.state()).toEqual({ nameplates: [], question: null });
  });

  it("returns copies so callers cannot mutate internal state", () => {
    const director = new OverlayDirector();
    director.update({ ...hidden, look });
    const plates = director.state().nameplates;
    const first = plates[0];
    if (first !== undefined) first.name = "hacked";
    expect(director.state().nameplates[0]?.name).toBe("J.J.");
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — cannot resolve `./overlayDirector.js`.

- [ ] **Step 3: Implement**

Write `overlayDirector.ts` per the Behavior above.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/overlayDirector.ts show-engine/src/overlayDirector.test.ts
git commit -m "feat(show-engine): overlay director with change detection"
```

---

### Task 8: Exports and the outputs pipeline test

**Files:**
- Modify: `show-engine/src/index.ts`
- Test: `show-engine/src/outputsPipeline.test.ts`

**Interfaces:**
- Consumes: everything built in Tasks 1–7 plus the existing package.
- Produces: the package's public surface for the outputs layer.

**Behavior:**

`index.ts` gains: `shouldFollowSpeaker`; `MukanaQuestion`, `QuestionOutcome`,
`parseMukanaQuestion`; `clampPage`; `TallyMode`, `TallyState`, `deriveTally`, `tallyEquals`;
`QuestionOverlay`, `OverlayState`, `OverlayDirector`.

**Verify every export name against the actual source before writing the barrel.** If a name
here does not exist under that spelling, stop and report NEEDS_CONTEXT rather than renaming
anything in another module.

The integration test proves the outputs layer composes with the layers beneath it. Each
scenario must assert something that emerges only from the composition — not restate a unit
test through the barrel.

- [ ] **Step 1: Write the failing test**

`show-engine/src/outputsPipeline.test.ts`:

```ts
import { describe, expect, it } from "vitest";
import {
  buildPanelistDb,
  deriveTally,
  GalleryDirector,
  LiveSlots,
  OverlayDirector,
  OverrideDb,
  ProgramBus,
  clampPage,
  parseHandsPayload,
  parseMukanaQuestion,
  resolveLook,
  shouldFollowSpeaker,
  stripChairs,
  tallyEquals,
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
  "1383": { pin: "1383", displayName: "J.J. Mc Kenna", location: "CA", role: "host", online: true },
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
    ["z-ann", "4242"],
    ["z-asl", "7777"],
    ["z-bo", "5555"],
    ["z-host", "1383"],
    ["z-reader", "2001"]
  ] as const) {
    ingest.apply({ kind: "joined", participant: participant(id, `Name | ${pin}`) });
  }
  ingest.commit();
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
  plateTone: "accent",
  tallySource: "boxes"
};

describe("outputs pipeline", () => {
  it("puts exactly the people a look shows on air", () => {
    const slots = seatedRoster();
    const parsed = parseHandsPayload("5555\n4242\nNONE");
    expect(parsed.kind).toBe("data");
    if (parsed.kind !== "data") return;

    const queue = stripChairs(parsed.queue, { hostPin: "1383", readerPin: "2001" });
    const look = resolveLook(teatime, { queue, slots: slots.slots(), page: 0 });
    const tally = deriveTally({
      source: { kind: "look", lookId: "teatime" },
      slots: slots.slots(),
      gallery: [],
      look,
      activeSpeakerSlot: null
    });

    expect(tally.onAirPins.sort()).toEqual(["1383", "2001", "4242", "5555"]);
    expect(tally.onAirPins).not.toContain("7777");
  });

  it("keeps the ASL interpreter off air and out of the gate", () => {
    const slots = seatedRoster();
    const aslSlot = slots.slotOf("z-asl");
    const asl = aslSlot === null ? null : slots.slots()[aslSlot - 1]?.panelist ?? null;
    expect(asl?.role).toBe("aslinterpreter");

    expect(shouldFollowSpeaker(asl?.role ?? null, ["aslinterpreter"])).toBe(false);

    const bus = new ProgramBus();
    bus.setActiveSpeakerFollow(true);
    bus.onActiveSpeaker("z-ann", "panelist");
    bus.onActiveSpeaker("z-asl", asl?.role ?? "panelist");

    const tally = deriveTally({
      source: bus.state().program,
      slots: slots.slots(),
      gallery: [],
      look: null,
      activeSpeakerSlot: slots.slotOf("z-ann")
    });
    expect(tally.onAirParticipantIds).toEqual(["z-ann"]);
  });

  it("labels exactly the people it puts on air", () => {
    const slots = seatedRoster();
    const parsed = parseHandsPayload("5555\n4242\nNONE");
    expect(parsed.kind).toBe("data");
    if (parsed.kind !== "data") return;

    const queue = stripChairs(parsed.queue, { hostPin: "1383", readerPin: "2001" });
    const look = resolveLook(teatime, { queue, slots: slots.slots(), page: 0 });

    const overlay = new OverlayDirector();
    overlay.update({ look, question: null, questionVisible: false });

    const tally = deriveTally({
      source: { kind: "look", lookId: "teatime" },
      slots: slots.slots(),
      gallery: [],
      look,
      activeSpeakerSlot: null
    });

    expect(overlay.state().nameplates.map((plate) => plate.slot).sort()).toEqual(
      [...tally.onAirSlots].sort()
    );
  });

  it("survives a queue that shrinks under a paged look", () => {
    const slots = seatedRoster();
    const full = parseHandsPayload("5555,7777,1383\n4242\nNONE");
    expect(full.kind).toBe("data");
    if (full.kind !== "data") return;

    const page = clampPage(teatime, full.queue, 1);
    expect(() => resolveLook(teatime, { queue: full.queue, slots: slots.slots(), page })).not.toThrow();

    const drained = parseHandsPayload("NONE\n4242\nNONE");
    expect(drained.kind).toBe("data");
    if (drained.kind !== "data") return;

    const clamped = clampPage(teatime, drained.queue, page);
    expect(clamped).toBe(0);
    expect(() =>
      resolveLook(teatime, { queue: drained.queue, slots: slots.slots(), page: clamped })
    ).not.toThrow();
  });

  it("reports a tally change only when who is live actually changes", () => {
    const slots = seatedRoster();
    const gallery = new GalleryDirector({ cells: 4 });
    gallery.resetFromSlots(slots.slots());

    const first = deriveTally({
      source: { kind: "gallery" },
      slots: slots.slots(),
      gallery: gallery.cells(),
      look: null,
      activeSpeakerSlot: null
    });
    const again = deriveTally({
      source: { kind: "gallery" },
      slots: slots.slots(),
      gallery: gallery.cells(),
      look: null,
      activeSpeakerSlot: null
    });
    expect(tallyEquals(first, again)).toBe(true);

    gallery.remove(1);
    const after = deriveTally({
      source: { kind: "gallery" },
      slots: slots.slots(),
      gallery: gallery.cells(),
      look: null,
      activeSpeakerSlot: null
    });
    expect(tallyEquals(first, after)).toBe(false);
  });

  it("carries a parsed question through to the overlay", () => {
    const outcome = parseMukanaQuestion(
      JSON.stringify({ q: { n: "Douglas", q: "Why?", tag: "General", v: 2, ts: 1, key: "k" } })
    );
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;

    const overlay = new OverlayDirector();
    expect(overlay.update({ look: null, question: outcome.question, questionVisible: true })).toBe(
      true
    );
    expect(overlay.state().question).toEqual({
      askerName: "Douglas",
      text: "Why?",
      tag: "General",
      votes: 2
    });
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npm run test --workspace show-engine`
Expected: FAIL — several names are not exported from `./index.js`.

- [ ] **Step 3: Write the barrel additions**

Extend `index.ts` per the Behavior above.

- [ ] **Step 4: Run the full suite, typecheck, and build**

Run: `npm run test --workspace show-engine && npm run typecheck --workspace show-engine && npm run build --workspace show-engine`
Expected: PASS, no type errors, `dist/` emitted with declarations. Every Plan 1 and Plan 2
test must still pass.

- [ ] **Step 5: Commit**

```bash
git add show-engine/src/index.ts show-engine/src/outputsPipeline.test.ts
git commit -m "feat(show-engine): export the outputs layer, prove it composes"
```

---

## Definition of Done

- [ ] `npm run test --workspace show-engine` passes, including every Plan 1 and Plan 2 test.
- [ ] `npm run typecheck --workspace show-engine` is clean.
- [ ] `npm run build --workspace show-engine` emits `dist/` with declarations.
- [ ] The package remains registered in the root `workspaces` array **and** in the `test:gate` chain.
- [ ] All four Plan 2 contract gaps are closed: the shared speaker gate (Task 1), `galleryCells` (Task 2), `clampPage` (Task 3), and the per-endpoint parse strategy (Tasks 4–5).
- [ ] Every new module carries its header comment and has an adjacent test file.
- [ ] All work is committed on `plan/ohg-show-engine-outputs`.

## Spec corrections this plan requires

These are documentation edits, not code. Fold them into the final task's commit or a
follow-up — but do not skip them, because Plans 5–7 implement against the spec on other
platforms.

- **§3.7** — the skip-roles rule does not live in `speakerRecency`. Replace that sentence
  with: the rule is a shared predicate (`shouldFollowSpeaker`) applied at dispatch by every
  consumer of an active-speaker event; `ProgramBus` applies it internally and the
  orchestrator applies it before fanning the event out to any position assigner.
- **§3.7** — "three strategies behind one interface" is inaccurate: `RecencyScores`
  deliberately does not implement `PositionAssigner`, because it orders rather than places.
- **§5** — the normative shapes have drifted from `contracts.ts`. `QueueState` is
  `{ previous, current, upcoming }`, not `{ prev, current, next }`. `Look` is
  `LookDefinition` and carries `label`, `includesHost`, `plateTone`, and `tallySource`.
- **§4.2** — `ohg.gfx.rundown` is an SPX transport concept with no referent in CoreVideo
  Pro's overlay engine. Remove it; `ohg.gfx.headline.*` and `ohg.gfx.question.*` survive as
  the operator's show/hide controls, which map to `overlayDirector`'s `questionVisible`.
- **§3.13** — the config inventory should list `galleryCells`, `skipRoles`, and `looks`.

## What Plan 4 picks up

The `HostAdapter` port with its mock and conformance suite, and the orchestrator that owns
the polling loop, debounced persistence, and the `ohg.*` control-action surface. The
orchestrator is the first component that composes *everything*: it drives `MukanaClient`'s
three endpoints on their own cadences, gates active-speaker events through
`shouldFollowSpeaker` before dispatch, clamps look pages with `clampPage`, publishes tally
only when `tallyEquals` says it changed, and re-renders overlays only when
`OverlayDirector.update` returns `true`.

Also carried forward, unchanged from Plan 2's outcomes: extract the duplicated recency
helpers in `speakerRecency.ts` before a fourth strategy arrives; decide whether
`ProgramBus.onActiveSpeaker` should take `Role | null` rather than `Role` (an unseated
speaker has no role, and defaulting to `"panelist"` would be a lie — note `shouldFollowSpeaker`
already accepts `null` for exactly this reason); and consider enabling
`noUncheckedIndexedAccess`.
