# Show Engine — Plan 4 Outcomes

Companion to `2026-08-05-show-engine-capabilities.md`. Written at the close of Plan 4
execution (2026-08-06).

**Shipped:** the capability model — `resolveCapabilities(config, health)` crossing declared
config with live integration health into `available | unavailable | disabled`, plus the two
structural decouplings the plan existed to make: roles re-keyed from PIN to `PersonKey`
(`pin → normalized name → participantId`), and `LookDefinition.boxFill` with `"queue"`
degrading to `"manual"` rather than emptying the guest boxes. 417 tests; typecheck, the new
test-inclusive typecheck, and build all clean.

## One claim to correct before Plan 5 reads it

**The "8 pre-existing type errors in test files" carried forward from Plan 3 was wrong.**
The final review reproduced them against the base commit: 7 of the 9 were introduced *by this
branch* — fixtures left stale by the `Panelist`, `LookResolution`, and `OverrideRecord` shape
changes, including a `persistence.test.ts` fixture still asserting `{pin: "1383"}`, a shape the
engine no longer produces. They read as inherited drift only because nothing had ever
typechecked a test file in this package. That is closed now (`show-engine/tsconfig.test.json`,
`typecheck:tests`, and a CI job); do not re-inherit the belief.

## The bug worth remembering

`clampPage` was written in Plan 3, before capabilities existed, and clamped against the look's
`pageCount` without knowing whether the hands queue was usable. During an outage `resolveLook`
correctly fell back to manual fill — `pageCount` becomes 1 — but a stale `page: 2` from before
the outage passed through `clampPage` unchanged and then made `resolveLook` **throw**. The
exact scenario this plan exists to survive took the tick down, violating three separate spec
clauses including §2's normative "no external integration failure may block a tick."

All 404 tests passed over it, including the degradation-equivalence property that is this
plan's stated deliverable. **The property was true; it just was not quantified over page
number** — every fixture used `page: 0`, so the entire paging seam sat outside the property's
reach. A property test binds only over the inputs you actually vary, and a constant in a
fixture is an unstated precondition.

The root cause was deeper than the missing argument: `clampPage` and `resolveLook` computed the
page range independently. They now share `pageCountForFill`, so they cannot disagree by
construction. The deliberate contract split — `clampPage` forgiving, `resolveLook` throwing —
is preserved.

## Decisions worth not re-litigating

- **`unavailable` and `disabled` are read at exactly one site.** A `Capability` value reaches
  only `effectiveBoxFill` (`lookDirector.ts:85`), through `canUse`, which collapses to
  `state === "available"`. The load-bearing property therefore holds structurally, not merely
  by test. Any future code that branches on *which* unusable state it is — outside the
  operator-facing `detail` string — breaks the design.
- **`clampPage` and `resolveLook` both take an optional `handsQueue`, defaulting to
  `NO_HANDS_QUEUE`.** The final review argued for making `clampPage`'s required. Kept optional:
  the two are symmetric, and the default points the safe way — omitted means unusable, which
  degrades rather than crashes. See the Plan 5 obligation below.
- **An absent `mukana` block is a parse error when any integration is enabled.** A config that
  asks the engine to poll an address it was never given is a mistake, not a degradation. A
  registry-less show has all flags off and parses clean.
- **`STATE_VERSION` is 2, and `ShowState.version` is typed to that literal** so a stale version
  in a fixture is a compile error rather than a silent test skip. The module's policy is
  reject-don't-migrate; a Plan-3 state file now fails loudly instead of silently discarding
  every operator-assigned role.
- **`personKeyForPin()` is the only place the `"pin:"` prefix is spelled.** `assignExclusiveRole`
  previously reconstructed a PIN by parsing a key that `personKey.ts` documents as opaque. It
  now re-keys the registry through the helper instead.

## Carried into Plan 5

**Obligation, not a suggestion: review every adapter for the `clampPage`/`resolveLook`
capability pairing.** Both parameters are optional and both default to unusable. Omitting on
one call while passing `available` to the other silently pins the operator to page 0 — no
crash, no error, just a paging control that does nothing. Pass the same capability to both, or
make both required at that point and take the API break while there are still only three
consumers.

**Still owed from Plan 3, now overdue.** Nothing yet calls `shouldFollowSpeaker` before a
position assigner. The concrete defect — an ASL interpreter evicting a panelist from the FILO
pool, swapping into the visible gallery window, and sorting to the front — stays live. Plan 5's
orchestrator must wire the dispatch gate and prove an interpreter speaking produces **no
`PlacementChange`** from either assigner.

**`ManualBoxAssignments` is not in `ShowState` yet.** The fallback the whole design rests on
currently has nowhere to persist. It must survive a tick and be cleared when the look changes
(spec §3.2).

**`registry` and `questionFeed` have no in-package consumer.** Only `handsQueue` is read today.
The spec's registry-outage scenario is untestable until the orchestrator exists — Plan 5 owns
proving that killing the registry mid-tick leaves the picture, the roster, and tally intact.

**Unchanged from Plan 3's list:** the `mukanaClient.ts` doc comment still claims `applyHealth`
is the sole health writer while `fail()` also writes directly; spec §4.2's
`ohg.gfx.headline.*` actions have no counterpart in `OverlayState`; `TallyState`'s three
parallel arrays carry a zip hazard that becomes a breaking change once host adapters read them.

## Process note

The no-implementation-bodies format held for a third plan — zero transcription defects.

The defect class moved again. Plan 1 was transcription, Plan 2 cross-task contract mismatch,
Plan 3 interfaces that described the producer without checking the consumer. **Plan 4's was a
property that did not quantify over enough.** The plan asked for the right invariant and stated
what each test must break on; the tests were written faithfully and reviewed twice at task
level; and the hole was in a variable nobody thought to vary. Both the task reviewer and the
implementer's own mutation testing confirmed the property bound — correctly, over the inputs it
ranged across.

The refinement for Plans 5–9: **when a plan specifies a property test, specify its
quantification.** Name the inputs it must range over, not just the invariant it must break on.
"Identical output under `unavailable` and `disabled`" was satisfiable by a test that held page,
look, and roster constant. "…for every page in the valid range, every fill strategy, and a
roster both larger and smaller than the box count" would not have been.

Second, smaller: a whole-branch review found what eight task-level reviews could not, because
it was the first reader positioned to notice that a Plan 3 function had not been revisited when
Plan 4 changed its preconditions. Cross-plan preconditions are invisible from inside a task.
