# CPU scope attribution for GPU handoffs

`COREVIDEO_QA_GPU_SUBMISSION_TIMING=1` explicitly enables slow-call diagnostics
for internal D3D encoder, monitor and participant export handoffs. The maintained
synthetic harness accepts `--gpu-submission-timing`, pins the flag in its manifest
and retains the real core's stderr. No defaults, source selection, GPU calls,
keyed-mutex keys, buffering, quality or ownership transitions change.

Each exporter emits one `[gpu-submit-scope-v1]` activation record with a fixed
internal branch label and process-local exporter instance epoch. That epoch
identifies an exporter object; it is not a source/media/resource generation.
Instances retained across resize keep their instance epoch. No source names,
meeting information or media are logged.

The producer's existing submit path measures five CPU scopes in order: slot
reservation under its mutex, `AcquireSync(0, 0)`, `CopyResource`, `ReleaseSync(1)`
and queue publication/notification. A call lasting at least 8 ms emits a fixed
numeric `[gpu-submit-cpu-v1]` record with its frame number and completed-stage
count. An early return leaves later stages unknown, represented by that count;
zero-valued unexecuted fields are not measurements. The remaining partial/tail
duration and total accompany the five intervals. Durations use steady-clock
nanoseconds. No clock is read by this scope when the flag is disabled.

Encoder prewarm and its existing `waitUntilReady` are measured separately before
submit. A slow `[gpu-readiness-cpu-v1]` record carries prewarm/readiness durations,
completed-stage count, frame identity and remaining tail. The producer-transfer
scope begins only after readiness handling, avoiding falsely attributing a slow
submit to readiness. These diagnostics do not extend the readiness wait.

Records enter the core's existing bounded asynchronous logger. They introduce
no hot-thread file I/O, background DLL thread or unbounded queue. Missing/dropped
log records remain missing evidence. This is slow-region attribution, not a full
per-frame trace or a health/qualification judge; use the independent delivery
trace and its loss/finalization checks for boundary acceptance.

CPU scopes include OS scheduling and preemption. A long acquire scope locates
time around that API call; it does not prove that the driver blocked, that GPU
execution lasted that long, or that a particular consumer caused it. Even a
zero requested timeout is not a hard real-time guarantee. The records explicitly
leave GPU duration and driver cause unverified. Frame numbers belong to the
exporter's incoming `ProgramFrame`, while older renderer log counters may advance
before logging; do not assume every historical counter label has the same identity.

When measuring trace overhead, hold this optional flag constant across both
conditions and compare matching manifests. Its diagnostics do not establish
actual content latency, receiver/display presentation, A/V or fleet behavior.
