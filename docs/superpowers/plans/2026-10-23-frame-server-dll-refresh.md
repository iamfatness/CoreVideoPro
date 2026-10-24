# Frame Server DLL Refresh Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After an upgrade, no Frame Server instance that loaded an older CoreVideo camera DLL survives Setup, so the next activation serves the registered DLL, and `Inspect` can say so from evidence.

**Architecture:** A pure decision (`Current` / `NotLoaded` / `Stale` / `Unknown` / `NotRunning`) plus a bounded `Stop-Service FrameServer` refresh, run in a 64-bit PowerShell from `Install-VirtualCamera.ps1`'s existing elevated machine block. Failure exits 3 (warn, keep the registration); 1 stays rollback.

**Tech Stack:** Windows PowerShell 5.1 scripts under `scripts/alpha/`, NSIS (`installer.nsi`, 32-bit), Python QA instrument `scripts/qa/camera_serving_evidence.py` (reused unchanged).

**Spec:** `docs/superpowers/specs/2026-10-23-frame-server-dll-refresh-design.md`

**Recommended executor:**

| Task | Model | Effort | Reason |
|---|---|---|---|
| 1 Probe stop semantics | opus | high | Interprets live service behaviour that gates the mechanism; fixture-free judgement. |
| 2 Pure decision + log parser | sonnet | medium | Table-driven pure code with explicit cases. |
| 3 Refresh script + fake seam | sonnet | high | Bounded polling and identity-change logic; tests carry the risk. |
| 4 Installer integration (exit 3, Sysnative, NSIS) | opus | high | Rollback/exit-code semantics across 32/64-bit and UAC; mistakes surface only on real installs. |
| 5 Real `Inspect` | sonnet | medium | Reuses Task 2/3 functions. |
| 6 Documentation | haiku | low | Mechanical doc edits from the spec. |
| 7 Installed upgrade acceptance | fable | high | Disruptive on the owner's box; a wrong call (false pass on stale serving DLL) is expensive to discover. |

**Estimated AI-assisted effort:** 1.0 + 1.5 + 2.0 + 2.5 + 0.75 + 0.5 + 1.5 = about 9.75 h (owner needed for Tasks 1 and 7).

## Global Constraints

- **Task 1 gates everything.** If `Stop-Service FrameServer` cannot remove the stale instance, or removal needs a process kill, STOP and report to the owner; do not add `Stop-Process` unasked.
- Installer stays a **32-bit NSIS** binary. Anything needing 64-bit semantics (module enumeration of the service host) runs through `Get-NativePowerShellPath` (Sysnative). Registry access stays `OpenBaseKey(..., Registry64)`, never `HKLM:` (pattern: `Install-CrashDumps.ps1`).
- The refresh runs only inside the already-elevated block: **no new UAC prompt, no new elevation path**. Code must behave identically with UAC disabled and on a normal PC.
- **No silent failure.** Every outcome is in the result JSON and `installer-logs\<release>-frameserver.log`; exit 3 shows a warning on interactive setup.
- A refresh failure never rolls back a verified registration. Exit 1 remains the only rollback trigger.
- `Unknown` evidence refreshes; `Current`, `NotLoaded` and `NotRunning` make **zero** service calls.
- Never kill a process. Never touch the production `vcam-serve.log` or SHM slot from a test (use temp paths; `COREVIDEO_VCAM_SHM_DIR` rule).
- Install time cannot prove the new DLL is serving (lazy load). Docs and JSON must say `servingIdentityVerified=false` until evidence of a fresh `Serving module path=` line exists.
- Write any file NSIS reads BOM-free. No `Get-FileHash` (use `Get-CameraPayloadSha256`).
- Never run the dev-box acceptance while the owner is in a call or streaming; ask first.

---

### Task 1: Probe what Stop-Service actually does (gates the plan)

**Files:**
- Create: `scripts/qa/probe-frameserver-stop.ps1` (throwaway evidence script; delete or keep under `scripts/qa/` if reusable)

**Interfaces:** Produces printed measurements only. No product code.

- [ ] **Step 1:** Write the probe: print `FrameServer` and `FrameServerMonitor` state, start type, dependents, PID, creation time, and whether the PID is shared with other services (`Win32_Service` by `ProcessId`).
- [ ] **Step 2:** Run from a 32-bit (SysWOW64) and a 64-bit PowerShell; record that `Get-Process -Id <pid> -Module` fails in 32-bit and works in 64-bit elevated.
- [ ] **Step 3:** With the camera open (owner present), time `Stop-Service FrameServer -Force` to `Stopped`; record dependents stopped, the consumer's behaviour, whether Windows restarts it, and whether the next activation creates a new PID with a fresh `Serving module path=` line.
- [ ] **Step 4:** Repeat once with an old-hash DLL deliberately loaded to confirm the stale image disappears.
- [ ] **Step 5 (decision):** All of "stops within 20 s, instance replaced, new load observed" -> continue. Otherwise STOP and report `BLOCKED` with the measurements.
- [ ] **Step 6:** Record the measurements as an "Outcome" section appended to the spec (not a commit by this plan's author unless asked).

---

### Task 2: Pure decision and serve-log parser

**Files:**
- Modify: `scripts/alpha/VirtualCameraRegistration.psm1` (add and export the functions below)
- Modify: `scripts/alpha/Test-VirtualCameraOwnership.ps1` (add cases, same `Assert-Decision` style)

**Interfaces:**
```powershell
Get-FrameServerRefreshDecision -ServiceState <string> -ExpectedDll <string> `
    -ModuleInspection <'observed'|'unavailable'> -LoadedPaths <string[]> -ServeLogPaths <string[]>
    # -> 'NotRunning' | 'Current' | 'NotLoaded' | 'Stale' | 'Unknown'
Read-ServeLogModulePaths -Lines <string[]> -ProcessId <int>   # -> string[] last-wins, PID filtered
Get-NativePowerShellPath                                       # -> Sysnative path on 32-bit-on-64, else $PSHOME
```

- [ ] **Step 1: Write the failing tests first.** One assertion per decision row in the spec; serve-log lines using the exact `[hh:mm:ss.mmm pid=N] Serving module path=...` shape; overlong and hostile lines ignored; `Get-NativePowerShellPath` equals Sysnative when `[Environment]::Is64BitProcess` is false on a 64-bit OS (parameterise the two booleans for testability).
- [ ] **Step 2:** Run the suite; confirm the new cases fail.
- [ ] **Step 3:** Implement. Path comparison is case-insensitive, trailing-slash tolerant, and only paths ending in `corevideo-virtualcam.dll` are considered.
- [ ] **Step 4:** Run from 64-bit and from `C:\Windows\SysWOW64\WindowsPowerShell\v1.0\powershell.exe`; both green.

---

### Task 3: The refresh script on a fake service seam

**Files:**
- Create: `scripts/alpha/Refresh-FrameServer.ps1`
- Create: `scripts/alpha/Test-FrameServerRefresh.ps1`
- Modify: `.github/workflows/ci.yml` (run the test beside `Test-CrashDumps.ps1`, line ~282)

**Interfaces:**
- Consumes: Task 2 functions.
- Produces: `Refresh-FrameServer.ps1 -Action Inspect|Refresh -ExpectedDll <p> [-ServeLog <p>] [-Provider <hashtable>]`; emits one JSON object `{schema:'frameserver-refresh-v1', decision, result:'not_needed'|'refreshed'|'not_removed'|'error', before:{pid,created}, after:{pid,created}, moduleInspection, servedPaths, durationMs, servingIdentityVerified:false}`; exit 0 / 3 / 1.

- [ ] **Step 1: Write the failing tests first** with a provider fake: stale -> `refreshed` with changed creation time; will-not-stop within the bounded wait -> exit 3, no kill call exists to assert; service replaced by Windows between steps; stopped stays stopped; `Current` / `NotLoaded` / `NotRunning` -> zero Stop/Start calls; `Unknown` -> refreshes; stop throws access denied -> exit 3 with the message in JSON.
- [ ] **Step 2:** Run; confirm failures.
- [ ] **Step 3:** Implement with real default providers (`Get-CimInstance Win32_Service` / `Win32_Process`, `Stop-Service -Force`, `Start-Service`, `Get-Process -Module`, a bounded `ReadServeLog` that reads at most the last 1 MB) and a 20 s stop wait, 20 s start wait. Restore only services that were `Running`.
- [ ] **Step 4:** Require 64-bit: if `[Environment]::Is64BitOS -and -not [Environment]::Is64BitProcess`, re-invoke through `Get-NativePowerShellPath` and relay JSON and exit code.
- [ ] **Step 5:** Run the suite from both PowerShell hosts; add to CI; confirm green.

---

### Task 4: Installer integration

**Files:**
- Modify: `scripts/alpha/Install-VirtualCamera.ps1`
- Modify: `scripts/alpha/installer.nsi` (install and uninstall camera result handling; welcome text one line)
- Modify: `scripts/alpha/Test-AlphaInstaller.ps1`, `scripts/alpha/Test-AlphaPackage.ps1`

**Interfaces:**
- Consumes: `Refresh-FrameServer.ps1`, `Get-NativePowerShellPath`.
- Produces: `frameServerRefresh` object in the install JSON; exit 3 = "registered, refresh failed"; log `installer-logs\<release>-frameserver.log`.

- [ ] **Step 1: Write the failing tests first.** In `Test-AlphaInstaller.ps1` assert the result carries `frameServerRefresh` and that an injected non-removable service (env var read only by the test provider path, never in production) yields exit 3 with the registration intact and the log line present. Assert `Refresh-FrameServer.ps1` ships in the package manifest.
- [ ] **Step 2:** After `Set-CameraMachineRegistration` verification (Install), after registration restore (Rollback) and before `Remove-UnreferencedCameraRuntime` (Uninstall), call the refresh via the native PowerShell with the registered DLL path. Add `-MachineOnly` elevated child launch via `Get-NativePowerShellPath` too.
- [ ] **Step 3:** Map exit codes: child exit 3 is a warning, not a throw; the user-session registration still proceeds; the script exits 3 at the end. Write the log file; print the loud line even with `-Quiet`.
- [ ] **Step 4:** NSIS: after the camera `nsExec`, branch `$1 == 3` to a path that `DetailPrint`s, appends to the setup log, shows a `MessageBox` unless silent, keeps `CameraChanged 1`, and continues. Mirror in `un.camera_result` (uninstall continues, reports files retained in use).
- [ ] **Step 5:** Run the installer test elevated and unelevated; run the existing forced-camera-failure path and confirm rollback still occurs on exit 1.

---

### Task 5: A real `Inspect`

**Files:**
- Modify: `scripts/alpha/Install-VirtualCamera.ps1` (the `Inspect` block)
- Modify: `scripts/alpha/Test-CameraUserSession.ps1`, `Test-CameraUserSessionTransport.ps1` only if their `servingIdentityVerified` assertions need the new field shape

**Interfaces:** `Inspect` calls `Refresh-FrameServer.ps1 -Action Inspect` and reports `moduleInspection`, `servedPaths`, `loadedIdentityMatchesRegistration`; `servingIdentityVerified` becomes true only when the serve log shows the expected path from the current FrameServer PID (same rule as `judge_serving_identity`).

- [ ] **Step 1: Write the failing test first:** a temp serve log with the expected path for the fake PID -> true; wrong path, other PID, or no lines -> false; module API failing never yields true.
- [ ] **Step 2:** Replace the swallowed `try/catch` with the Task 3 inspect path; keep `unverified` as an explicit value with the reason.
- [ ] **Step 3:** Run the existing user-session tests; their "never claims delivery" assertions must still pass (delivery stays unverified).

---

### Task 6: Documentation

**Files:**
- Modify: `docs/reference/installer.md`, `docs/reference/virtual-camera.md`, `CLAUDE.md` (one-line pointer), `docs/BACKLOG.md` (#781 row status)

- [ ] **Step 1:** Add the refresh behaviour, exit 3, the 64-bit/Sysnative requirement, the lazy-load caveat and the acceptance command to `installer.md`.
- [ ] **Step 2:** Replace the "restart Windows if an older DLL remains" note in `virtual-camera.md` with the mechanism and how to verify with `camera_serving_evidence.py`.
- [ ] **Step 3:** State in each doc what is NOT verified until Task 7 passes.

---

### Task 7: Installed upgrade acceptance on the dev box

**Files:**
- Create: `artifacts/frame-server-refresh-<date>/` (evidence: pre/post FrameServer identity, installer logs, `vcam-serve.log` excerpts, continuity report)

**Interfaces:** Consumes the released candidate installer and `scripts/qa/native-camera-continuity.py`.

- [ ] **Step 1:** Ask the owner for a window (no call, no stream). Record installed version, camera DLL SHA256, FrameServer PID and creation time; open the camera and capture the pre-upgrade `Serving module path=` line.
- [ ] **Step 2:** Run the candidate (different camera DLL hash) interactively; save the log and JSON. Confirm `refreshed` and a changed PID or creation time.
- [ ] **Step 3:** Open the app by the stable shortcut, activate the camera in Zoom, then run `native-camera-continuity.py`; require `servingIdentityVerified=true`, the new SHA256, and a `Serving module path=` line from the new PID.
- [ ] **Step 4:** Re-run the same installer: confirm `Current` and an unchanged PID. Repeat the upgrade once silently elevated.
- [ ] **Step 5:** Run the fault-injection control; confirm exit 3, warning, log and intact registration.
- [ ] **Step 6:** Write the outcome in the spec, update the #781 issue comment text for the owner (do not close the issue; reboot, fresh-user and fleet remain open).

---

## Self-review

- Every spec requirement maps to a task: decision (2), refresh (3), install/rollback/uninstall + exit 3 + NSIS (4), real Inspect (5), docs (6), acceptance (7), probe of the unmeasured assumptions (1).
- Tests precede implementation in Tasks 2-5. The seam (provider) exists so no test touches the real service or the production serve log.
- Unresolved until Task 1: dependent services, shared host, hang behaviour with an active consumer.
