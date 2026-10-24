# Installer cleanup #883: prove the Get-FileHash fix, retire residual version folders — design

**Date:** 2026-10-23
**Status:** approved; owner rulings D4 and D5 recorded below (2026-10-23 23:50 EDT)
**Issue:** [#883](https://github.com/iamfatness/CoreVideoPro/issues/883) (OPEN, label `backlog`, no comments)
**Related:** #866 (retire previous installs), #863/#888 (PR #1026, staging root), #433 (PR #1033, crash dumps)

## Verdict

Issue #883 has two halves with different states.

**Half 1, "cleanup subprocess cannot resolve Get-FileHash": fixed in code, but not by #1026 or #1033.**
Commit `91db55bf` (2026-10-13, "Verify retired payload hashes without PowerShell command
lookup (#883)") replaced the `Get-FileHash` call on the retirement path with streamed
`[Security.Cryptography.SHA256]` (`scripts/alpha/PreviousInstallations.psm1:4-17`, call site
`:84`). The camera module does the same (`VirtualCameraRegistration.psm1:3-15`).
`Test-PreviousInstallations.ps1:2-9` masks `Get-FileHash` inside the planner module, and
`docs/reference/installer.md:21-24` records it. PR #1026 (build staging root) and PR #1033
(`Install-CrashDumps.ps1`, 64-bit registry view) do not touch this path; #1033 only repeats the
same WOW64 precedent.

Evidence the fix works in the standard installer: tonight's install of
`beta-2026-10-23-1072a50` retired `beta-2026-10-22-q10191cd6d1e`
(`previous-installations-cleanup.json` shows `status: removed`, nothing preserved).

**The root cause was never captured.** The issue asks to "capture its architecture and module
paths". Nobody did. We know only that the child could not resolve a `Microsoft.PowerShell.Utility`
cmdlet that resolves fine interactively. The fix removed the one symptom. The same script still
depends on command resolution for `Get-CimInstance` (CimCmdlets), `ConvertTo-Json` and
`ConvertFrom-Json` (Utility), `Get-ItemProperty`, `Get-ChildItem`, `Remove-Item` (Management) and
`New-Object -ComObject`. If the environment fault is real, the next cmdlet to fail is
unpredictable, and the failure would again read "retained, cause unknown".

**Half 2, "residual-folder cleanup": still open, and measurably so.** The second paragraph of the
issue asks the installer to distinguish registered-payload retirement from residual-folder
cleanup and never to report success from uninstall-entry count alone. Measured on the dev box
after tonight's correct install (`%LOCALAPPDATA%\Programs\CoreVideoPro`):

| Folders | Contents | Why they survive |
|---|---|---|
| 29 | only `.camera-desktop-registration-before.json` | Not in the metadata allowlist (`PreviousInstallations.psm1:131` lists `.camera-registration-before.json` but not the `-desktop-` file written by `CameraUserSession.ps1:13`). The folder can never become empty. |
| 2 | `Recordings\` (10 files total) | Operator data inside an install folder. Correctly preserved, but silently: no report, no recoverable location. |
| 1 | full current payload | `beta-2026-10-23-1072a50`, correct. |

Registered uninstall entries: 1. The entry-count signal the issue warns about reads "clean" while
32 stale folders remain. All 32 are unregistered, so `Get-PreviousInstallationPlan` (`:36-50`),
which enumerates registry keys, cannot see them. The 8 remnants the issue describes were moved by
hand into `backups\retired-install-remnants-<guid>`; the installer still does not do that.

## Goals

1. After setup, no superseded version folder under the install root remains unless it holds
   operator-owned or modified content; that content is moved to a recoverable backup (owner
   ruling pending, Q1) or reported with exact paths.
2. The cleanup helper's own environment is deterministic and diagnosed: a resolution failure
   names the cmdlet, host bitness, language mode, `PSModulePath` and `$PSHOME`, and is loud.
3. The root cause of the original failure is reproduced or, failing that, bounded by a test that
   deliberately corrupts the inherited environment.
4. Acceptance is an installed upgrade on a real machine, not unit tests.

## Non-goals

- Never run an older version's uninstaller (legacy camera removal can undo the new owner).
- Never terminate processes. A version with any running executable stays.
- No recursive delete anywhere. No change to camera ownership, shortcuts or user settings.
- No change to the 32-bit NSIS installer, the `$SYSDIR` (WOW64-redirected) launch, or
  `-NoProfile -ExecutionPolicy Bypass`. Hardening happens inside the script.

## Trace: how the cleanup subprocess is launched

`scripts/alpha/installer.nsi:267`:
`nsExec::ExecToStack '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\Remove-PreviousInstallations.ps1" -AppDirectory "$INSTDIR"'`

- NSIS is 32-bit, so `$SYSDIR` (`C:\Windows\System32`) is redirected by WOW64 to the **32-bit**
  `SysWOW64\WindowsPowerShell\v1.0\powershell.exe` (5.1).
- `-NoProfile`: no profile can break it and none can rescue it.
- `-ExecutionPolicy Bypass`: policy is not the cause (a policy failure names the script, not a cmdlet).
- The child **inherits the parent's environment** untouched; `nsExec` does not clear
  `PSModulePath`. The installer is started by Explorer, by `Start-Process` from an agent shell, or
  by `Test-AlphaInstaller.ps1`. If the launching shell is PowerShell 7, its `PSModulePath` (PS7
  `Modules` first) is inherited by the 5.1 child.
- Constrained Language Mode does not make `Get-FileHash` unresolvable, so it is a non-cause for
  this symptom. It would, however, make `New-Object -ComObject WScript.Shell`
  (`PreviousInstallations.psm1:109`) and the .NET hash fail loudly with a different message; the
  diagnostics block below makes language mode visible.

Leading hypothesis (unproven, hence Task 1): a PS7-launched installer hands the 5.1 child a
`PSModulePath` that makes `Microsoft.PowerShell.Utility` command discovery bind to, or fail
against, the wrong module. That is exactly the "installer child environment/module difference"
the issue names. Task 1 turns the hypothesis into a repro or kills it.

## Design

### A. Deterministic module environment (Remove-PreviousInstallations.ps1)

At the top, before any other statement:

1. Record a diagnostics block: `$PSVersionTable.PSVersion`, `[Environment]::Is64BitProcess`,
   `$ExecutionContext.SessionState.LanguageMode`, `$PSHOME`, `$env:PSModulePath`, `$PSCommandPath`.
2. Reset `$env:PSModulePath` for this process to `$PSHOME\Modules` plus
   `%WINDIR%\System32\WindowsPowerShell\v1.0\Modules`, dropping anything inherited.
3. `Import-Module` the built-in modules the script needs (`Microsoft.PowerShell.Utility`,
   `Microsoft.PowerShell.Management`, `CimCmdlets`) by **absolute manifest path** under
   `$PSHOME\Modules`, `-ErrorAction Stop`. A missing module throws naming the path.
4. `Get-Command` every cmdlet the script and module call (one fixed list); any miss throws
   `Setup helper cannot resolve <cmdlet> (module <m>). <diagnostics on one line>`.
5. Any terminating failure is appended to
   `%LOCALAPPDATA%\CoreVideoPro\installer-logs\<release>-retire.log` with the diagnostics block
   (same log convention as #1033's crash-dump log), then rethrown, so the exit code is nonzero and
   `installer.nsi:269-273` shows its warning.

`Get-FileHash` stays out of the path. Nothing in A changes what is deleted.

### B. Residual-folder sweep (PreviousInstallations.psm1)

New functions `Get-ResidualInstallationPlan` and `Remove-ResidualInstallation`, run by
`Remove-PreviousInstallations.ps1` **after** the registered retirement, so a folder just emptied
of payload but not removed is also handled.

**Candidates:** direct children of `%LOCALAPPDATA%\Programs\CoreVideoPro` (the install-root guard
at `Remove-PreviousInstallations.ps1:7` still applies) whose name matches
`^(alpha|beta)-[0-9]{4}-[0-9]{2}-[0-9]{2}-[a-z0-9]+$`, that are not the current directory, have
**no** registration key (registered folders keep the existing path and its `retained` semantics),
are not a reparse point, and have no running executable under them (same fail-closed process
inventory as today).

**Classification per file** (explicit-stack walk; reparse points are never entered and make that
folder `retained`):

| Class | Rule | Action |
|---|---|---|
| owned-metadata | root-level name in `Uninstall.exe`, `.camera-registration-before.json`, `.camera-desktop-registration-before.json`, `.corevideo-prerelease-install`, `build-manifest.json`, `previous-installations-cleanup.json` | delete |
| proven-payload | folder still has a valid `build-manifest.json` and the file's SHA-256 matches it | delete (same proof as the registered path) |
| unknown | anything else: modified payload, every file under `Recordings\` | backup (Q1) or retain and report |

The metadata names move into one shared function used by the registered and residual paths, so
they cannot drift again (`.camera-desktop-registration-before.json` was missed because the list
was inline).

**Backup (if Q1 is yes):** each unknown file is **moved** (never copy-then-delete; same volume
asserted, else retain) to
`%LOCALAPPDATA%\CoreVideoPro\backups\retired-install-remnants-<guid>\<releaseId>\<relative path>`.
`backup-manifest.json` in the backup root lists source, destination, length and SHA-256 per file;
it is written before the first move and rewritten after the last. Collisions get a numeric suffix,
never an overwrite. Afterwards empty directories are removed one at a time, non-recursively,
deepest first, then the version folder. Any failure leaves folder and backup intact and the folder
reported `retained` with the reason.

### C. Honest reporting

`previous-installations-cleanup.json` becomes `{summary, registered[], residual[]}`. `residual`
entries carry `releaseId`, `status` (`removed|backed-up|retained|skipped`), `reason`,
`backedUpTo`, `moved`. `summary` carries `registeredRetired`, `residualRemoved`,
`residualBackedUp`, `retained`. Exit code is 1 when anything is `retained` for an error reason
(locked file, running version, unsafe path). Unknown content under a "leave" ruling is `retained`
with reason `operator content: <n> files`, listed in the warning box, exit 0. The helper prints a
one-line summary last so NSIS captures it; the dialog counts folders, not uninstall entries. A copy
of the log goes to `installer-logs` so it survives the next upgrade's cleanup.

### D. Installer wiring (installer.nsi:267-273)

No launch change. Only the warning text changes: folder counts and the backup location.
`/SD IDOK` stays so silent setup never blocks.

## Owner rulings (2026-10-23 23:50 EDT)

- **D4 (answers Q1): move, never delete, and say so loudly.** Setup MOVES operator content
  (for example `Recordings\`) out of retired version folders into a dated backup folder with a
  manifest, never deletes it, and reports the backup path loudly in the installer message and
  in the cleanup JSON. `-LeaveUnknown` stays available for manual runs only.
- **D5 (answers Q2): yes.** The helper MAY reset `PSModulePath` for its own process only, and
  logs both the inherited and the effective value.

The original questions, kept for the record:

## Open questions (answered by D4/D5 above)

- **Q1.** When a stale version folder holds operator content (the two `Recordings\` folders, 10
  files), should setup **move it to the backup folder automatically** (matches the issue text and
  what was done by hand at the owner's request) or **leave it in place and only report**? The
  design assumes move-with-manifest; leave-and-report is the `-LeaveUnknown` flag.
- **Q2.** May the helper reset `PSModulePath` for its own process only? It cannot affect the
  parent or other processes. The design assumes yes.

## Testing strategy

Tests first, in the style of `Test-CrashDumps.ps1` and `Test-PreviousInstallations.ps1`:
disposable fixtures under `%TEMP%`, an isolated `HKCU:\Software\CoreVideoProInstallerTests\<guid>`
namespace, an `Assert` helper, every case also run from the 32-bit SysWOW64 host. New
`Test-CleanupEnvironment.ps1` and `Test-ResidualInstallations.ps1` join the `native-shell-windows`
CI step beside `Test-PreviousInstallations.ps1`.

Installed acceptance (plan Task 7): inventory and `-InspectOnly` before; standard interactive
install of the next beta; require exactly one version folder (plus, if Q1 is yes, one backup
folder whose manifest covers the 10 recording files), one uninstall entry, both shortcuts and
camera ownership on the new install, settings untouched, `summary.retained = 0`.

## Risks

- **Data loss in the sweep.** Mitigations: move-only, same-volume, manifest-before-move,
  non-recursive removal, reparse points never entered, install-root guard, release-id regex,
  registered folders untouched, running versions skipped, fail closed on process inventory.
- **False confidence about root cause.** Section A fixes the environment, but only a repro (Task 1)
  proves the cause. If Task 1 cannot reproduce, the closing comment must say "not reproduced;
  hardened and diagnosable".
- **The dev box is the owner's machine.** Acceptance touches the real install and camera and needs
  the owner's go-ahead; the no-input-injection rule applies.

## Findings (Task 1, 2026-10-24): REPRODUCED

The original failure is reproduced, through a real `nsExec::ExecToStack` of
`$SYSDIR\WindowsPowerShell\v1.0\powershell.exe` exactly as `installer.nsi` launches it, on both
the 32-bit (SysWOW64) and 64-bit Windows PowerShell 5.1.26100 hosts.

**Cause.** In Windows PowerShell 5.1, `Get-FileHash` (like `New-Guid`, `Format-Hex`,
`New-TemporaryFile`) is a *script function* in `Microsoft.PowerShell.Utility.psm1`, found only
by module auto-loading. Compiled cmdlets of the same module (`ConvertTo-Json`,
`ConvertFrom-Json`, `Select-Object`) are loaded with the default session and never need
discovery. A PowerShell 7 shell exports a `PSModulePath` with PS7's own `$PSHOME\Modules`
ahead of the Windows PowerShell paths. Recent pwsh versions strip those entries only when pwsh itself launches
`powershell.exe` directly; when it launches `setup.exe`, the polluted value passes through NSIS
untouched into the 5.1 child. Discovery then binds `Get-FileHash` to PS7's Core-only
`Microsoft.PowerShell.Utility` 7.0.0.0 manifest (a cmdlet in an assembly 5.1 cannot load) and
the command is reported `The term 'Get-FileHash' is not recognized`, the exact text in #883.
`Get-Command Get-FileHash` in an ordinary Windows PowerShell window succeeds, which is why the
manual retry worked.

**Variants tried (32-bit host unless noted):**

| `PSModulePath` given to the 5.1 child | `Get-FileHash` |
|---|---|
| inherited from cmd / Windows PowerShell | resolves |
| PS7 paths first, PS7 not installed (dirs absent) | resolves |
| PS7 paths only, absent | resolves |
| unset / empty | resolves |
| nonexistent directory | resolves |
| stray `Microsoft.PowerShell.Utility` 9.9.9 (exports nothing relevant) first | resolves |
| corrupt / unwritable / fresh module analysis cache | resolves |
| `__PSLockdownPolicy=4` | resolves (FullLanguage; not enforced without WDAC) |
| **PS7 7.4.6 `Modules` dir first (real pwsh layout)** | **NOT RECOGNIZED** (both hosts, also via nsExec) |
| PS7 `Modules` first, then helper resets `PSModulePath` to `$PSHOME\Modules;System32\...\Modules` in-process | resolves |

A hand-written look-alike of PS7's Utility manifest did not reproduce at the full-helper level;
PS7's manifest verbatim (signature stripped) does, so `Test-CleanupEnvironment.ps1` embeds it.
The test FAILS against the pre-fix module (`91db55bf^`: old version `retained`, reason
`The term 'Get-FileHash' is not recognized`, exit 1) and passes against the current one,
which proves it bites. It also runs a real PS7 `Modules` directory when one is installed (the CI
`windows-latest` image has pwsh 7).

**What is still not proven:** that the 2026-10-13 install was in fact launched from a PS7
shell. The launching environment was never captured. Every other variant tried resolves, and
pwsh 7 is not installed on the dev box today, so the agent shell that ran that install is the
plausible source. The closing comment must say "reproduced under a PS7-inherited environment;
the 10-13 launch environment itself was not captured".

**Bounding the real install root (read-only, 2026-10-24 03:00 EDT):**
`%LOCALAPPDATA%\Programs\CoreVideoPro` holds 33 folders: 29 with only
`.camera-desktop-registration-before.json`, 2 with `Recordings\` (10 + 2 files:
`beta-2026-10-17-089ece2`, `beta-2026-10-18-038594f`), and **2 full payloads** (880 files each):
`beta-2026-10-23-1072a50` (registered, current) and `beta-2026-10-22-q1022ecd2749`
(unregistered; it was not in the 2026-10-23 table). One uninstall entry. No CoreVideo process
running. The residual sweep would delete the hash-proven payload of the unregistered full
folder and back up anything else in it.
