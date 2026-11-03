# Shell crash during a "forced stop" (#457, dump 17304): triage design

**Date:** 2026-10-23
**Status:** triage design, no code changes. Awaiting owner review.
**Issue:** [#457](https://github.com/iamfatness/CoreVideoPro/issues/457) (backlog T1.7, tier 1 at filing, moved to tier 2 by 6c19ba1b)
**Related:** #462 (merged fix, 3a3bedfa / 6c19ba1b), #513 (finalizer-thread 0xc000027b), #461 / T1.8 (close guard), #433 / PR #1033 (installer WER LocalDumps)
**References:** `docs/reference/winui-crash-class-0xc000027b.md`, `docs/reference/engine-teardown-order.md`, `docs/reference/core-on-air-policy.md`, `docs/reference/core-restart-drill.md`

## Summary

The triage is mostly done. The one open question is what counts as proof.

- **The dump is gone.** `CoreVideoPro.WinUI.exe.17304.dmp` is not on this box: not in
  `%LOCALAPPDATA%\CrashDumps`, not in `%LOCALAPPDATA%\CoreVideoPro\crashes` (that folder does not
  exist), and not under `D:\`. The WER report-archive folder named in the 1001 event is also gone.
  The build folder it came from (`...\ChatGPT\CoreVideo Pro\beta-release\`) no longer exists, so
  there are no matching shell PDBs either.
- **The crash was not caused by the forced stop.** The shell's own log shows a GRACEFUL close that
  finished its cleanup and then fail-fasted. The core shut down cleanly first. This is the
  "post-Exit dispatcher drain" crash, already documented as a member of the 0xc000027b family. It
  is not #513 (finalizer) and it is not the engine-off deadlock class.
- **The fix is merged and the live check it was waiting for has happened, unplanned.** #462 was
  held open only because "the real check is a long in-meeting close." The retained logs since then
  contain **7 in-meeting closes after 41 to 612 minutes**. All 7 took the new
  `TerminateProcess` path. None of them fell back. Event Log has no crash with the 17304
  signature after 2026-09-10.
- **Recommendation:** post the evidence to #457 and close it as fixed by #462. Add a cheap
  standing gate (a graceful close at the end of the RC soak) so that a regression would show up.
  Decide whether the `ForceExit` fallback should also use `TerminateProcess` (residual risk, below).

## What is known

### The issue as filed (2026-09-10)

A 1.1 GB full dump, `%LOCALAPPDATA%\CrashDumps\CoreVideoPro.WinUI.exe.17304.dmp`, was written at
2026-09-10 08:52:43. That was the moment all three processes were force-stopped
(`Stop-Process -Force`) to relaunch from main. The question was whether the shell crashed because
the core died under it (clause 1 of the on-air policy) or whether the dump was an artefact of the
forced termination. The issue named build `c36d80a` as the source.

The 2026-09-10 investigation, recorded in code comments and the crash-class doc, said this:
UI thread, `DispatcherQueue::DeferInvokeCallback` under `DispatcherQueueController::ShutdownQueue`
under `FrameworkApplication::StartDesktop`, stowed `E_ABORT` from a non-managed callback, AFTER
`shutdown: resources released`. The sibling dump 48644 had the same stack with a
`DispatcherQueueTimer::TimerCallback` and stowed `E_UNEXPECTED`.

### Independent evidence collected 2026-10-23 (read-only)

| Source | What it shows |
|---|---|
| Application log, event 1000, 2026-09-10 08:52:36 | `CoreVideoPro.WinUI.exe`, PID `0x4398` (= 17304), module `CoreMessagingXP.dll` 10.0.27200.1048 (WinAppSDK Runtime 2.4.0), exception `0xc000027b`, offset `0x97de6`. Process start time `0x1DD41181B1DB7B6` = 07:32:48, so it had been up 80 min. |
| Application log, event 1001, 08:52:40 | WER APPCRASH, report `7f60387a-6831-4b72-9da0-92429402129b`. Signature P4 `combase.dll` 10.0.26100.9444, **P7 `80004004` (E_ABORT)**, P8 `0x579e4`. This matches the stowed E_ABORT. |
| `%LOCALAPPDATA%\CoreVideoPro\launch.pre-live-confirm.log`, 87 lines `pid=17304` | Build stamp `1.0.0+46b07ff5e7b2...` (**not c36d80a**). `zoom-join: Connected` at 07:33:01, in the meeting until close. Last five lines: 08:52:35.764 `shutdown: close requested`, .788 `disposing studio view model`, .788 `stopping media core`, 36.125 `studio view model disposed`, 36.125 `shutdown: resources released`. Nothing after. |
| `git` | `46b07ff5` (2026-09-10 07:25, "Record the post-reboot state in the handoff") is reachable only from `origin/backup/419-pre-rebase-1d164b7`. It is not an ancestor of `main` or of the fix. |
| Event log 1000, shell only, 2026-09-10 to 2026-10-23 | 16 shell crashes. **Only 17304 has `CoreMessagingXP.dll+0x97de6`.** The rest are `Microsoft.UI.Xaml.dll+0x3a9c5d` (the generic stowed fail-fast site: #513 19580, 50128, several startup crashes), one `coreclr c0000005` (#889) and one `CoreMessagingXP c0000602`. None of them is a shutdown-drain crash after the fix. |
| Retained shell logs, post-fix closes | 41 `shutdown: cleanup complete; terminating the process directly` lines. Zero `force-exit fallback`, `cleanup did not complete` or `TerminateProcess returned` lines. In-meeting closes after 30 min or more: 09-10 20:10 (60 min), 09-11 08:37 (612), 11:02 (123), 15:26 (63), 19:50 (41), 09-13 11:39 (225), 14:38 (67). Session length is a lower bound (log rollover). |
| Dumps present today | Only `CoreVideoPro.WinUI.exe.28528.dmp` (2026-10-16, crashed at startup, `Microsoft.UI.Xaml+0x3a9c5d`). It is **not** #457 evidence. |
| WER LocalDumps on this box | Per-exe HKLM keys for `CoreVideoPro.WinUI.exe`, `corevideo-native.exe`, `corevideo-zoom-engine.exe`: DumpType 2 (full), DumpCount 20, DumpFolder `%LOCALAPPDATA%\CrashDumps` (from `scripts/setup-crash-dumps.ps1`). The #1033 installer layout (`%LOCALAPPDATA%\CoreVideoPro\crashes`, count 5) is not on this box. |
| Tooling | `cdb.exe` at `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\`. `dotnet-dump` 10.0.745401, `dotnet-stack`, `dotnet-gcdump`, `dotnet-trace` are global tools. The WinAppSDK runtime 2.4.0.0 package is still installed, so its native binaries are available for symbol matching. |

### Why "forced stop" was the wrong frame

`Stop-Process -Force` calls `TerminateProcess`. That runs no user-mode code. It cannot raise
0xc000027b, write event 1000 or start a WER dump. The shell logged `close requested` 0.9 s
before the fault, so a graceful close reached it first. The relaunch procedure most likely closed
the window and then force-stopped whatever was left. The 08:52:43 file time is when WER finished
writing a 1.1 GB full dump, 7 s after the fault. The core did not die under the shell: the shell
stopped it, and `studio view model disposed` was logged 337 ms later. On-air policy clause 1 is
not involved.

## The three hypotheses

| # | Hypothesis | Would be confirmed by | Would be excluded by | Verdict |
|---|---|---|---|---|
| 1 | The #513 finalizer class: the GC finalizer thread releases a XAML object (`WinRT.IObjectReference.Finalize` → `ComObject<...>::Release` → stowed `E_ILLEGAL_METHOD_CALL`) | Faulting thread = CLR Finalizer (MTA). `GC.RunFinalizers` on the managed stack. Stowed `0x8000000E`. Fault in `Microsoft.UI.Xaml.dll+0x3a9c5d`. Time-delayed, often idle. | Faulting thread is the UI (STA) thread with `ShutdownQueue` on the stack. Stowed code is not `0x8000000E`. Fault is in a different module. Fault comes within a second of a known lifecycle event. | **Excluded.** Module is `CoreMessagingXP+0x97de6`, not Xaml `+0x3a9c5d`. WER P7 is `E_ABORT`, not `E_ILLEGAL_METHOD_CALL`. The fault came within about 1 s of `resources released`, not after a delay. The thread identity comes from the 09-10 report only, because the dump is gone. |
| 2 | A forced-stop ordering bug in the shell, of the engine-off deadlock (ZoomISO G4) kind: teardown touches the core or the SDK in the wrong order, or blocks the UI thread | A hang before the fault: UI thread blocked in `WaitForExit` or a supervisor gate, or a core stop that does not finish. The core dies first (an exit in `media-core.log` before the shell's stop). Fault inside the `ShutdownAsync` steps, before `resources released`. | Every `ShutdownAsync` step logs completion. The core stop returns quickly. The fault comes after `resources released`. | **Excluded as the deadlock class.** The stop took 337 ms, every step logged, and the fault came after the last step. **It IS a shutdown-ordering defect in a different sense:** the shell handed torn-down XAML back to WinUI's dispatcher drain. That is the defect #462 fixed. |
| 3 | Something new | A stack that matches neither of the above | — | **This one.** It is a new member of the 0xc000027b family: the post-`Exit` dispatcher drain on a graceful close. It was new on 2026-09-10 and has been documented since #462. |

**Limits of this classification.** The native and managed stacks come from the 2026-09-10
investigation. Without the dump they cannot be re-read. Three independent sources agree with that
report: the event 1000 module and offset, the WER P4/P7 signature (combase, E_ABORT), and the log
timing. None of them conflicts with it. Nothing on this box can do better: no dump, no matching
binaries, no PDBs. A re-read is possible only if the dump or the `beta-release` folder turns up
somewhere else, for example in a OneDrive version history or another machine. Task 1 of the plan
checks for that once and then stops looking.

## Tooling steps (for 17304 if it is found, or for any future shell shutdown dump)

Open the dump from a copy, never from the original: copy it and the exact publish folder
(exe + `CoreVideoPro.WinUI.pdb` + `*.dll`) to `D:\CoreVideo-QA\dumps\457\` first.

Native side (cdb):

```powershell
$cdb = "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe"
$dmp = "D:\CoreVideo-QA\dumps\457\CoreVideoPro.WinUI.exe.<pid>.dmp"
$sym = "srv*D:\symcache*https://msdl.microsoft.com/download/symbols;D:\CoreVideo-QA\dumps\457\publish"
& $cdb -z $dmp -y $sym -logo D:\CoreVideo-QA\dumps\457\native.txt -c "!analyze -v; .exr -1; .ecxr; kPn 60; ~; ~*kn 30; lmvm CoreMessagingXP; lmvm Microsoft_UI_Xaml; q"
```

Read in `native.txt`:
- **The exception record.** `ExceptionCode: c000027b`. `ExceptionInformation[0]` points to the
  stowed-exception array and `[1]` is its count. `!analyze -v` decodes it as
  `STOWED_EXCEPTION_<n>`, with the HRESULT and the stowed stack.
- **The faulting thread.** In `kPn 60`, check whether it is thread 0 / UI with `ShutdownQueue` and
  `DeferInvokeCallback` (the drain), or the finalizer with `RunFinalizers` (#513).
- **All threads.** In `~*kn 30`, look for a supervisor or `WaitForExit` frame (an ordering hang)
  and a live core-bridge reader.
- **`lmvm`.** Gives the exact WinAppSDK runtime version, for the upstream report.

Managed side (dotnet-dump; SOS inside cdb is the fallback):

```powershell
dotnet-dump analyze $dmp -c "clrthreads" -c "clrstack -all" -c "pe -nested" -c "syncblk" -c "finalizequeue" -c "exit" > D:\CoreVideo-QA\dumps\457\managed.txt
# fallback inside cdb:  .loadby sos coreclr; !threads; !clrstack -a; !pe -nested; !syncblk; !finalizequeue
```

Read in `managed.txt`:
- `clrthreads`: which OS thread is `(Finalizer)` and which is the STA UI thread. Is a managed
  exception present on the faulting thread? (The drain fail-fast has none, so `pe` is empty.)
- `clrstack -all`: whether `ShutdownAsync`, `ShutdownCompletion.Complete`, `ForceExit` or
  `Environment.Exit` is on any stack. Seeing `Environment.Exit` would point to the residual risk
  below.
- `syncblk`: managed locks held. For a deadlock-class crash you would expect the supervisor gate
  held by one thread and awaited by the UI thread.

For a #513-shaped dump, follow the crash-class doc: `!dumpobj` on the finalizer frame's `this` to
tell a plain `ObjectReference` apart from `ObjectReferenceWithContext`.

## What "done" means for this triage

1. A written classification against the three hypotheses, with the evidence row for each. This
   spec is that classification. The plan has it posted to #457.
2. One of these two:
   - **(a)** Close as fixed by #462. Evidence: the post-fix in-meeting long-close record above,
     plus no recurrence of the signature in Event Log.
   - **(b)** Keep open with a named capture plan, if the owner wants a planned, observed long
     close rather than the incidental ones.
3. The issue's factual errors corrected in the issue: the build was `46b07ff`, not `c36d80a`; it
   was a graceful close, not a forced-stop artefact; the dump is not retained.
4. A standing regression check exists, so the next occurrence is caught rather than inferred
   (see below).

## Fix proposal

The primary fix is already on main (#462). Nothing more is needed to close the issue. Two
follow-ups, for the owner to decide:

1. **Standing gate (recommended).** The RC acceptance soak ends with a graceful close of the real
   app, after the meeting has run 30 minutes or more. Send `WM_CLOSE` to the main window, which
   the no-input-injection rule allows. Pass means all of these:
   - `launch.log` gains `shutdown: cleanup complete; terminating the process directly`;
   - the process exits within 6 s;
   - no new `CoreVideoPro.WinUI.exe.*.dmp` appears;
   - no new event 1000 for `CoreVideoPro.WinUI.exe` appears within 60 s.

   This turns "no recurrence" from something inferred from logs into a gate. The existing
   `ShutdownCompletionTests` already pin the order and the gate at unit level.
2. **Residual risk in the fallback (decide, do not assume).** `ApplicationLifecycle.ForceExit`,
   used when cleanup fails or times out, still ends in `Environment.Exit`. That is `ExitProcess`,
   which runs `DLL_PROCESS_DETACH` in Microsoft.UI.Xaml and CoreMessagingXP. `ShutdownCompletion`'s
   own comment calls that state "even less defined." No fallback has fired in the retained logs,
   so the exposure is a rare path. One option: after the log line, `ForceExit` calls
   `ShutdownCompletion.TerminateCurrentProcess` instead of `Environment.Exit`. Pin it with a
   `ForceExit` seam test like `ATerminateThatReturnsFallsBack...`. This is a separate small PR,
   only if the owner wants it.

## Capture plan, if the signature recurs or the owner chooses (b)

- **Dump settings.** This box already writes full shell dumps (HKLM per-exe key, DumpType 2, count
  20, `%LOCALAPPDATA%\CrashDumps`). On an installed build from #1033 they land in
  `%LOCALAPPDATA%\CoreVideoPro\crashes`, DumpType 2, count 5. Check both folders. Check with
  `reg query "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\CoreVideoPro.WinUI.exe"`.
  If it is missing, run `scripts/setup-crash-dumps.ps1` (elevated, owner) or the installer's
  `Install-CrashDumps.ps1`.
- **Retention (the lesson of 17304).** The rotating DumpCount and build folders that get rebuilt
  are what lost this dump. As soon as a shell dump appears, copy it, the exact publish folder (exe,
  PDBs, DLLs) and the matching `launch.log` / `media-core.log` / `perf.log` to
  `D:\CoreVideo-QA\dumps\<issue>\`. Record the build stamp from the log's `build=` field. Analyze
  BEFORE rebuilding (the PDB rule).
- **Correlate.** Event 1000 (module+offset) and event 1001 (WER P4/P7) give the signature even
  when the dump is lost:
  `Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000,1001; StartTime=<t-5min>; EndTime=<t+5min>}`.
- **Optional, for a planned close.** Run the soak with `DOTNET_DbgEnableMiniDump` unset (WER owns
  the dump). There is no need for a debugger attached: `ShutdownCompletion` writes its last line
  synchronously, so the log alone shows which exit path ran.

## Out of scope

- The `Microsoft.UI.Xaml+0x3a9c5d` startup-crash cluster (2026-09-15 to 2026-10-16, including
  the 28528 dump still on disk). That is a different signature and needs its own issue if no
  existing one covers it.
- #513 exposure reduction.
- The T1.8 Zoom engine leave order on app close.
