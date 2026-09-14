# ISO recorder throughput — 2026-09-14

PR #531 includes the tested startup allocation fix from #528 and isolates each
Windows ISO file's encoder work. The observed failure in #529 involved six
hardware ISO tracks plus two software tracks sharing one serial recording worker.
Steady-state queue loss continued without Takes.

Each ISO now has an ordered audio/video worker. Queue limits are six video frames
in steady state and 96 audio packets; codec startup has a bounded 32-frame video
allowance for one second and until that backlog drains. Source timestamps are
captured before enqueue and all tracks use the Program capture epoch. Accepted
media drains before finalization. No source resolution, frame rate, or bitrate
is reduced. Slow tracks have separate queues so they cannot evict another
track's frames. The existing outer worker preserves bounded engine teardown.

Per-track queue drops and encode timing are exposed. Inner and outer recording
loss are combined, and successful finalization cannot erase degraded recording
health. A subsequent clean recording may become healthy again.

Validation at code commit 74efd444: all 1,000 full Release native tests passed.
This includes a real eight-file Media Foundation recording that preserves all
60 submitted pictures per source, bounded queue and blocked-track regressions,
exception/finalization handling, and loss retained after finalization. The real
writer measurements included first-write costs above 500 ms, supporting the
separate bounded startup allowance. This test is not a laptop capacity result.

Live 1080p60 streaming with eight ISO files is the remaining validation. The
already-published beta-2026-09-14-85ecaca0 does not contain this ISO fix.
