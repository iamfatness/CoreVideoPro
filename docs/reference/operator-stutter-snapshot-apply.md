# The operator stutter is the SNAPSHOT APPLY, and it has two halves (2026-09-12)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

Owner, live: "While streaming I am seeing the stuttering on inputs in preview and
program and the multiviewer." **The core was innocent and measured so**: 59.8
render slots/s, 60.0 video-out ticks/s, zero deadline misses, zero underruns, no
monitor shedding, render work 3.9 ms of a 16.7 ms budget. All three stuttering
surfaces are WinUI hosts on one thread, and that thread was the problem.

From their `launch.log` (14:00-14:12), EVERY apply exceeded 10 ms, a steady
360/minute = 6/s:

| | median | p90 | max |
|---|---|---|---|
| `ApplySnapshot` total | **19.0 ms** | 21.3 ms | 234 ms |
| `audioReadouts` | 9.0 ms | 9.4 ms | 224 ms |
| `applyParticipants` | ~4.2 ms | | **197 ms** |

Their own present logs show the cost: preview 1440 frames in 32 s (**45/s**),
multiview 1440 in 29 s (**49.6/s**), against a core delivering 60.
`RefreshSurfaceBindings` is NOT a contributor (0.5-1.0 ms/call, 11/s, UI busy 1%).

**Half one, the steady tax: diagnostic strings recomputed at snapshot rate.**
`RefreshAudioReadoutBindings` fired 26 notifications, ~10 of which each walk the
whole audio session, capture sources, senders and recording to BUILD A STRING
(proof, validation checklist, capture/bus/monitor summaries). They are now split:
meters and device labels stay live on every snapshot, the diagnostic summaries
run on a 1 s throttle — the same interval and shape as
`SettingsViewModel.RefreshDiagnosticsReadout`, which is called one line below for
exactly this reason. **Every operator gesture still passes `throttle:false`** and
gets an immediate answer; only the per-snapshot path throttles.

**Half two, the spikes: one structural rebuild costs up to 202 ms.** When
`ApplyLiveParticipants`'s signature flips it runs ten synchronous UI-thread
operations, two of which replace a bound collection. It fired 93 times in twelve
minutes. It is COALESCED now — leading edge plus one trailing timer, 150 ms, the
same proven shape as the surface-binding throttle above — so the first change
still applies immediately (a join/leave is not made laggy) and a burst collapses
to one extra rebuild carrying the LATEST set, not the one that tripped the
signature.

**THE OBVIOUS SUSPECT WAS WRONG, AND THAT IS THE LESSON.** The signature buckets
each participant as video-on/off, so "camera flicker during resubscribe churn" was
the natural explanation. It is false: 207 engine `participants` payloads across
that exact window carry **zero** video-on/off transitions, **zero** screen-share
transitions and **zero** roster id-set changes. Something else flips that
signature and it is still unidentified. Four hypotheses were killed by experiment
in the test meeting, each its own build: verbose diagnostics (forced on — no
effect), Engine off vs on, sources assigned, and tab realization (moves
`audioReadouts` 0.1 -> 0.4 ms, real but 22x short of 9 ms). **Do not "fix" the
video-off signature on the strength of the comment above it.**

**So the rebuild now NAMES ITS OWN SLOW STEP**, like the FFmpeg stderr tail and the
media-decoder branch logging: each of the ten operations is timed, and a rebuild
past ONE FRAME (16.7 ms) logs `perf: structural participant rebuild <total>ms
participants=N :: roomLists=… gallery=… audioRows=… multiviewTiles=…
participantList=… showInputEditors=… multiviewGrid=… previewRouting=…
productionReadouts=…`. Deliberately NOT gated on verbose diagnostics — this
incident was only diagnosable because verbose happened to be on. Verified firing
(4.7 ms here, largest step `multiviewGrid` 1.3 ms); silent below one frame.

**Still open:** which step costs 197 ms on the owner's machine (it is ~4.7 ms on
this one), and what actually flips the signature. The next occurrence answers
both in one line.
