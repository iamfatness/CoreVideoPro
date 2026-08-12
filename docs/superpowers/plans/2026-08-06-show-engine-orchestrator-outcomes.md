# Show Engine — Plan 5 Outcomes

Companion to `2026-08-06-show-engine-orchestrator.md`. Written at the close of Plan 5
execution (2026-08-12).

**Shipped:** the first composition in the package. `ShowEngine` owns every module instance,
an injected `Clock`, and one `HostAdapter`; it ticks in a fixed order, publishes one
deep-copied `ShowSnapshot`, and emits host commands only for what changed. With it: the
`clock.ts` seam, the `hostAdapter.ts` port plus a recording `MockHost` shipped in `src/`,
`showSnapshot.ts`, `hostCommands.ts`, and persisted state at v3. 603 tests; typecheck,
`typecheck:tests`, and build all clean.

**All three carried obligations are discharged, each proven by mutation:**

- **The ASL-interpreter gate.** `shouldFollowSpeaker` had existed for two plans with nothing
  calling it before a position assigner. It is now gated in `tick()` after the panelist
  rebuild, quantified over 5 roles × 2 assigner types × follow on/off. Removing the gate, or
  moving it below the dispatch, reds the interpreter cases on the *assigner* assertions.
- **The `clampPage`/`resolveLook` capability pairing.** Both calls share one per-tick
  capability value. The property ranges over page −2..4 × 3 capability states × 2 fill
  strategies.
- **`ManualBoxAssignments` in persisted state**, both directions — restore *and* write.

## The bug worth remembering

`ZoomIngest` deliberately keeps a departed participant, marked offline, "so they can be
restored on reconnect". This plan's brief nonetheless asserted that a departure leaves a
*hole*. The only way to satisfy that test was to delete the seat — which made a reconnecting
guest **permanently unseatable**, because a departure never changes the participant id set, so
the tick takes the `refresh` path forever after and `refresh` only re-pulls already-seated
panelists. An offline ghost could also take a low slot on the next `rebuild` and then be
deleted, stranding a live guest in `unseated` beside an empty seat.

Owner ruling, 2026-08-06: **a Zoom departure does not vacate a seat.** The seat is held and
flagged offline, so a connection blip cannot drop a panelist off air and a reconnect returns
them to the same slot. Clearing a seat is an explicit operator action, as it was in the
Isadora patch this ports.

## Decisions worth not re-litigating

- **`PersistedShowState` and `ShowSnapshot` are different types with different lifetimes.**
  The persisted subset was renamed with no alias left behind, specifically so the two cannot
  silently re-merge.
- **`clampPage` and `resolveLook` both take an *optional* capability defaulting to unusable.**
  The design is symmetric and the default fails safe. The trap — passing `available` to one
  and omitting it on the other — is closed by the two calls being adjacent and sharing one
  per-tick value, and by the property that now ranges over both.
- **The tick gates the active speaker, not the intake.** Role resolution needs the panelist
  database; at intake it is one tick stale and empty at startup, so an interpreter's first
  event would resolve to a `null` role and pass.
- **`ProgramBus.onActiveSpeaker` takes `Role | null`.** Plan 3's outcomes predicted that
  forcing the orchestrator to invent `"panelist"` for an unseated speaker "is a lie"; this plan
  specified the coalesce anyway and the predicted failure appeared — under
  `skipRoles: ["panelist"]` the engine's gate approved a dispatch that ProgramBus's internal
  gate silently vetoed. Do not reintroduce a fabricated role.
- **A poll never blocks a tick, and a busy gate prevents overlap.** Without the gate, a stale
  outcome overwrote a fresher one (the hands queue jumping *backwards* on air) and hung fetches
  accumulated unbounded — 20 concurrent over 20 ticks, with backoff never engaging because a
  hung fetch never fails.
- **`restore()` discards rather than throws** on a stale `lookId` or a geometry that no longer
  matches config, warns through `ShowSnapshot.restoreWarnings`, and self-heals to disk. Those
  are legitimate operator edits, not file corruption; genuine incoherence still throws.

## Carried into Plan 6

**Obligation, not a suggestion.** Plan 5 closed the case where a hung endpoint reported
`available` forever — which had made the manual-box fallback unreachable on air — by degrading
a poll outstanding more than `MUKANA_HUNG_POLL_INTERVALS` (3) times its interval. That fix
**false-positives and has no hysteresis**: an endpoint answering correctly after ~6.5 s flips
`boxFill` `queue → manual → queue`, and one hovering near the threshold oscillates every poll
cycle. It is unreachable today because nothing wires a live `FetchLike`. Before any host does:

1. Ship the `AbortSignal.timeout` on the injected `FetchLike` — a timeout makes a hung endpoint
   fail *cleanly* rather than hover, which is the real fix.
2. Add hysteresis so a recovered endpoint does not flap the guest boxes.
3. Pin the constant's lower bound — `3 → 1` currently leaves 603/603 green.

**Structural, for early Plan 6.** `showEngine.ts` is ~1100 lines (552 code, 548 comment). The
review judged that fine to carry but named two genuine move-only carve-outs: a `MukanaPoller`
and a look/paging controller. Do them before adding an action surface, not after.

**Plan 6's first design decision:** `applyLook`'s box map has no representation for
`hostSlot`/`readerSlot`, and `scenePreset` has the same gap — so each of three adapters would
re-derive `lookId → scenePreset` in Swift, C#, and C++. It is spec-faithful today (design spec
§6 declares this signature), which is why it was not changed here.

**Smaller, all parked with rulings in the ledger:** a `"range"` paging refusal survives a
fill-strategy flip and then contradicts its own snapshot; a `"fill"` refusal recorded between
ticks is cleared before publication if the feed recovers; `emitGallery`'s diffing is never
exercised through `tick()` because no operator API mutates gallery cells; the assigner's
`positions()` output is never read; nothing renders `restoreWarnings` yet; the restore warning's
text claims "no look is selected", which is false when `setLook()` preceded `restore()`.

**Still owed from Plan 3:** the `mukanaClient.ts` doc comment claims `applyHealth` is the sole
health writer while `fail()` also writes directly. Spec §4.2's `ohg.gfx.headline.*` actions
still have no counterpart in `OverlayState` — decide before building the action surface.
`TallyState`'s three parallel arrays carry a zip hazard that becomes a breaking change once
host adapters read them.

## Process note

The no-implementation-bodies format held a fourth time — **zero transcription defects across
four plans**. The defect class has moved again, and this time it moved somewhere expensive.

**Plan 5 shipped eight defects in its own tests.** Three were loudly wrong and cost a fix round
each. Four were silently vacuous — green suites over code paths with zero protection, including
one where the entire outcome-apply path could be deleted with all 579 tests passing. The eighth
was in a *specification* rather than a fixture: the degradation-equivalence normalizer was asked
to strip `detail` and compare the rest, but `state` differs by construction, so the property as
written was unsatisfiable by any correct implementation.

**One of them changed the architecture.** Task 9's polling fixture never drained microtasks,
which made a *correct* busy-gate implementation look broken. The implementer measured that
accurately, removed the gate, and wrote the rationale into a production comment as though it
were a fact about real time. It was a fact about a fixture. The consequences were a stale poll
overwriting a fresher one and unbounded in-flight fetches — both live-show failures.

Two things caught essentially all of this, and neither was running the suite:

1. **Mutation testing.** Reviewers broke the implementation and watched what stayed green. It
   found something on nearly every task. It should be a *step in the brief*, naming the specific
   mutations, not a reviewer's initiative — that change is now in
   `docs/superpowers/plan-authoring-rules.md`.
2. **Reviewers verifying their own suggestions.** The final Task 10 fix is the sharpest example:
   the reviewer's one-line guard was itself insufficient, because a value-based pin only binds
   where the fixture's value differs from what an eraser writes. The implementer caught it, and
   replaced the fixture with one carrying no empty, null, or zero value — so a new `ShowSnapshot`
   field is now a compile error rather than silently unguarded.

The eight rules distilled from this are in `docs/superpowers/plan-authoring-rules.md`. Read it
before writing Plan 6. Expect its defect class to be subtler than fixtures, and expect those
rules to be insufficient rather than wrong.
