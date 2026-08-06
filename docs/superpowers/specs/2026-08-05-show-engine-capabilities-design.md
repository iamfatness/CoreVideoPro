# Show Engine — Capabilities Design Spec

**Date:** 2026-08-05
**Status:** Approved design, pre-implementation
**Amends:** `2026-08-04-ohg-show-engine-design.md` (the parent design spec)

## 1. What this is

The show engine was designed around Office Hours Global, whose panelist registry
("Mukana") supplies identity, editorial roles, the raised-hands queue, and the current
audience question. Other shows use Mukana too, so it stays a first-class configurable
integration — but **a show must survive without it**, both because some shows never had it
and because a third-party question-and-chat service must never be able to take a live
production down.

This spec adds a capability model: each optional input resolves to an effective state from
what config declares and what the integration's live health reports, and every consumer
branches on that rather than on configuration. It also closes the two places where the
engine is structurally coupled to Mukana rather than merely enriched by it.

### Goals
- A show with no registry keeps the roster, gallery, speaker follow, tally, nameplates,
  program bus, and looks.
- A registry outage mid-show degrades the production to exactly the registry-less
  behavior, automatically, without operator intervention.
- The operator can always tell the difference between "this show doesn't do that" and
  "that stopped working."

### Non-goals
- Making Mukana optional at the *code* level — it remains a supported integration with a
  real client; only its *presence* becomes optional.
- A general plugin or dependency-injection framework. Two named strategies at the two
  points that need different behavior; nothing more.
- Automatic reclaim of operator decisions when an integration recovers (§4).

## 2. The capability model

Three optional inputs, resolved independently:

| Input | Feeds | Behavior when absent |
|---|---|---|
| `registry` | display name, location, editorial role, PINs | identity falls back to parsing the Zoom display name (already built) |
| `handsQueue` | look guest boxes and paging | manual box fill (§3.2) |
| `questionFeed` | the question lower third | no question overlay; nameplates unaffected |

Each resolves to one of three effective states:

- **`available`** — configured **and** its integration reports healthy
- **`unavailable`** — configured but currently failing or dormant
- **`disabled`** — not configured for this show

**The load-bearing property: `unavailable` and `disabled` produce identical downstream
behavior.** Every consumer asks "can I use this?", never "is this configured?". A mid-show
outage therefore makes the show behave exactly as a show that never had the integration —
which converts an incident into a known state.

They differ in exactly one respect: what the operator is told (§4).

Resolution is a pure function over state the engine already holds:

```ts
resolveCapabilities(config: ShowEngineConfig, health: MukanaHealthByEndpoint): ShowCapabilities
```

No new I/O and no new polling. `MukanaClient.healthFor(endpoint)` already reports
`ok | dormant | failing` per endpoint and maps directly: `ok` → `available`,
`failing` → `unavailable`, `dormant` → `unavailable`. Dormant resolving to `unavailable` is
correct rather than a compromise — outside show hours the registry genuinely is not there,
and the show should behave as though it has none until it appears.

```ts
type CapabilityState = "available" | "unavailable" | "disabled";
type Capability = { state: CapabilityState; detail: string | null };
type ShowCapabilities = {
  registry: Capability;
  handsQueue: Capability;
  questionFeed: Capability;
};
```

`detail` is populated only when `unavailable`, carrying the reason the client already
produced (`"HTTP 503 from hands"`, `"outside show hours"`). No new message vocabulary.

**Hard rule, normative:** no external integration failure may block a tick. The orchestrator
resolves capabilities and proceeds. It never awaits a recovery, never retries inline, and
never lets a fetch outcome gate the render path. The polling clients back off on their own
schedule; the show loop reads whatever the last successful poll left behind.

## 3. Closing the two structural couplings

### 3.1 Role identity resolves through a chain

`OverrideDb` is keyed by PIN today, and the panelist join only consults it when a PIN
exists. PINs come from the Mukana registration convention, so **a registry-less show cannot
assign a host or reader at all** — which cascades into chair resolution, nameplates, tally,
and the active-speaker skip gate.

Roles become keyed by a `PersonKey`, resolved the way identity already resolves:

```
pin (when present)  →  normalized display name  →  participantId
```

Normalization lowercases, collapses whitespace, and strips any PIN token, so
`"Ann Lee | 4242"` and `"ann lee"` resolve alike — people retype their names when they
rejoin. A name that normalizes to the empty string is treated as absent and falls through
to `participantId`, so a blank or PIN-only display name never produces a shared key that
would silently merge two people's roles. The key is computed once in `panelistDb` and
carried on the `Panelist`, so every consumer reads the same key rather than re-deriving
it.

Accepted consequences, stated plainly:
- Two guests genuinely sharing a display name in a registry-less show collide and share a
  role.
- A host who renames themselves mid-show loses their role; the operator reassigns.

Both are visible and recoverable; neither is silent. In a registry-backed show PINs win and
neither can occur.

`assignExclusiveRole` continues to enforce single-host/single-reader across both the
override table and the registry. With no registry it enforces across the override table
alone, which is already its behavior with an empty registry — the guarantee is unchanged.

### 3.2 Guest boxes gain a named fill strategy

`LookDefinition` gains `boxFill: "queue" | "manual"`.

- **`"queue"`** — fill from the hands queue, with paging. Requires `handsQueue` available;
  **falls back to `"manual"` when it is not**, rather than emptying the boxes.
- **`"manual"`** — the operator assigns a roster slot per box. Always available.
  `pageCount` is 1 and paging actions are inert.

A look declares its preferred strategy; the resolver picks the effective one. Manual
assignments live in engine state, survive a tick, persist alongside the gallery, and are
cleared when the look changes.

Manual is the always-available mode precisely so `"queue"` has somewhere to degrade into.
If manual were itself optional, degradation would land on empty boxes — the failure this
design exists to prevent.

## 4. Surfacing and control

Capabilities publish as engine state so every surface renders from one source:

```ts
capabilities: { registry: Capability; handsQueue: Capability; questionFeed: Capability }
```

**Operator visibility:**
- `disabled` → the control is **hidden**. A show that never had hands should not carry a
  dead paging control forever.
- `unavailable` → the control stays **visible but inert**, with `detail` attached. A greyed
  "next guest" reading *"hands feed unreachable — 503"* tells the operator the show is
  degraded and to place guests manually. Hiding it would tell them nothing while they are
  live.

**Action surface:**
- `ohg.look.nextGuest` / `.prevGuest` return a typed refusal when the effective fill is
  `manual` — not a silent no-op, not a throw.
- New: `ohg.look.box.assign {box, slot}` and `ohg.look.box.clear {box}`.
- Companion and OSC pick these up from the manifest automatically, as with every other
  action.

**No automatic reclaim.** When an integration recovers, its capability returns to
`available` and a look's preferred `"queue"` strategy resumes on the next tick — but manual
box assignments the operator made are **kept** until they change the look. Yanking the
picture back the instant a third-party service recovers is the kind of surprise that makes
operators distrust automation mid-show.

## 5. Testing

**The load-bearing test is a property, not a case.** For any roster, look, and program
state, engine output with a capability `unavailable` must be identical to output with that
capability `disabled`. Parameterized across the three capabilities, this guards the actual
guarantee — if it fails, a mid-show outage stops behaving like a registry-less show and the
third-party risk is back.

Also:
- Table-driven resolution over config × health, including dormant → `unavailable`.
- Role-key chain across reconnect (same display name) and rename (role lost, recoverable).
- `"queue"` falling back to `"manual"` when `handsQueue` is unavailable, and boxes never
  emptying as a result.
- One integration scenario that kills the registry mid-tick and asserts the picture, the
  roster, and tally all survive intact.

## 6. Where this lands

**A new Plan 4, ahead of the orchestrator** (which becomes Plan 5; the series grows to
nine). The ordering is deliberate:

- The orchestrator must resolve capabilities before it can gate anything.
- The action surface needs `box.assign` / `box.clear` defined before it registers actions.
- Re-keying roles touches `overrideDb` and `panelistDb`, far cheaper now — while their only
  consumers are inside the package — than after three host adapters read them.

It stacks as a new branch rather than amending PRs #363–365. Those are already under
review; rewriting them would invalidate that review, and the re-key is a clean forward
change.

**Breaking changes this introduces**, both to types Plans 6–9 will implement against on
other platforms, and both cheapest now because those plans do not yet exist:
- `LookDefinition` gains `boxFill`.
- `OverrideDb` changes its key type from PIN to `PersonKey`.

## 7. Amendments to the parent spec

- **§3.4** — `overrideDb` is keyed by `PersonKey`, not PIN.
- **§3.5** — `panelistDb` computes and carries `personKey` on each `Panelist`.
- **§3.9** — `lookDirector` honors `boxFill`, and `handsQueue` is an optional input.
- **§4.3** — the state snapshot gains a `capabilities` node.
- **§4.2** — add `ohg.look.box.assign` and `ohg.look.box.clear`; note that
  `nextGuest`/`prevGuest` refuse under manual fill.
- **§7** — record that Mukana is optional at runtime, and that no integration failure may
  block a tick.

## 8. Decisions log

- Mukana stays a supported, configurable integration used by multiple shows; shows must
  survive without it, and a third-party question/chat service must never risk a production
  — jwallace, 2026-08-05.
- `unavailable` and `disabled` are behaviorally identical downstream, differing only in
  what the operator is told — jwallace, 2026-08-05.
- Role identity resolves pin → normalized display name → participantId, accepting
  same-name collision and rename-loses-role in registry-less shows — jwallace, 2026-08-05.
- Guest boxes fill manually when there is no queue; manual is always available and is what
  `"queue"` degrades into — jwallace, 2026-08-05.
- Operator decisions are not automatically reclaimed when an integration recovers —
  jwallace, 2026-08-05.
