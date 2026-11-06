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

## November 5 investigation result

A gate now holds the first maintenance callback after it samples an incomplete
GPU operation. The test observes the original immutable snapshot, changes the
completion flag, then releases maintenance. With the original two reads, the
callback publishes completed=0 but pending=false, so the owner correctly sleeps
and never publishes completion. This forced schedule failed at the completion
assertion; its patch and log are retained as `1137-forced-schedule-red.patch` and
`1137-red-test.log` under `D:/CoreVideo-QA/overnight-20261105`.

The fixture now samples completion once for both fields. The forced transition
therefore leaves work pending for the next maintenance poll, which publishes
completed=1 and then sleeps. All seven monitor-worker tests pass. Original
snapshot immutability, copied sequence 7, one completed render and owner-thread
maintenance/destruction remain asserted. Failure cleanup releases the gate
before joining the worker. Production worker code and the completion backstop
are unchanged. Full integrated Release and exact-head hosted TSAN remain pending;
the Windows result is not a sanitizer claim.
