# OHG Show Engine — Plan 3 Outcomes

Companion to `2026-08-05-ohg-show-engine-outputs.md`. Written at the close of Plan 3
execution (2026-08-05).

**Shipped:** the four Plan 2 contract gaps closed, plus the outputs layer —
`tallyPublisher` (who is on air) and `overlayDirector` (what text is on screen), with
`parseMukanaQuestion` and barrel exports. 342 tests, typecheck and build clean. Five
instances of spec drift corrected in the design spec.

## One claim to correct before Plan 4 reads it

**Gap #2 is made *closable*, not closed.** The shared `shouldFollowSpeaker` predicate
exists and `ProgramBus` applies it, but **nothing in the package yet calls it before a
position assigner.** The concrete defect it was created to fix — an ASL interpreter
evicting a panelist from the FILO pool, swapping into the visible gallery window, and
sorting to the front of the gallery — stays live until Plan 4 wires the dispatch gate in
the orchestrator.

Plan 4 must include an orchestrator test proving an interpreter speaking produces **no
`PlacementChange`** from either assigner. Without it the accessibility rule is half-built,
and the half that is missing is the half that moves pictures.

## Decisions worth not re-litigating

- **`LookResolution` carries `tallySource`,** copied by `resolveLook` from the definition
  alongside `scenePreset` and `plateTone`. The alternative — passing `deriveTally` both a
  definition and a resolution — would force every caller to supply two objects that must
  agree, which is the cross-module mismatch class Tasks 4–5 spent their effort removing.
- **The hands wire format is the three-line text form.** Settled against the raw patch
  extraction in Plan 2's outcomes; do not revisit without new evidence.
- **`parseHandsPayload` now validates strictly** (each line is `NONE` or a comma-separated
  list of 4-digit PINs; anything else is `invalid`). A 200 carrying an HTML error page
  previously yielded a healthy-looking empty queue — indistinguishable on air from every
  hand being lowered. Human ruling, 2026-08-05.
- **A current-speaker line with more than one PIN is `invalid`,** not first-wins.
  `QueueState.current` is singular by design and no consumer ever comma-splits it.
- **`clampPage` is the orchestrator's tool; `resolveLook` still throws.** Two callers, two
  contracts, documented on both functions. Do not unify them.
- **`OverlayDirector` clones on both ingest and egress.** Both are load-bearing: the
  ingest clone also prevents a caller mutating a `LookResolution` afterwards from silently
  defeating change detection. A per-task review called the second one redundant; the final
  review overturned that. Do not remove either.

## Carried into Plan 4

**Parked with a ruling:** `mukanaClient.ts`'s doc comment claims `applyHealth` is the only
writer of health records. It is not — `fail()` still assigns `this.state[endpoint]`
directly. The path the review actually named *is* routed through `applyHealth`, so this is
a false comment rather than false behavior. Fix by softening the comment or routing
`fail()` through `applyHealth` too.

**Decide before building the `ohg.*` action surface:** spec §4.2 keeps
`ohg.gfx.headline.in()/.out()/.change(name, location)`, but `OverlayState` is
`{nameplates, question}` with no headline concept. Either `OverlayDirector` grows a third
element (an operator-driven headline with its own visible flag, parallel to
`questionVisible`), or the spec records the headline as host-side and not engine state.
Cheap now, expensive once the action surface is half-built.

**Settle before anything consumes it:** `TallyState` exposes three parallel arrays where
`onAirPins` may be shorter than the other two (a seated walk-in with no PIN is genuinely on
air). Consider a single `onAir: { slot, participantId, pin: string | null }[]` with derived
convenience lists — it removes the zip hazard. After Plan 4 this is a breaking change
across three host adapters.

**Smaller items:** `ProgramBus.onActiveSpeaker(role: Role)` vs
`shouldFollowSpeaker(role: Role | null)` — the orchestrator will routinely hold an unseated
speaker and forcing it to invent `"panelist"` is a lie; give `deriveTally`'s
`activeSpeakerSlot` a named helper so `slots.slotOf(bus.state().activeSpeakerId)` is not
recomputed three ways; `deriveTally` never checks `look.lookId === source.lookId`, so a
stale resolution paired with a freshly-switched source reports the wrong people live
(treating a mismatch as unresolved is two lines and matches the existing `look === null`
handling); three error idioms now coexist (prefixed message strings, bare `Error`, named
error classes) and the `ohg.*` surface will have to render all three to an operator; spec
§3.11 still says tally "publishes to oh.tally.video" though nothing in the module publishes
anywhere; `parseLine` also rejects a stray trailing comma, which is untested and worth
watching in shadow mode.

Still open from Plan 2: extract the duplicated recency helpers in `speakerRecency.ts` before
a fourth strategy arrives, and consider `noUncheckedIndexedAccess`.

## Process note

The no-implementation-bodies format held up a second time: **zero defects traced to
transcription**, across two plans now.

The defect class has moved again — from transcription (Plan 1), to cross-task contract
mismatch (Plan 2), to **Interfaces blocks that describe the producer without checking the
consumer**. Both plan-level misses this round have that exact shape: `tallySource` existed
on `LookDefinition` but `deriveTally` consumed `LookResolution`; `TallySource` was produced
by `contracts.ts` but no export let a consumer outside the package name it. Plan 2's
outcomes recommended a cross-task contract pass and this plan ran one — it was not enough,
because it asked "do the types meet?" rather than "can the consumer reach it?"

Two refinements for Plans 4–8:

1. **Run each Interfaces block against the consuming call site**, not just the producing
   module. "Producer has it, consumer cannot reach it" caught both misses.
2. **For integration tests, state the invariant each scenario must break** if the
   composition is wrong ("this test must fail if `ProgramBus` stops gating") rather than
   only supplying the code. This plan supplied a test body *and* a prose rule; the
   implementer copied the body faithfully, the body did not satisfy the prose, and the
   prose lost. A supplied test body is copied including its blind spots.
