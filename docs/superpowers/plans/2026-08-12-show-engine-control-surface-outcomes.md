# Show Engine — Plan 6 Outcomes

Companion to `2026-08-12-show-engine-control-surface.md`. Written at the close of Plan 6
execution (2026-08-16).

**Shipped:** the operator surface. 28 `ohg.*` actions with positional typed params dispatched
onto `ShowEngine`; a pure projection of `ShowSnapshot` into flat scalar feedback fields; and an
**exported** conformance suite Plans 7–9 run against their real adapters. Plus two move-only
carve-outs taken *before* the new surface (`MukanaPoller`, `LookController`), hysteresis and an
abort contract for hung endpoints, `applyLook` widened to carry `scenePreset` and both chairs, an
operator-driven headline, and roster/gallery operator methods including smart-gallery ordering.

915 tests; typecheck, `typecheck:tests`, build, and `verify:barrel` all green.

## The correction this plan is built on

The parent design spec §4.1 claims the host "proxies `ohg.*` invokes to the engine subprocess and
merges engine state into `ControlState` under an `ohg` node." **That mechanism does not exist.**
`ControlActionRegistry` is a hardcoded static list, `StudioControlSurface` dispatches through a
compile-time `switch`, and a coverage test enforces the two stay a closed 1:1 set. There is no
dynamic registration and `ControlState` has no extensible subtree.

Owner decision, 2026-08-12: the engine owns the action *semantics*; each shell bridges them.
**Plan 7 must add dynamic or proxied registration to `CoreVideoPro.Control`** — which today is a
closed compile-time registry — plus an extensible `ControlState` subtree. That is real work and it
is not yet started.

## Decisions worth not re-litigating

- **PINs and participant ids are `string` params, not `int`.** A 4-digit PIN `"0042"` coerces to
  `42`, and `personKeyForPin("42")` is a *different person* — a silent identity swap putting the
  wrong name and role on air. The leading-zero test is what makes this load-bearing.
- **`ProgramSource` encodes as one prefixed string** (`black` / `gallery` / `activeSpeaker` /
  `look:<id>` / `slot:<n>`), with `parseProgramSource`/`formatProgramSource` round-tripping in both
  directions, because the projection publishes the formatted form.
- **`invokeAction` never throws and never mutates before validating.** A malformed OSC packet from
  a Companion button must not take down a live show. Verified by a 1350-invoke adversarial sweep
  and a 30-invoke never-mutates sweep.
- **The abort deadline is `MUKANA_HUNG_POLL_INTERVALS × interval`, not 1×.** At 1× the abort fired
  before the hung rule could engage, making the whole hysteresis mechanism unreachable for any
  conforming host. The package holds exactly one timer — `AbortSignal.timeout` in `MukanaClient`,
  unref'd, documented at three sites.
- **`lastMukanaPollAt` is a due anchor only.** Overloading it to also mean "when this poll started"
  is what let a forced sync mark an in-flight poll hung. `pollInFlightSince` now carries the start
  time, so "busy without a start time" is unrepresentable.

## The bug worth remembering

**Pressing "resync" degraded a healthy feed.** `syncAll` set `lastMukanaPollAt = -Infinity` to force
endpoints due — and `detectHungEndpoints` read that same field to compute how long a poll had been
outstanding. An endpoint with a fetch in flight got `outstandingMs = Infinity` and was marked hung:
health `failing`, capability `unavailable`, the look degraded to manual box fill, paging refusing,
lamp red, and two healthy settles of hysteresis required to recover.

It survived all ten task gates because **every fixture settled its fetch immediately**, so no test
ever had an in-flight poll when sync fired. It took the whole-branch review to find, and it is
operator-reachable by a button labelled "sync".

## Carried into Plan 7

**Obligation:** wire the bridge. `CoreVideoPro.Control` needs dynamic or proxied registration and an
extensible `ControlState` subtree before any `ohg.*` action is reachable from Companion or OSC.

**Security note, unprompted but real:** OSC carries **no auth token** today — only loopback-vs-LAN
gating. Every `ohg.*` action reachable over OSC is therefore reachable unauthenticated on the LAN,
including the ones that move people on air.

**The conformance suite's reach is narrower than its name**, and this is documented in its header
rather than left implied: it pins the engine→host *call* contract and fails a call-dropping host,
but it asserts on what a recording facade *received* — an adapter that ignores `scenePreset` or
lies in `capabilities()` still passes. Nine of 28 actions are invoked; `projectControlFields`,
`OHG_FIELD_TEMPLATES`, and `oscAddressFor` have zero conformance coverage. Plans 7–9 still owe
per-shell adapter conformance.

**Deliberate carries, each recorded with a ruling in the ledger:** `setSmartGallery` is published
and readable but not persisted (a restart reverts it to off, having possibly persisted a frozen
smart-ordered arrangement); the per-tick `RecencyScores.order()` is unconditional while the toggle
is on; `GalleryDirector.assertSlot` validates only "non-negative integer", so `replaceGalleryCell`
accepts a slot matching no occupied roster slot; int rounding diverges from C#'s banker's rounding
at exact midpoints (comment corrected, behavior kept); `TallyState`'s parallel-array zip hazard is
latent since only `onAirSlots` is read; `mukanaClient.ts`'s `applyHealth` doc claim is still stale.

**Informational, from the final re-review:** releasing the canonicalizer's ancestor set makes it
exponential on a shared DAG (1ms / 27ms / 321ms at sharing depth 12 / 16 / 20). Irrelevant for the
flat call records the suite compares, and the correctness fix was worth it.

## Process note

**`showEngine.ts` is 1414 at HEAD versus 1383 at base — net *up* by 31.** The plan asserted the net
direction would be down, and the check ran once at Task 7 (1362) and was never re-run after Tasks 8
and 10. The ledger's "net down" claim was stale and has been corrected. The structural direction is
genuinely good — 626 lines now live in two separately testable classes — but a claim that is only
verified once, mid-plan, is not a claim.

**Rule 10 recurred five times in this plan alone**, and a sixth variant appeared:

1. A loose `toMatch(/no response/)` accepted a *less* informative operator diagnostic after a
   move-only task collapsed it.
2. A hand-maintained id list could not see a deleted `switch` case.
3. `CAPABILITY_NAMES` fed *both* sides of a drift check, so shrinking it shrank both together.
4. The conformance suite's own equality used `JSON.stringify` and so was property-order sensitive —
   failing a semantically correct adapter facade, which is precisely what it exists to serve.
5. A fix added a boundary (`assignBox` slot validation) and tested only the rejection half;
   changing `slot < 0` to `slot < 1` left all 882 tests green.
6. And in the fix wave: the whole-branch review's own proposed one-liner for the never-throws fix
   **did not work** — `describeArg` was itself the thrower, since `String(Object.create(null))`
   throws.

Two rules were added to `docs/superpowers/plan-authoring-rules.md` during execution: **rule 9** (a
new contract needs a fixture that honors it — Task 3 shipped an abort contract in which every
fixture ignored the signal) and **rule 10** (a structural test must exercise the structure, not
describe it).

**What actually caught things.** The plan operationalized rule 7 by putting a required "Mutations to
run" block in every task, and implementers ran them unprompted and reported signatures — which let
reviewers spend their attention elsewhere. But **every Critical and nearly every Important came from
a mutation nobody listed**: the collapsed diagnostic, the 1×-vs-3× abort deadline, the missing dirty
flag, the `bigint` escape, the order-sensitive comparator, the sync-marks-hung collision. Named
mutations verify the properties a plan already knew to care about; adversarial reviewers find the
ones it didn't. Keep both.

**Three times an implementer found a defect in a check that a plan or a reviewer had prescribed** —
the insufficient 1:1 coverage test, the value-based normalizer pin, and the never-throws one-liner.
That is rule 7 working in the direction nobody designs for: the mutation requirement surfaces broken
*guards*, not just broken code.
