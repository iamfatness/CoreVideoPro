# Monitor retirement observation: specification and investigation plan

Issue: [#1137](https://github.com/iamfatness/CoreVideoPro/issues/1137). Order/limit: [BACKLOG](../BACKLOG.md).

## Contract

An owner-thread maintenance observation is internally consistent: unfinished GPU work reports pending; completed work publishes the reader completion and then permits sleep. Published snapshots are immutable; previously exported attribution stays stable. Backend destruction occurs on its owner thread and one completed render remains one render.

The original TSAN run failed `completedCopies == 1` without a data-race report. The fixture reads atomic `gpuDone` separately for `completedCopies` and `pending`. A false-to-true transition between reads can construct completed=0/pending=false. That is a test-fixture hypothesis, not evidence of a runtime worker bug.

## Method

1. Read `MonitorRenderWorker`'s maintenance/sleep contract and preserve the failed run. Build a deterministic two-phase producer transition with bounded gate release and failure-safe cleanup.
2. Force completion after the fixture samples its completion flag but before it constructs pending. Demonstrate that the old fixture can suppress another poll and fail the immutable-reader assertion.
3. If confirmed, sample the atomic once for each observation. Ensure the forced transition still causes another maintenance pass and eventual completed=1. Do not add arbitrary sleeps or change worker code unless a separate runtime counterexample demands it.
4. Preserve checks for old snapshot completion=0, current copied sequence=7, completed render count=1, owner-thread maintenance/destruction, and eventual idle without ongoing polling. Await observable state transitions with deadlock backstops, not a shared deadline accidentally consumed by startup.
5. Run the exact test, whole monitor group, relevant Release suite and hosted TSAN. Reintroduce the double-read mutation and confirm the deterministic test catches it. Distinguish sanitizer availability from a passing local Windows run.

## Completion

The actual bad interleaving is demonstrated and fixed; no production timing tolerance is widened. Tests verify progress and immutability against fixture constants. If the hypothesis is disproven, retain the counterevidence and do not manufacture a worker fix.
