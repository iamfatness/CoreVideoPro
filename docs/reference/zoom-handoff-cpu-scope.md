# Zoom video handoff CPU scopes

Set `COREVIDEO_QA_ZOOM_HANDOFF_TIMING=1` before constructing the runtime, or
pass `--zoom-handoff-timing` to the maintained synthetic monitor harness.
The flag defaults off. Disabled scopes read no clock, allocate no storage and
emit no records. GPU/source selection, playout, thumbnails and mutex ownership
are unchanged.

Program's `configured`, compositor roster poll and latest-decoded fetch share
the runtime mutex with video publication. Opt-in scopes separate wall time
before acquisition from the body. The publication batch also sums the wall
time spent encoding its existing thumbnails and counts those encodes. The
sum is a subset of the batch body; it is not another duration to add to it.
Encoding continues in its existing location under the mutex in this diagnostic
change. This allows testing its contribution without silently implementing a
repair or changing the control workload.

Calls of at least 8 ms emit `[zoom-handoff-cpu-v1]` through the bounded asynchronous
core logger. The scope is declared before the lock guard, so its destructor
emits only after the guard releases the mutex. Fixed operation labels contain
no source names, participant identifiers, credentials or media. The activation
record is `[zoom-handoff-scope-v1]`. These are CPU wall durations in steady-clock
nanoseconds, including scheduling/preemption. A long mutex acquisition does not
identify the owner holding it, and overlapping durations do not alone prove
causation. No clock calibration, source/Program identity or GPU duration is claimed.

This is selective attribution, not complete boundary tracing or a health judge.
Absent/dropped records remain unknown. Preserve the raw delivery trace and
independent output/recording failures. Hold the flag constant in matched
instrumentation comparisons. No beta or parent qualification follows from an
activation marker or a clean short run.
