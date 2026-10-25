# Graceful-close check (#457)

`scripts/qa/graceful-close-check.ps1` closes a long-running CoreVideo Pro
**once** and judges whether it ended on the clean exit path with no crash. It is
the standing regression gate for the post-Exit dispatcher-drain crash (#457,
fixed by #462): after a clean shutdown the shell must end with
`TerminateProcess`, never hand torn-down XAML back to WinUI. See
[the crash-class doc](../reference/winui-crash-class-0xc000027b.md) (T1.7).

**Who runs it:** the operator who owns the running session, at the end of their
own run. It closes the app. The RC acceptance soak runs it as its last step
(`docs/superpowers/plans/2026-10-23-rc-beta-acceptance-soak.md`, Task 5 Step 3);
do not schedule a separate meeting for it while the soak is running.

```powershell
# After 30+ minutes in the test meeting, Engine on, no recording, no stream:
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\graceful-close-check.ps1
```

| Parameter | Default | Notes |
|---|---|---|
| `-Out` | `D:\CoreVideo-QA\graceful-close\<timestamp>` | must be on D: |
| `-MinSessionMinutes` | 30 | the shell must have been up this long |
| `-AllowOutsideMeeting` | off | otherwise refuses unless `meetingState` is `in_meeting` |
| `-AllowVirtualCamera` | off | otherwise refuses while the virtual camera is `live` or `starting` |
| `-Api` | `http://127.0.0.1:8011` | control API, read only (`/state`, `/snapshot`) |

## Refusals (exit 2, nothing is closed)

- Not exactly one `CoreVideoPro.WinUI` process, or no control API answer.
- Recording or streaming live or finalizing (`Test-DrillSafeToClose`, shared
  with the outputs drill). The shell's close guard would prompt and activate its
  window. `engine on` is not a refusal here: the guard does not ask about it.
- Virtual camera `live` or `starting`. The close guard never asks about the
  camera, so the app would close under Zoom (or whatever uses the camera) with
  no prompt. That is the vcam-to-Zoom show this run looks like.
- Not exactly one visible, unowned top-level window. The shell opens unowned
  popouts (multiview, program/preview, audio mixer, diagnostics, production
  settings). `Process.CloseMainWindow` would send the close to whichever is on
  top. Close the popouts first. This is checked again just before the close.
- Session shorter than `-MinSessionMinutes`, or not in a meeting.

## What it does

1. Records the PID, the log session id (PIDs get reused, so everything keys on
   the session), the build stamp and the existing shell dumps in
   `%LOCALAPPDATA%\CrashDumps` and `%LOCALAPPDATA%\CoreVideoPro\crashes`.
2. Posts ONE `WM_CLOSE` to the main window. There is no SendKeys, focus change or
   UI Automation. It never sends a second close: a second close during cleanup
   makes the shell take the `ForceExit` path, which is not the exit under test.
3. Waits 6 s for exit (and up to 60 s more without sending anything), then
   watches until 60 s after the close.

## Verdict (`verdict.json`; exit 0 pass, 1 fail)

PASS needs all of these:

- this session's last `launch.log` line is
  `shutdown: cleanup complete; terminating the process directly`;
- the process exited within 6 s;
- no new `CoreVideoPro.WinUI.exe.*.dmp`;
- no new Application event 1000 for `CoreVideoPro.WinUI.exe`.

On a FAIL with a new dump, copy the dump, the exact publish folder (exe, PDBs,
DLLs) and the launch/media-core/perf logs to `D:\CoreVideo-QA\dumps\<issue>\`
**before** any rebuild, then follow the capture plan in the #457 spec.

On a FAIL where the shell is **still running** after 66 s, the shell's own 6 s
watchdog did not fire, so the watchdog thread itself is wedged and the hang is
the evidence. Do not kill the process first: create a full dump (Task Manager >
Details > `CoreVideoPro.WinUI.exe` > Create dump file, or `procdump -ma <pid>`),
copy it with the publish folder and logs as above, then end the process.

The check refuses (never closes) when `/state` or `/snapshot` is unreachable or
the snapshot has no `virtualCamera` block: an unknown camera state is treated as
live. It fails (never passes) when the Application event log cannot be read:
missing evidence is not clean evidence.

## Evidence so far

The 2026-10-23 census found 7 clean in-meeting closes after 41-612 minutes on
the fixed path. **All seven are on September builds** (2026-09-10 to
2026-09-13). The October closes in the retained logs were short or outside a
meeting. So the owner's first 30-minute run of this check (plan Task 5c, or
the soak's last step) is the first long in-meeting close on an October build.
Until then, "no recurrence" covers September builds only.

Tests: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\graceful-close-check.test.ps1`.
These cover the pure parts: log parsing, the verdict, the refusals and window
selection over a fake window list. Win32 window enumeration was smoke-tested on
explorer, not yet against a running shell.
