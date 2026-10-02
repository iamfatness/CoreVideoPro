# One destination failing cannot take the show down (beta slice, PR19 — output supervisor)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

Each network destination now has a supervisor with **a generation, a health signal and a
bounded restart policy**. It is a decorator that sits OUTSIDE the per-protocol async writer:

```
CompositeOutputSender -> SupervisedOutputSender -> AsyncOutputSender -> RTMP / SRT / NDI adapter
```

**Outside, not inside — that is the entire isolation argument.** Every call the supervisor
makes lands on `AsyncOutputSender` (enqueue-and-return; `session()` is a cached snapshot),
so a wedged FFmpeg pipe or a blocked libNDI send cannot block the supervisor, the control
path, or a sibling destination. Restarts run on the supervisor's own thread — never the
render tick, never under `coreMutex`, the same reason `BrowserSourceHostAdapter` spawns on
its own supervisor thread.

- **The ladder is the HOUSE ladder** (`modules/OutputDestinationSupervisorPolicy.h`), adopted
  verbatim from `BrowserHostRestartPolicy` / `PluginHostRespawnPolicy` / `ShowEngineRestartPolicy`
  / `MediaCoreSupervisor`: 5→10→20→40→60 s, **give up after 5 consecutive failures**, reset the
  budget after a healthy RUN, and an operator reset (`recover`) always clears give-up.
- **HEALTHY = accepted units advancing, re-evaluated at read time.** `framesSent +
  audioFramesSent` is the only number that can only move when the transport actually took
  bytes from us. A launched FFmpeg child, a non-null `NDIlib_send_create`, or a status string
  reading "live" are all LAUNCHES, and each has been observed to survive the destination
  dying. Rule 7, at the exact point it was ignored.
- **Three budgets, three questions, and they are deliberately different numbers.**
  1000 ms = "is it producing right now?" (the SAME declared value as
  `core::kProducingProgressStaleMs` / the qualification judge's `encoderQueueAgeMs`, so health
  and the truthful lifecycle cannot drift apart); 5000 ms = "is it coming back on its own?"
  — restarting an encoder over a 1.2 s hiccup costs a reconnect and a keyframe for nothing;
  30 s = "has it earned its budget back?"; 15 s = "did it ever come up at all?".
- **Generations are checked by OBJECT IDENTITY, not a counter** (`DestinationEventStamp`
  carries a `weak_ptr` to the run token). `ShowEngineSupervisor`'s stated reason applies
  exactly: a number-only guard reds nothing when the child object is swapped, and a retired
  FFmpeg child's last snapshot would otherwise vouch for the generation that replaced it.
- **A terminal failure BYPASSES the ladder.** The show engine's exit 78 is the model: an
  unreachable endpoint is retryable, an inadmissible configuration is not.
  `isTerminalResultCode` names them (`endpoint-missing`, `rtmp-settings-{missing,invalid}`,
  `source-name-invalid`, `runtime-missing`, `ffmpeg-missing`, `*-output-unavailable`), and an
  **unknown code is RETRYABLE** — guessing "terminal" would silently stop protecting a
  destination, which is the failure this exists to prevent.
- **Give-up is LOUD, never a private opinion.** The supervisor rewrites the published sender
  record to `status: failed` / `lastResultCode: "supervisor-gave-up"` with its reason, so
  `core::SenderLifecyclePolicy` (which reads `status`/`destinationHealth`) reports it as
  failed everywhere downstream. There is no second status machine.
- **It reaches a support bundle.** `outputSenders.senders[].supervisor` carries generation,
  healthy, gaveUp, consecutiveFailures, restarts, nextAttemptInMs, lastProgressAgeMs,
  acceptedUnits, staleEventsRejected, malformedObservations, failureClass, reason — and
  `/snapshot` serves `sessionState` verbatim, so it is visible immediately.
- **NDI's residual risk is PUBLISHED, not fixed.** `destinationIsolationTraits()` is the one
  place that says which destinations are actually contained: RTMP/SRT are out-of-process
  FFmpeg children in a job object and `interrupt()` ends them; **NDI is in-process** (runtime
  `Processing.NDI.Lib.x64.dll`, `send_send_video_v2` on the writer thread with `clock_video`
  set) and **implements no `interrupt()` at all**, so a wedged libNDI send can be detected,
  published and escalated but NOT released — the writer is detached after
  `AsyncOutputSender`'s 2 s grace and leaks until exit. Moving NDI out of process is PR 20/28.
  An UNKNOWN destination is assumed unprotected; never claim isolation that was not established.

Tests: `native/tests/OutputDestinationSupervisorTest.cpp` — the ladder, the health rule, and
a deterministic in-process fault host covering hang, crash, malformed reply, IPC disconnect,
stale completion, restart, terminal failure, and off-caller-thread action.

**Build gotcha this shipped with:** widening `OutputSender` (in `Interfaces.h`, included
almost everywhere) while another agent was building in the same `native/build-dev` left a
test TU compiled against the OLD struct. The symptom was NOT a link error — it was
`FAST_FAIL_STACK_COOKIE_CHECK_FAILURE (0xC0000409)` inside a test that stack-constructs
`MediaCore`, at a different point on each run, which reads exactly like a heap race in
someone else's code. **After changing a widely-included struct, rebuild with `--clean-first`
before believing any crash you see.**
