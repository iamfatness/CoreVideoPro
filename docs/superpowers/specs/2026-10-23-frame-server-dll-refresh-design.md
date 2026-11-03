# Frame Server DLL refresh: an upgrade must leave the new camera DLL serving — design

**Date:** 2026-10-23
**Status:** proposed design, for owner approval
**Issue:** [#781](https://github.com/iamfatness/CoreVideoPro/issues/781) (camera installation prerequisite for #517)
**Related:** #783 (protected machine runtime, ownership-aware registration; merged), #433 / `Install-CrashDumps.ps1` (the 32-bit installer / 64-bit seam pattern), `docs/reference/installer.md`, `docs/reference/virtual-camera.md`

## Why

#783 made the camera *registration* correct after an upgrade: a hash-versioned, LocalService-readable DLL under the
machine runtime root, the HKLM `InprocServer32` entry pointing at it, and the per-user alias aligned. It deliberately
left one thing open: nothing makes the Windows Frame Server **load** that DLL.

Evidence, from the issue thread and `docs/BACKLOG.md`:

- **Upgrade qualification on `beta-2026-10-14-4366d5b`:** Setup exited 0, all 858 manifest hashes matched, both camera
  ownership records pointed at the new version. The first camera trial was nonetheless INVALID: `FrameServer`
  PID 29648 still served the old DLL `8670d079` while registry and core activation used the new `cd63ab88`. The
  issue records this as the concrete demonstration that "registered owner and loaded serving module can differ after
  upgrade". Only a manually authorised, elevated Frame Server refresh fixed it; the repeat trial passed
  (2,701 samples, zero identity losses).
- **An earlier failed trial** ran an older DLL (`12e2c3...`) and produced 28 receiver repeat/skip pairs matching 28
  contended reads in the serve log. The corrected trial changed service instance and tracing together, so the
  contention cause is not isolated, but "stale DLL served" was a confound in the evidence.
- **The October 4/5 drift:** the per-user alias pointed at October 4 while the machine owner was October 5.
- **`Install-VirtualCamera.ps1 -Action Inspect` cannot see this today.** It hard-codes `servingIdentityVerified=$false`
  and its `Get-Process -Module` sits in a `try/catch` that reports `unverified`. The installer is a 32-bit NSIS binary
  that launches the 32-bit SysWOW64 PowerShell, and a 32-bit process cannot enumerate the modules of the 64-bit
  service host, so the probe fails by construction in the exact host the installer uses.
- Install's final JSON says "Close camera consumers or restart Windows if an older DLL remains loaded". That is a
  note, not a mechanism, and nothing checks it.

The result: **camera validation can pass against the wrong DLL.** After an upgrade the old camera code keeps serving
until the service happens to recycle. Serving provenance can *detect* this after the fact; no product step
*prevents* it.

### What already reports serving provenance (reused, not rebuilt)

- `native/virtualcam-dll/dllmain.cpp` logs `Serving module path=<executing DLL>` from `ClassFactory::CreateInstance`
  (the DLL's own `GetModuleFileNameW`, not a registry claim) into `%ProgramData%\CoreVideoPro\vcam-serve.log`, with the
  Frame Server `pid=` in each line.
- `scripts/qa/camera_serving_evidence.py` (`ServeLogCapture`, `service_identity`, `judge_serving_identity`) fixes the
  `FrameServer` PID + process creation time, parses those lines, and fails a trial when the served path or SHA256 is
  not the installed candidate. `native-camera-continuity.py`, `program-display-copy-probe.py` and
  `outputs-drill.ps1` consume it; the support bundle already ships `vcam-serve.log`.

That is the *acceptance instrument*. This spec adds the *action* and an install-time check using the same signal.

## Scope

**In:**

1. A refresh step, run by the installer during the camera machine change, that makes any Frame Server instance
   which loaded a different CoreVideo camera DLL exit, so the next camera activation loads the registered one.
2. A conservative, idempotent decision: refresh only when the running instance is stale or cannot be shown
   current; do nothing when `FrameServer` is stopped or provably serves the registered DLL.
3. Install, Rollback and Uninstall use the same step, so a removed owner's DLL is not left locked (#783 reports
   `retained_in_use_or_reference`; refreshing before `Remove-UnreferencedCameraRuntime` resolves it).
4. A real `Inspect`: 64-bit-correct module enumeration plus the serve log, setting `servingIdentityVerified` from
   evidence instead of a constant.
5. Loud, durable outcomes: result JSON, `DetailPrint`, a log under `%LOCALAPPDATA%\CoreVideoPro\installer-logs\`, and a
   warning box on interactive setup when the refresh fails.
6. Tests on a fake service seam; an installed acceptance on the dev box.

**Out (named, not done here):**

- A *runtime* stale-service warning in the app/core. The core is unelevated and cannot open the LocalService service
  host for module inspection; reading the serve log from the shell is a separate small follow-up (file an issue after acceptance).
- Hot-swapping the DLL under a live consumer; any change to the DLL, COM registration, GUID or MF registration
  (`MFCreateVirtualCamera`, `MFVirtualCameraLifetime_Session`, `MFVirtualCameraAccess_CurrentUser` stay as they are).
- Reboot / cached-consumer / fresh-user / alternate-admin qualification and fleet rollout (open in #783's comment;
  this change makes them testable but does not claim them).
- Killing processes (see mechanisms).

## What Windows actually requires (and what is only measured)

Documented (Microsoft Learn, *Frame Server Custom Media Source*, ms.date 2025-05-08): a Frame Server custom media
source "runs as Local Service" and "runs in Session 0 (System Service session) and can't interact with the user
desktop". That matches our design (HKLM registration, LocalService-readable protected DLL, file-backed SHM across
sessions). The page does not document when the service unloads a source DLL, and the `MFCreateVirtualCamera`
reference page returned 404 during research. The claims below are therefore **inference from measured behaviour,
to be confirmed by Task 1**, not citations:

- The media source is an in-proc COM server loaded into the `FrameServer` service host. A loaded image stays mapped
  until that process exits; COM unloads only through `DllCanUnloadNow` / `CoFreeUnusedLibraries`, which a service
  host does not drive. Evidence: PID 29648 kept `8670d079` while the registry named `cd63ab88`.
- Removing the session virtual camera (`IMFVirtualCamera::Remove`, which the core already calls on stop) detaches
  the camera but is not a documented guarantee of image unload; the stale image outlived core restarts.
- `FrameServer` is demand-started by camera consumers, so stopping it is recoverable by Windows. It is not boot-critical.

## Mechanisms considered

| Option | Verdict |
|---|---|
| A. `Stop-Service FrameServer` (with dependents), wait, restore prior state | **Chosen.** This is what the owner-authorised manual refresh did, and it worked (new service instance, new DLL). Supported API, no process kill, bounded, Windows demand-starts it again. |
| B. MF re-registration (`Remove()` + new `MFCreateVirtualCamera`) | Rejected as the fix: unload is not guaranteed (evidence above) and it needs the core running as the user, which the installer deliberately prevents. Stays as runtime behaviour. |
| C. Kill the service-host PID | Rejected as default: abrupt for every other camera consumer, and the host may be shared. Only an opt-in `-ForceTerminate` if Task 1 shows Stop can hang, still reported loudly. |
| D. A unique CLSID or path per release so collision cannot occur | Rejected: the CLSID is the identity in `MFCreateVirtualCamera` and in the ownership records; per-release CLSIDs accumulate machine registrations and break #783's ownership model. |
| E. "Reboot to finish" prompt only | Rejected as the only answer: it is today's behaviour with a footnote and is unacceptable on a production machine. It remains the fallback message when Stop fails. |
| F. RunOnce at next login | Rejected: defers the defect past the validation the upgrade exists to enable and adds a second elevated actor. |

## Chosen mechanism

`scripts/alpha/Refresh-FrameServer.ps1 -Action Inspect|Refresh -ExpectedDll <path> [-ServeLog <path>]`, run in a
**64-bit** PowerShell, elevated, from inside `Install-VirtualCamera.ps1`'s existing elevated machine block (same UAC
prompt; no second prompt, unlike Crash Dumps). Pure logic lives in `VirtualCameraRegistration.psm1`.

1. **Identify** the `FrameServer` service: state, PID, process creation time (the same identity
   `camera_serving_evidence.py` uses, so both agree on "the instance").
2. **Decide** (pure function, unit tested):
   - not running -> `NotRunning`, nothing to do;
   - modules enumerated, the CoreVideo camera DLL loaded only from the expected path -> `Current`;
   - modules enumerated, none loaded -> `NotLoaded` (nothing stale; do nothing);
   - modules enumerated, a CoreVideo camera DLL from any other path -> `Stale`;
   - modules unavailable: use the serve-log lines for this PID; a last path other than expected -> `Stale`;
     no evidence either way -> `Unknown`, which **refreshes** (a needless restart is cheaper than a wrong serving DLL).
3. **Refresh** (`Stale` / `Unknown`): `Stop-Service FrameServer -Force` (also stops dependents such as
   `FrameServerMonitor`), poll to `Stopped` for at most 20 s and confirm the old PID/creation time is gone; then
   start the services that were running before and confirm a *different* creation time. Never `Stop-Process`.
4. **Exit codes:** 0 refreshed or not needed; 3 = the stale instance could not be removed in time (service would
   not stop, or access denied); anything else is a real error.
5. A refreshed `FrameServer` loads the DLL **lazily** on the next camera activation. Install-time success is "the
   stale image is provably gone". "The new image is serving" is proven by the first activation line in
   `vcam-serve.log`, which is the acceptance criterion.

### Architecture seams (owner constraints)

- **32-bit NSIS / 64-bit view.** Mirror `Install-CrashDumps.ps1`: registry access via `OpenBaseKey(..., Registry64)`
  (already true for the camera), plus a new `Get-NativePowerShellPath` returning
  `%windir%\Sysnative\WindowsPowerShell\v1.0\powershell.exe` when the OS is 64-bit and the process is not, else
  `$PSHOME\powershell.exe`. The elevated child launch and the refresh both go through it, because `Get-Process
  -Module` against a 64-bit service host is impossible from SysWOW64 PowerShell (the present `Inspect` defect).
- **UAC.** The refresh is reachable only inside the already-elevated machine block. With UAC disabled the process is
  always admin and the path is identical. On a normal PC the camera step's existing prompt is the only prompt, and a
  declined prompt means nothing machine-side changed, as today. Silent `/S` must still be pre-elevated.
- **Failure is loud and non-destructive.** A refresh failure does NOT roll back an otherwise correct registration
  (files and ownership are right; only the running instance is stale). `Install-VirtualCamera.ps1` exits 3 with
  `frameServerRefresh.result` in the JSON; NSIS treats 3 as "installed, camera not yet serving the new DLL":
  `DetailPrint`, a line in `installer-logs\<release>-frameserver.log`, and (interactive) a `MessageBox` telling the
  operator to close camera apps or restart Windows. Exit 1 still means failure and rollback.
- **Consumers.** Running CoreVideo is already refused by the machine block. Other consumers lose their Frame Server
  session briefly; the refresh is skipped whenever the instance is provably current, so a same-hash reinstall disturbs no one.

## Acceptance criterion

On the dev box perform a **real upgrade** from the installed beta to a candidate whose camera DLL SHA256 differs:

1. Before: open the camera so `vcam-serve.log` shows `pid=<P> Serving module path=<old hash dir>`; record the
   FrameServer PID and creation time.
2. Run the candidate installer (interactive, and separately silent elevated).
3. After: installer log and result JSON report `refreshed`; FrameServer PID or creation time differs from the
   recorded one; `Inspect` never reports `servingIdentityVerified=true` before the first activation.
4. Activate the camera through Zoom and the existing instrument: `scripts/qa/native-camera-continuity.py` must
   report `servingIdentityVerified=true`, the **new** SHA256, a fresh `Serving module path=` line from the new
   service PID, and no regression in the continuity verdict.
5. Re-run the same build: refresh is a no-op (`Current`, PID unchanged).
6. Negative control (fault injection): an unstoppable service yields exit 3, a warning and a log, and the
   registration is intact.

Evidence is retained under `artifacts/` with failed attempts, per repo practice. A pass that did not show the new
serving path from a new PID does not count. This does not claim reboot, fresh-user or fleet acceptance.

## Testing

- **Pure:** `Get-FrameServerRefreshDecision` over the table in step 2, including module-API failure falling back to the
  serve log and then to `Unknown`; serve-log parsing (PID filter, last line wins, hostile or overlong lines, unrelated modules).
- **Service seam, no real service touched:** a provider hashtable of scriptblocks (`GetService`, `Stop`, `Start`,
  `GetModules`, `ReadServeLog`). Cases: stale -> stop/start/creation changed -> refreshed; will-not-stop -> exit 3 and
  nothing killed; Windows restarts the service between steps; a stopped service stays stopped; current -> zero service
  calls (deleting the decision must fail a test).
- **Bitness:** run the suite from the 32-bit SysWOW64 PowerShell as `Test-CrashDumps.ps1` does, asserting
  `Get-NativePowerShellPath` resolves to Sysnative there.
- **Installer:** `Test-AlphaInstaller.ps1` asserts the refresh result is present and, unelevated, the logged refusal;
  NSIS handling of exit 3 uses the existing fixture failure paths.
- **Not testable in CI:** a real FrameServer restart. That is what acceptance covers.

## Risks, named

- **Stop semantics unmeasured.** Dependents, hang behaviour with an active consumer, and whether the host is shared are
  assumptions until Task 1; the plan stops and reports if Stop cannot remove the instance.
- **Other consumers.** A restart briefly drops other apps' camera sessions. Mitigations: skip when current, never on
  the running-core path, say so in the welcome text.
- **Over-eager `Unknown`.** A restart on missing evidence may be needless; accepted, because the alternative is the defect.
- **Policy or EDR blocking service control or module inspection** (the agent environment already blocked elevated module
  inspection). The code reports `unverified` / exit 3, never a pass.
- **Lazy load** means install cannot prove the new image; only acceptance step 4 can. The docs must say so.

## Outcome: Task 1 probe (2026-10-23, partial; Stop half left for acceptance)

Measured on the dev box with `scripts/qa/probe-frameserver-stop.ps1 -Mode Observe`, unelevated, Zoom running (camera not
open), CoreVideo not running:

- `FrameServer` and `FrameServerMonitor`: both `Stopped`, start type `Manual`, each depends only on `rpcss`, neither has
  dependents. Image paths are separate service groups (`svchost.exe -k Camera` and `-k CameraMonitor`), so the camera
  host is not expected to be shared with unrelated services; `sharedHostServices` is recorded on a running instance.
- Bitness, measured: from the 32-bit SysWOW64 PowerShell, `Get-Process -Module` fails even on the user's own 64-bit
  `explorer.exe` ("Cannot enumerate the modules"); from the 64-bit PowerShell it lists 409 modules for `explorer.exe`
  but still fails on a LocalService `svchost` unelevated. So module evidence needs **64-bit and elevated**; the
  unelevated `Inspect` must fall back to the serve log.
- `[Environment]::Is64BitOS` does not exist in .NET Framework (it evaluates to `$null` in Windows PowerShell 5.1);
  the property is `Is64BitOperatingSystem`. The implementation uses that.

**Not measured (gate still open):** Steps 3-4 (timed `Stop-Service FrameServer -Force` with the camera open, consumer
behaviour, Windows self-restart, new PID + fresh `Serving module path=` line, stale image gone) were NOT run: the
brief allowed a stop only with no Zoom/CoreVideo process and the camera unused, and Zoom was running; it also needs
elevation. The owner runs them at acceptance (plan Task 7, step 0) with
`scripts/qa/probe-frameserver-stop.ps1 -Mode Stop -RestartAfter -AllowConsumers -OutDir D:\...` from an elevated
64-bit PowerShell. The implementation proceeds on the issue's evidence that an owner-authorised manual Stop refresh
replaced the instance and the repeat trial passed; if the probe shows Stop cannot remove the instance within 20 s,
the mechanism is wrong and the PR must not merge.
