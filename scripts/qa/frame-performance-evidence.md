# Strict frame performance evidence

Run `node scripts/qa/validate-frame-performance.mjs REPORT.json [VERDICT.json]`.
Exit 0 means frame evidence passed for this bounded interval only. Exit 1 means
failure, insufficient evidence, or invalid input. Reports are limited to 4 MiB.
Run regression tests with `node --test scripts/tests/frame-performance.test.mjs`.

`operatorFunctionalPassed` copies the independent `soakPassed` result. It cannot
establish `framePerformancePassed`. Rounded loop-start maxima and average FPS
cannot establish actual GPU readiness or presentation. Legacy late-loop counts
are reported as such, never relabeled as exact missed-frame counts.

The report supplies finalized ISO timestamps `startTime` and `endTime`, an
`errorMatches` array, and `performanceEvidence` with:

- `metricVersion: "anchored-deadline-v1"`, `targetFps: 60`, `clock: "monotonic"`.
- `requiredPaths`: unique IDs including `cpu-submission`, `program-gpu`, and
  `program-presentation`, plus every enabled output destination.
- `enabledOutputs`: explicit inventory of enabled destination IDs, such as
  `recording:program` and `ndi:program`. Empty means none enabled. The runner must
  capture the actual configuration; the validator cannot discover running outputs.
- `paths`: one measurement per required ID, at most 64.

Each path supplies `id`, `measurement: "completion-deadlines"`, `errors: []`, and
`coverage: {complete: true, startTime, endTime}` covering the entire requested
interval, including the first and last partial telemetry windows. Safe integer
counters are `firstSlot`, `lastSlot`, `expectedSlots`, `completedSlots`,
`uniqueCompletedSlots`, `deadlineMisses`, and `skippedSlots`. Sequence range and
completion counts for GPU/presentation/output paths must agree, contain enough
slots for the duration, and misses and skips must both be zero. Uniqueness is actual frame identity, so padded or
duplicated output cannot count as distinct rendering. Counters describe this
interval, obtained from correctly bounded cumulative telemetry; lifetime counts
must not be substituted. A restart or telemetry gap makes coverage incomplete.

CPU completion means CPU submission only. CPU deadline misses, skipped producer
slots, and legacy loop timing are diagnostics; a buffer may absorb producer
variation while meeting every scheduled delivery deadline. They are reported in
`diagnostics`, not converted into presentation failures. GPU deadlines are the
scheduled buffered presentation deadlines, not the earlier producer deadlines.
GPU and presentation paths require their own completion observations; export paths require actual destination
completion. Missing instrumentation must leave those paths absent, producing
`insufficient-evidence`. Do not fabricate zero counters or copy CPU counts into
downstream paths. Exact optional `worstPresentationIntervalNs` exceeding
`ceil(1,000,000,000 / 60) = 16,666,667` nanoseconds fails. This integer-nanosecond
quantization admits the rounded rational 60 Hz period; 16,666,668 ns fails.
Old rounded `worstFrameMaxMs` is deliberately not used as a
presentation deadline. Anchored deadline counters, rather than rounded loop-gap
metrics, establish deadline compliance.

The gate validates supplied evidence, not its authenticity. Preserve the source
telemetry, tested hardware/workload/output configuration, and collector alongside
the verdict for review. Existing CPU-only telemetry cannot yet produce a complete
performance pass, even if an operator soak succeeds.
