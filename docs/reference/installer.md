# Installer: one entry point, and uninstall removes what it caused (T2.7/T2.1, 2026-09-12)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

Three rules, all learned from the owner's own machine:

Upgrade retirement (#865): after payload, camera ownership, registry and shortcuts
have committed, Setup runs `Remove-PreviousInstallations.ps1`. The standard per-user
install root retains one registered version. Custom QA destinations never clean
the real install root. Candidates require the exact uninstall registration,
publisher, version marker and package manifest; paths and ancestor reparse points
are checked before deletion. Running versions and unverifiable candidates remain
with a cleanup report and warning. Missing version folders permit removal of their
matching stale registration only. Cleanup removes hash-matching delivered files,
allowlisted first-run runtime files and version shortcuts, using nonrecursive
directory removal. Modified payload files and operator files remain. It never
invokes older uninstallers or changes webcam registration: legacy uninstallers
can otherwise unregister the newly installed camera. The upgrade helper tests use
an isolated registry namespace and temporary fixtures.

Payload hashes are streamed through .NET SHA-256 rather than relying on
`Get-FileHash` resolution in Setup's PowerShell helper (#883). The retirement
regression also runs with that command masked in the planner module on both
32-bit and 64-bit Windows PowerShell.
The allowlisted retirement metadata includes the previous cleanup report itself;
otherwise an otherwise empty version folder survives after its registration is gone.
Unknown operator files and modified payloads remain protected.

Failure rollback must move Setup's working directory out of `$INSTDIR` before
removing the owned payload and empty installation directory. `SetOutPath` sets
both the extraction destination and process working directory. A camera preflight
failure otherwise leaves an empty but undeletable install root until Setup exits;
the next attempt treats that root as occupied and refuses to retry. Full upgrade
validation forces camera preflight failure, checks previous installs remain, then
retries the same destination successfully.

- **ONE stable `CoreVideo Pro` shortcut** (desktop + Start menu), repointed at
  each install. The owner accumulated five version-named desktop shortcuts,
  launched a three-day-old build by mistake and filed a defect against code that
  already had the fix. The version-named shortcut survives in the Start menu
  ONLY. Uninstall REMOVES the stable pair rather than repointing it: choosing
  "the newest remaining release" means ranking sibling folders, and a wrong guess
  silently opens the wrong build — the exact failure this exists to stop.
- **Uninstall removes the first-run media runtime, from a MANIFEST.**
  `Install-MediaRuntime.ps1` records what it wrote to
  `notices\ffmpeg\installed-files.txt`; `un.RemoveMediaRuntime` deletes that set
  and nothing else. A manifest rather than a hard-coded list because the file set
  follows the pinned upstream build; a manifest rather than `RMDir /r` because
  `$INSTDIR` is an app folder an operator's files can land in. Measured before
  the fix: TWO uninstalled versions still holding 10 files / **195 MB each**.
  The loop refuses any manifest line that is absolute, rooted, or contains `..`
  — a file that decides what an uninstaller deletes is worth validating.
- **A refused uninstall SAYS WHY.** Refusing when the registry does not match is
  correct, but refusing silently left an operator with no Apps & Features entry
  and an uninstaller that appeared to do nothing.

**Two traps this cost, both worth remembering:**

1. **NEVER run an interactive helper from a silent installer.**
   `regsvr32` WITHOUT `/s` opens a modal result dialog, and
   `Register-VirtualCamera.cmd` deliberately omits `/s` so a tester who
   double-clicks it gets feedback. Calling that .cmd from the installer hung a
   silent install forever — two `regsvr32` processes waiting on a dialog nobody
   could see, until the 180 s `Test-AlphaInstaller` timeout. The installer calls
   `regsvr32 /s` directly. Install registers the virtual camera; **uninstall must
   unregister it**, before the payload delete, or a COM registration survives
   pointing at a deleted DLL and every app that enumerates cameras inherits a
   broken device.
2. **A UTF-8 BOM is three literal characters to NSIS.** Windows PowerShell 5.1's
   `Set-Content -Encoding UTF8` writes one, so the manifest's first path became
   `<BOM>ffmpeg.exe`, `Delete` removed nothing, and only that one file was left
   behind — a failure that looked like flaky file locking. Write manifests with
   `[IO.File]::WriteAllLines(..., New-Object Text.UTF8Encoding $false)`. Any file
   NSIS reads must be written BOM-free.

`Test-AlphaInstaller.ps1` is the gate and now asserts all of it:
`stableShortcutOnly`, `mediaRuntimeRemoved`, `manifestEscapeRefused`, alongside
the existing real silent install / runtime probe / duplicate rejection /
uninstall / user-file preservation.

**T2.1 / #433 is implemented (PR #1033, 2026-10-23); installed acceptance is pending.** Setup now
does all three:

- **Virtual camera** - registered by `Install-VirtualCamera.ps1` (already so; unchanged).
- **WER LocalDumps** - `Install-CrashDumps.ps1`, run in the `installed:` phase after the
  payload and camera are committed. Windows reads LocalDumps from HKLM only (measured
  2026-09-12: an HKCU key for a crashing test exe produced zero dumps), so this is the
  elevated step. Owner ruling 2026-10-23 permits the HKLM write. For each of
  `CoreVideoPro.WinUI.exe`, `corevideo-native.exe`, `corevideo-zoom-engine.exe` it writes
  `HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\<exe>` with
  `DumpFolder` = `%LOCALAPPDATA%\CoreVideoPro\crashes` (REG_EXPAND_SZ), `DumpType` = 2
  (full), `DumpCount` = 5, plus a `CoreVideoProInstaller` marker value. Interactive setup
  passes `-AllowElevation` (a RunAs child; this is a second UAC prompt after the camera's,
  and the welcome page says so); silent `/S` setup must already
  be elevated, exactly like the camera step. **It never fails setup**: a declined prompt
  or refused write exits 2, is `DetailPrint`ed, appended to
  `%LOCALAPPDATA%\CoreVideoPro\installer-logs\<release>-crashdumps.log`, shown in a
  warning box (interactive) and setup continues. An existing key WITHOUT the marker (for
  example one from `scripts/setup-crash-dumps.ps1`) is preserved, not overwritten.
  Uninstall removes only marked keys, and keeps them while another CoreVideo Pro version
  is still registered (the exe names are shared across versions).
- **64-bit registry view is REQUIRED.** The installer is a 32-bit NSIS binary, so the
  PowerShell it launches is the 32-bit SysWOW64 host, and the `HKLM:` PSDrive follows the
  process view: it would write `HKLM\SOFTWARE\WOW6432Node\...`, which WerFault never reads
  for 64-bit processes (zero dumps, silently). `Install-CrashDumps.ps1` therefore opens the
  hive with `OpenBaseKey(LocalMachine, Registry64)` for every read, write, marker check and
  removal, the same precedent as `Install-VirtualCamera.ps1` and
  `VirtualCameraRegistration.psm1` (`Get-CameraRuntimeRoot`). Never use `HKLM:`,
  `New-Item` or `Set-ItemProperty` against LocalDumps. An elevated child that reports no exit
  code is treated as unknown and verified by reading the 64-bit keys back.
- **Recording folder** - created if missing at the app's own default
  (`RecordingFolderPolicy`: the user's Videos known folder + `CoreVideo Pro`, else
  `%LOCALAPPDATA%\CoreVideo Pro`; NSIS `$VIDEOS`, so a redirected Videos folder is
  honoured). `CreateDirectory` never clobbers; uninstall preserves it.

Tests: `Test-CrashDumps.ps1` (runs unelevated against a throwaway HKCU key, including from the 32-bit SysWOW64 PowerShell to exercise WOW64 redirection; in CI
`native-shell-windows`) covers values/kinds, idempotence, foreign-key preservation,
shared-install retention, and the loud non-fatal refusal. `Test-AlphaInstaller.ps1` asserts
the recording folder, and either the HKLM keys (elevated run) or the logged refusal
(unelevated run). NOT verified without a real elevated install: the UAC RunAs path of
`-AllowElevation`, and an actual WER dump landing in the crashes folder.

## Frame Server refresh after a camera DLL upgrade (#781)

**Implemented 2026-10-24 (PR for #781); installed acceptance is pending (plan Task 7, owner-run).**
Spec: `docs/superpowers/specs/2026-10-23-frame-server-dll-refresh-design.md`; plan:
`docs/superpowers/plans/2026-10-23-frame-server-dll-refresh.md`.

Why: an in-proc camera DLL stays mapped in the `FrameServer` service host until that process exits, so after an
upgrade a correct registration (#783) could still be served by the OLD DLL (measured: PID 29648 served `8670d079`
while registry and core used `cd63ab88`). Camera validation could pass against the wrong DLL.

What Setup does now, inside the camera step's existing elevated block (no new UAC prompt, no new elevation path):

- After the Install registration is verified, after a Rollback restore, and on Uninstall after the keys are removed
  but **before** `Remove-UnreferencedCameraRuntime`, `Install-VirtualCamera.ps1` runs
  `Refresh-FrameServer.ps1 -Action Refresh -ExpectedDll <registered DLL, or empty>`.
- The decision is pure (`Get-FrameServerRefreshDecision` in `VirtualCameraRegistration.psm1`): `NotRunning`,
  `Current` (only the expected DLL is loaded) and `NotLoaded` make **zero** service calls; `Stale` (another CoreVideo
  camera DLL is loaded, or the serve log names one for this PID) and `Unknown` (no evidence) refresh. Observed modules
  outrank the serve log; a serve log last written before the instance started is not evidence (PID reuse).
- Refresh = `Stop-Service FrameServer -Force`, wait at most 20 s for the old PID+creation time to be gone (or replaced
  by a Windows demand-start), then start it again only if it was running. Never `Stop-Process`, never kills.
- **Exit codes.** `Refresh-FrameServer.ps1`: 0 refreshed/not needed, 3 not removed, 1 error.
  `Install-VirtualCamera.ps1` maps any refresh failure to **exit 3** = "registered and verified, Frame Server not
  refreshed". It never rolls back a verified registration for this; exit 1 stays the only rollback trigger. Exit 3
  prints a `WARNING:` line even with `-Quiet`; NSIS `DetailPrint`s it, appends to `<release>-setup.log`, and shows a
  warning box on interactive setup (close camera apps or restart Windows). Uninstall and the NSIS rollback path
  accept 3 the same way.
- Every outcome (including `not_needed`) is one line in
  `%LOCALAPPDATA%\CoreVideoPro\installer-logs\<release>-frameserver.log` (BOM-free; the compact result JSON is on the
  line). The elevated RunAs child cannot return stdout, so it leaves the result in
  `<app>\.camera-frameserver-refresh.json`, which the parent reads and deletes (NSIS deletes it too).
- **64-bit / Sysnative required.** The 32-bit NSIS host launches SysWOW64 PowerShell, which cannot enumerate a
  64-bit process's modules (measured: not even the user's own `explorer.exe`). `Get-NativePowerShellPath` returns
  `%windir%\Sysnative\WindowsPowerShell\v1.0\powershell.exe` from a 32-bit process on 64-bit Windows, else System32
  (never `$PSHOME`, so a pwsh 7 parent cannot leak in). The RunAs child, the refresh and Inspect all go through it, and
  `Refresh-FrameServer.ps1` re-invokes itself through Sysnative if started 32-bit. Note `[Environment]::Is64BitOS`
  does not exist in .NET Framework (it reads `$null`); use `Is64BitOperatingSystem`.
- **Lazy load.** A refreshed Frame Server loads the DLL on the next camera activation, so install time can only prove
  the stale image is gone. `servingIdentityVerified` stays `false` until a fresh `Serving module path=` line from the
  current FrameServer PID names the registered DLL. `Install-VirtualCamera.ps1 -Action Inspect` now computes it from
  that evidence (and the manifest hash) instead of a constant; unelevated, module inspection is `unverified` with a
  `moduleInspectionReason` and the serve log decides.
- Fault injection (tests only): `COREVIDEO_FRAMESERVER_FAULT=unstoppable` (or `-FrameServerFault unstoppable`) swaps
  in a built-in fake instance that refuses to stop; the real service is never touched in that mode.

Tests: `Test-FrameServerRefresh.ps1` (fake provider, 36 checks, plus a read-only real Inspect; CI runs it from the
64-bit and SysWOW64 hosts; `-IncludeInstalledInspect` adds the desktop-user Inspect locally) and the decision/parser
table in `Test-VirtualCameraOwnership.ps1` (51 cases, both hosts). `Test-AlphaInstaller.ps1` asserts the
frameserver log after a real silent install and, elevated, a fault-injected exit 3 with the registration intact.

**NOT verified until the owner-run acceptance (plan Task 7):** a real `Stop-Service FrameServer` with the camera open
(timing, consumer behaviour, Windows self-restart); the stale image actually disappearing on an upgrade; a fresh
`Serving module path=` line from the new PID; the NSIS warning box. The acceptance probe is
`scripts/qa/probe-frameserver-stop.ps1` (`-Mode Observe` read-only; `-Mode Stop -RestartAfter -AllowConsumers
-OutDir D:\...` elevated, 64-bit, owner present), then `scripts/qa/native-camera-continuity.py` after activating the
camera in Zoom. Reboot, fresh-user, alternate-admin and fleet rollout remain out of scope.
