# Plan-authoring rules for the show-engine series

**Status:** living. Derived from actual defects in Plans 1–5, not from general principle.
**Read this before writing Plan 6.** Every rule below cost a fix round, a wrong design, or both.

## Why this file exists

The plan format works: since Plan 2 stopped supplying implementation bodies and switched to
interfaces + behavior prose + complete tests, **zero defects have traced to transcription**
across four plans. That is the format earning its keep.

What it did not do is make the plans correct. It moved the defect class up the stack — from
"the implementer copied my code wrong" to "my tests were wrong in ways a green suite could not
reveal." Plan 5 alone shipped **seven fixture defects**, and one of them changed the
architecture. These rules target that class specifically.

## The governing rule

**A passing test is not evidence. A test that would fail if the code were wrong is evidence.**

Every rule below is a way of closing the gap between those two statements. On Plan 5, nearly
every real finding came from a reviewer breaking the implementation and watching what stayed
green — not from running the suite.

---

## 1. State the fixture's shape invariants, not just its values

Three Plan 5 fixtures **could not pass under any correct implementation**:

- `gallery.assignments: []` against `cells: 16` — `GalleryDirector.fromJSON` has a hard
  `assignments.length === cells` invariant, so the restore tests threw unconditionally.
- `setLook("banter")` against a config fixture defining only `teatime`.
- An overflow assertion assuming `p1..p10` sort numerically. They are strings: `p1, p10, p2, …`

These are cheap — they fail loudly on first run — but they burn a round each. When supplying a
fixture, state alongside it the invariants it must satisfy: **array lengths that must match a
declared size, ids referenced elsewhere in the config, and any ordering the assertions depend
on.** The implementer can then check the fixture rather than discover it.

## 2. Never assert a computed value against another value from the same computation

Plan 5 Task 7's capability property compared `snap.page` against `snap.look.pageCount`. Both
come from the code under test, so under a mutation they moved together and the comparison
stayed true. Dropping the capability from **both** `clampPage` and `resolveLook` — which kills
paging outright — passed all 548 tests.

The fix was an expectation the fixture knows independently: 5 PINs against a 2-box look means
`pageCount` is exactly 3 when queue fill is genuinely engaged and 1 otherwise. That number comes
from the fixture's design, so no mutant can move it.

**Every assertion needs at least one operand that is a fixture-derived constant.** A property
whose two sides both come from the implementation is a tautology with extra steps.

## 3. Specify a property test's quantification, not only its invariant

Plan 4's degradation property was correct and still missed a throw on the exact outage path it
existed to prevent, because every fixture held `page: 0`. Plan 5 Task 6's gate property could
not observe a documented ordering rule because `skipRoles` was fixed at its default.

**Name the inputs the property must range over.** Plan 5 Task 7 got this right — page −2..4 ×
three capability states × two fill strategies — and it worked. "Identical output under
`unavailable` and `disabled`" is satisfiable by a test that holds everything else constant;
"…for every page in the valid range, every fill strategy, and a roster larger and smaller than
the box count" is not.

## 4. An async fixture must drain microtasks — or it will steer the design

This is the expensive one, and it is new as of Plan 5.

Task 9's polling fixture ran two ticks before reading. The fetch outcome first lands at tick
five, so both reads saw untouched defaults, the retention test passed vacuously, and **the
entire outcome-apply path could be deleted with 579/579 green.**

Worse: the same fixture made a *correct* busy-gate implementation look broken. The implementer
measured that accurately, removed the gate, and wrote the rationale into a production comment as
though it were a fact about real time. It was a fact about a fixture that never drained
microtasks. The consequences were a stale poll overwriting a fresher one (the hands queue
jumping backwards on air) and unbounded in-flight fetches against a hung endpoint.

**If the code under test starts promises, the fixture must drain them.** A timer-free
`for (let i = 0; i < 8; i += 1) await Promise.resolve();` between steps is enough, and it also
decouples the tests from how many `await` hops happen inside the module — which they were
otherwise one refactor away from breaking on.

**And the corollary:** when a supplied test contradicts a reasonable implementation, the test is
the *first* suspect, not the second. Say so in the brief.

## 5. Check a supplied test against the modules it exercises

Plan 5's seat-drop test asserted a hole when a panelist leaves. `ZoomIngest` deliberately keeps a
departed participant marked offline "so they can be restored on reconnect", and
`LiveSlots.refresh` documents that the seat is held. The only way to satisfy the test was to
delete the seat — which made a reconnecting guest **permanently unseatable**, because a
departure never changes the id set.

It took an owner ruling to resolve, and the ruling was that the test was wrong.

**Before shipping a test body, read the doc comments of the modules it drives.** A plan test that
contradicts a shipped module's documented behavior is the plan's bug, and it will be discovered
in the most expensive possible way — as a design argument, mid-implementation.

## 6. Walk the carried-obligations list against the new plan's Interfaces blocks, line by line

Two defects were **predicted in writing before they shipped**:

- Plan 4's outcomes named the `clampPage`/`resolveLook` capability pairing as a Plan 5 obligation.
  Plan 5 shipped it correctly but proved it with a property blind to two of three mutations.
- Plan 3's outcomes wrote that forcing the orchestrator to invent a `"panelist"` role for an
  unseated speaker "is a lie." Plan 5's brief then specified `role ?? "panelist"` verbatim, and
  the exact predicted failure appeared: the engine's gate approved a dispatch that ProgramBus's
  internal gate silently vetoed.

The outcomes docs are working. The **reading** of them is what failed — they were treated as
background rather than as a checklist. Walk each carried obligation against the specific
Interfaces block that touches it, and note in the plan where it is discharged.

## 7. Put the mutations in the brief

Mutation testing found something on nearly every Plan 5 task. It should not depend on a reviewer
choosing to do it.

**Name the specific mutations in each task brief**, next to the tests they target: "remove the
gate and confirm the interpreter cases red on the *assigner* assertions"; "drop the capability
from each call and from both, and confirm all three red." Then require the results in the report.
This converts the practice from initiative into a step, and it makes a vacuous test visible in
the same round it is written.

## 8. Say which assertions are confirmatory rather than independent

Some assertions ride along without adding discriminating power. Plan 4's tally/overlay
equivalence tests could not fail independently of the look-resolution one. Plan 5's ProgramBus
assertions in the gate property pass even with the engine's gate absent, because ProgramBus
applies the same predicate internally.

Both are worth keeping as documentation. Both are dangerous unlabeled, because a reader counts
them as guarantees. **Mark them in the test's doc comment**, and never let a confirmatory
assertion be the only thing standing behind an invariant.

---

## Scorecard, for calibration

| Plan | Format | Dominant defect class |
|---|---|---|
| 1 | implementation bodies supplied | transcription — 5 findings originated in the sample code |
| 2 | interfaces + prose + tests | cross-task contract mismatch |
| 3 | same | Interfaces blocks describing the producer without checking the consumer |
| 4 | same | a property that did not quantify over enough |
| 5 | same | fixtures — wrong, vacuous, and in one case design-steering |

The class gets subtler each time. Expect Plan 6's to be subtler than fixtures, and expect these
rules to be insufficient rather than wrong.
