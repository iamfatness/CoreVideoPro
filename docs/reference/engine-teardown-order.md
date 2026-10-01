# Engine teardown order (the ZoomISO deadlock class — G4, 2026-07-18)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

The reference product (ZoomISO) froze in production because teardown destroyed
renderers before stopping raw data with a callback in flight. Hard rules:

- **Exit order in the engine:** `Leave()` → `meeting_event.stop_raw_media("shutdown")`
  (raw-media off + unsubscribe_all across video/share/audio) → bounded message-pump
  drain (~250ms, never unbounded) → `share_engine.detach()`/audio shutdown →
  `CleanUPSDK()`. Renderer destructors must NEVER run after `CleanUPSDK()` —
  `EngineVideo` is a stack local in `main()`, so the explicit stop is what
  guarantees that.
- **Callback vs teardown must serialize.** `~ParticipantSubscription` sets
  `m_stopping` first, drains `m_targets_mtx` (acquire+release), THEN
  `unSubscribe()`/`destroyRenderer()` — do not hold a mutex across those SDK
  calls (they may wait on a callback that takes the same mutex). EngineShare's
  single-mutex callback/teardown pattern is the other accepted shape.
- **EngineVideo's subscription maps are guarded by `m_mtx`**, but
  `ParticipantSubscription` build/destroy makes SDK calls
  (`createRenderer`/`destroyRenderer`) and so runs OUTSIDE the map lock — move
  unique_ptrs out of the map under the lock, construct/destroy after release.
  Lock order: `m_mtx` → `m_targets_mtx` (leaf, never reversed).
- **Shell: stop off the UI thread.** Every core stop rides `Task.Run`. Leave-meeting
  (`SettingsViewModel`), respawn and the post-failure `ForceStopMediaCoreAsync` use
  `_bridge.Stop()`: a kill-tree + `WaitForExit(1500)` under the supervisor gate. App exit
  (`StudioViewModel.DisposeAsync`) uses `_bridge.StopForAppExit(2 s)` instead (T1.8, below):
  worst case 2 s grace + 1.5 s kill wait = 3.5 s inside the 5 s `ShutdownTimeout`.
- **App close never cuts a recording off unasked (T1.8, #461, 2026-09-10; fix round 1).** Closing
  used to kill-tree the core with no stop sent, so the Program moov atom, every ISO writer and any
  RTMP/SRT egress died mid-write (unplayable show file). Now, in order:
  1. `MainWindow.OnAppWindowClosing` hands the request to `CloseGuardFlow` (constructible, tested
     in `CloseGuardFlowTests`), which asks `CloseGuardPolicy` (pure, tested). Recording or
     streaming live — the shell flags, the core lifecycle (`stopping`/`finalizing` COUNT as live),
     or `recording.active` on a core with no lifecycle; the virtual camera alone never asks — opens
     a ContentDialog "Stop outputs and close?" with **Stop and close** / **Keep running** (the
     default). The window is restored/activated first and the dialog opens after the Closing
     callback returns. A second close while it is open, or while outputs finish, is ignored (and
     brings the window forward) — never a force-exit. **If the dialog cannot be shown (no XamlRoot,
     another ContentDialog open, anything) while outputs are live, the flow takes the Stop-and-close
     path — never the old kill path.** Only a failure of the stop itself falls through to a plain
     shutdown.
  2. **Stop and close** → `OutputShutdownCoordinator`: sends the EXISTING transport stops
     (`SetRecordingAsync(false)` / `SetStreamingAsync(false)`, not awaited — they can hold for
     30 s on a busy core) and refuses new Record/Stream starts while it runs. It then waits for
     evidence tied to THIS stop, read from the bridge's `LastSnapshot` (never its own syncs, which
     would compete with the stop for the single sync slot):
     **freshness** — only a snapshot whose `RawReceivedUtc` is later than the last moment the stop
     was still pending (intent set or a Record/Stream command in flight — a stop sent while a start
     is in flight is silently ignored by the transport, so it is re-sent once a second until it
     lands); **session binding** — the `OutputStopBaseline` captured at the close request names the
     recording lifecycle session and the senders that were live, and only THEIR terminal states
     count (a stale pre-stop `completed`, another session's state, an old failure, or an ABSENT
     node never read as finished); **core generation** — the supervisor restart count; a change means
     a respawned core answered, so the wait ends at once as `CoreRestarted` ("the recording was
     interrupted with the old core and nothing more can be saved"), and a core that is gone ends it
     as `CoreUnavailable`. The generation baseline is re-armed when the stop is actually SENT: a
     core that respawned while the dialog was open is logged as having lost the old files, and
     whatever the NEW core is doing is still stopped and waited for (fix round 2). Bounded at
     15 s; on timeout it logs `shutdown: outputs did not finish within 15s — closing anyway` and
     proceeds. Only THEN does the unchanged `ShutdownAsync` run, so the 15 s is outside the
     5 s / 6 s watchdog budget.
     **STREAMS ARE STOPPED BEFORE RECORDING, and the order is load-bearing** (fix round 2). Each
     transport stop builds its sync payload inline from the current flags. A recording stop sent
     while Streaming is still desired carries `start-program-output{rtmp…}`, which the core
     treats as unowned (`recordingStatus_` is "stopping") and answers with `encoder->start`: the
     sink generation bumps, the recording lifecycle is ERASED, and the finalized file never
     reports `completed` — so record+stream always ran to the 15 s bound. Pinned by
     `OutputShutdownCoordinatorTests.StreamsAreStoppedBeforeRecording…` and
     `RecordAndStreamTogetherFinishesInsteadOfTimingOut` (both fail with the order swapped). The
     core defect itself (a non-recording Start erasing a finalizing recording's lifecycle — it
     also leaves `recording.status` "stopping" whenever an operator stops Record while Stream
     stays up) is filed separately and NOT fixed here. Two more evidence rules: `interrupted` is
     NOT terminal (the core's `isTerminal` is completed|failed; a stalled writer is still open),
     and a sender only counts as finished when its ADAPTER reads `stopped`/`failed` — the core
     reports `idle`, or re-serves an older run's terminal state, while the adapter is still live.
  3. **App-exit core stop only** (`StudioViewModel.DisposeAsync` → `MediaCoreBridgeService.StopForAppExit`):
     snapshot the core's descendants (`ProcessTreeSnapshot`, pid + start time), close stdin — the
     core's quit signal: JsonRpcServer's reader hits EOF, the loop breaks, all workers join, `main`
     returns and MediaCore's destructors run — wait up to `ShutdownBudget.CoreExitGrace` (2 s) for
     it to exit, THEN the old kill-tree. While it exits the supervisor keeps DRAINING the retired
     core's stdout (`_drainOnlyProcess`), because the core's writer thread flushes before it can
     join — stop reading and a full pipe wedges the exit (`MediaCoreAppExitStopTests` fails with
     the drain removed). After a clean exit it waits ≤250 ms for stderr EOF (the core's last log
     lines) and kills any recorded descendant still alive (the tree kill can no longer reach them;
     that test fails with the sweep removed). A plain `Stop()` that lands while a grace is still
     running (the shutdown-timeout fallback) kills the retiring core instead of orphaning it.
     `ShutdownBudgetTests` pins the budget; the disposal after the core stop is logged with its
     duration. Leave-meeting, respawn and the post-failure `ForceStopMediaCoreAsync` keep the
     immediate kill.
  **Remaining gaps:** the Zoom engine still does not get the Leave → `stop_raw_media` order on app
  close — `~ZoomEngineRuntime` terminates it (same as the kill-tree did). And NONE of this has run
  against the real app yet: the pre-merge live checklist is in the T1.8 fix-round-1 report (idle
  close, Keep running via mouse/Esc/Enter, Stop and close + ffprobe of Program and every ISO,
  repeated closes, minimized close, a stream stop, zero new WinUI dumps).
- **Never delete the vcam SHM file in `stop()`** — same hard rule as the
  virtual-camera section below; stop only unmaps/closes handles, the writer
  re-opens IN PLACE on the next start.
