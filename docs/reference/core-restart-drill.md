# The core-restart drill (2026-10-01)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

`python scripts/qa/core-restart-drill.py [--takes N] [--record] [--record-seconds S]` kills the
media core under a running app that is in a meeting with Engine on, and judges the recovery.
Control API plus one process kill; no keyboard or mouse input.

- **Gated:** the core restarts and rejoins; the SHELL's roster equals the new core's (`/state`
  now carries `zoomRosterEpoch`, `zoomRosterRevision` and `zoomParticipantIds`, next to
  `/snapshot`'s core JSON); the multiviewer config survived; Program delivers frames; Takes after
  the restart log no roster dip; once the dead engine's old participant has left, no show slot
  names someone absent; no new Program underruns.
- **Reported, not gated:** which slots the app's own user holds before and after, and what
  happened to a recording.
- **What it measured on `43b33b9` (three runs, test meeting):** recovery in about 9 s. A
  25.7 s recording was 26.0 s playable after the kill, so the file survives to within its last
  fragment. Before #732 neither the recording nor the stream came back (see below).
- **It found a second recovery defect on its first day: a core that restarts and never rejoins
  Zoom (2 of 6 runs).** `MediaCoreSupervisor.OnChildExited` raises `HealthChanged` with its
  lock released, between marking the dead core and respawning it. `StartAsync` spawns a core
  whenever it finds no live process, so any shell call that "ensures the core is running" in
  that window spawned the replacement itself, and the crash handler then saw a child it did
  not spawn and returned WITHOUT running `RecoverChildAsync`. The core was up, the meeting and
  the capture connections were gone, and nothing was logged. Fix: `_crashRecoveryOwed` is set
  at the crash and cleared only by a deliberate Stop; the handler adopts a child someone else
  started and still runs the recovery. Test: `CoreRecoveryRejoinTests` (a `StartAsync` fired
  from inside `HealthChanged`; fails without the fix). After the fix, 4 of 4 live runs
  rejoined, which is not enough runs to prove a one-in-three race gone; the unit test is the
  proof.
- **Its roster checks were watched failing:** with `ReleaseRosterBarrierForRespawnedCore`
  disabled the drill reports the shell on the dead core's epoch and a slot naming someone not
  in the meeting. Its "Takes do not dip" check passed in that run and is the weaker one.
- **Recording and stream both come back after a core restart (#732, owner rulings 2026-10-01
  and 2026-10-02).** `ApplyBridgeHealthChanged` has always cleared the Recording and Streaming
  intents the moment the core starts recovering (`InterruptOutputSessions`: the old session's
  continuity is gone, and the bridge must not re-assert it against a core that has not
  rejoined Zoom). Nothing remembered that they had been running, so the rest of the show was
  neither recorded nor streamed. Now:
  - `OutputResumeAfterCoreRestart` (MediaCore, pure, tested), one instance per output, holds
    that one fact across the crash. A second crash before the resume does not forget it; the
    operator pressing that output's button, or a deliberate core stop, cancels it.
  - The supervisor raises `RecoveryCompleted(CoreOnly | ZoomRejoined | ZoomRejoinFailed)`
    once per recovery, after the rejoin's roster is published.
  - **Recording** needs the meeting: on `ZoomRejoined` a NEW session starts in a new folder;
    otherwise the Output status line says why not.
  - **Stream** does not: it reconnects on every outcome, because a destination left
    disconnected ends the broadcast.
  - Both start through the ordinary transport path (pre-flight, start proof, rollback, retry),
    retried for up to 10 s until the shell has applied the recovered core's state.
  - Every step is in `launch.log` as `recording: ...` / `stream: ...` and on the Output status
    line.
- **Measured live (test meeting, 2026-10-02, one run):** stream to a local SRT listener plus a
  recording with 8 ISOs; core killed; Recording back 9.8 s and Streaming 10.3 s after the kill;
  the listener received the new session at 60 fps. The drill script itself still does not
  stream: the shell's destination is the saved real one, so that run swapped the saved
  destination for `127.0.0.1` by hand and restored it. **Not measured:** RTMP, a real
  platform's behaviour when the same key reconnects, and a restart whose Zoom rejoin fails.
- **Two things to know when reading a run:** the rejoined app user has a NEW Zoom id and the
  old instance stays in the meeting for about 50 s; and `recording.totalFramesWritten` sums
  Program and every ISO, so it is not a duration.
