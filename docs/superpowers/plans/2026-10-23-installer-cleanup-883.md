# Installer Cleanup #883 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Setup's previous-version cleanup runs in a deterministic, diagnosed PowerShell environment, and after an upgrade no superseded version folder is left behind, with operator content backed up and reported rather than silently stranded.

**Architecture:** `Remove-PreviousInstallations.ps1` pins its module environment and logs diagnostics, runs the existing registry-driven retirement, then a new folder-driven residual sweep in `PreviousInstallations.psm1` that classifies every file as owned-metadata, proven-payload or unknown. Unknown files are moved (never deleted) to a backup folder with a manifest. One shared metadata list serves both paths.

**Tech Stack:** Windows PowerShell 5.1 (32-bit SysWOW64 host as launched by 32-bit NSIS, and 64-bit), NSIS (`scripts/alpha/installer.nsi`), Pester-style script tests with an `Assert` helper (no Pester module dependency, like `Test-CrashDumps.ps1`).

**Spec:** `docs/superpowers/specs/2026-10-23-installer-cleanup-883-design.md`

## Global Constraints

- **Installer stays 32-bit NSIS.** Helpers run as the 32-bit SysWOW64 PowerShell 5.1 unless a test launches Sysnative on purpose. Every new test runs under BOTH hosts (`$env:WINDIR\SysWOW64\...` and `System32\...`). Never introduce `HKLM:` writes.
- **UAC is disabled on the dev PC but code must work on a normal PC.** Nothing here needs elevation (all paths under `%LOCALAPPDATA%` and `HKCU`). Do not add an elevation requirement.
- **Loud failures.** No `-ErrorAction SilentlyContinue` on anything that decides deletion or moving. Every refusal names the path and reason. No empty `catch`.
- **No recursive deletion, ever.** `Remove-Item` without `-Recurse`, one file or one empty directory at a time. Unknown content is MOVED, same volume only, manifest written first.
- **Never terminate processes, never run an older uninstaller, never change camera ownership or shortcuts.** Fail closed if process inventory is unavailable (existing behavior, `Remove-PreviousInstallations.ps1:13-17`).
- **Do not use `Get-FileHash` on the setup path.** Use `Get-InstallPayloadSha256`.
- **One metadata list.** `Get-OwnedInstallMetadataNames` is the only place the names live.
- Tests create fixtures under `%TEMP%\CoreVideoRetirement-<guid>` and `HKCU:\Software\CoreVideoProInstallerTests\<guid>` only. Never touch the real `%LOCALAPPDATA%\Programs\CoreVideoPro` outside Task 7.
- Never run Task 7 while the owner's app is streaming, recording or has the virtual camera on; never SendKeys or steal focus.
- Task 4 is blocked on owner rulings Q1 and Q2 in the spec. Tasks 1-3 are not.
- Do not commit until the owner asks; commit messages end with the harness-supplied attribution line.

## Review Focus

- **Anything that deletes or moves a file.** Trace each path from classification to action; confirm a reparse point, a path outside the version folder, a running version or a registered folder can never reach it.
- **`Recordings\` classification.** It must be `unknown`, never `owned-metadata`; a metadata-named file below the root is also `unknown`.
- **Backup ordering.** Manifest written before the first move; a crash mid-sweep leaves every file either at its source or at a destination listed in the manifest.
- **Exit codes and the NSIS message.** `retained` for an error reason exits 1; the warning counts folders, not uninstall entries.
- **WOW64.** Both hosts pass; nothing assumes 64-bit `$env:ProgramFiles` or an unredirected `$SYSDIR`.

---

### Task 1: Reproduce or bound the original failure (gates the claim "root-caused")

**Recommended executor:** Opus 5, high effort (investigation; the hypothesis may be wrong).

**Files:**
- Create: `scripts/alpha/Test-CleanupEnvironment.ps1`
- Scratch, not committed: `%TEMP%\cvp-883-repro\*`

**Interfaces:** produces nothing for later tasks except the polluted `PSModulePath` value used by Task 2's test; consumes `Remove-PreviousInstallations.ps1`.

- [ ] **Step 1: Capture the real environment.** Write a throwaway script printing `$PSVersionTable.PSVersion`, `[Environment]::Is64BitProcess`, `$PSHOME`, `$env:PSModulePath`, `$ExecutionContext.SessionState.LanguageMode` and `(Get-Command Get-FileHash -ErrorAction SilentlyContinue).Source`. Run it (a) from `cmd` via the 32-bit host, (b) from a PowerShell 7 shell via `Start-Process` of the 32-bit host (inherited PS7 `PSModulePath`), (c) through a real `nsExec` using a 20-line NSIS stub that execs it exactly as `installer.nsi:267` does. Save outputs.
- [ ] **Step 2: Try to break resolution.** In (b), repeat with `PSModulePath` set to PS7-first, empty, a nonexistent directory, and a stray `Microsoft.PowerShell.Utility` folder earlier on the path. Record which make `Get-FileHash`, `Get-CimInstance` or `ConvertTo-Json` unresolvable in the 5.1 child.
- [ ] **Step 3: Write the failing test.** `Test-CleanupEnvironment.ps1` takes the winning polluted `PSModulePath` (or, if none reproduces, the PS7-first and empty cases) and runs `Remove-PreviousInstallations.ps1 -AppDirectory <fixture>` in a child 32-bit host with that variable against a fixture install root, asserting exit 0 and cleanup JSON showing the old fixture `removed`. Run it before Task 2: it must FAIL for any environment that reproduces the fault, proving the test bites. If nothing reproduces, record that and keep the test as a guard.
- [ ] **Step 4: Write up.** Append a "Findings" section to the spec: reproduced (exact `PSModulePath`) or "not reproduced after N variants". Do not claim root cause without a repro.
- [ ] **Step 5: Commit** (when asked): `Add cleanup-environment test for #883`.

### Task 2: Deterministic module environment and diagnosed failures

**Recommended executor:** Sonnet 5.5, medium effort.

**Files:**
- Modify: `scripts/alpha/PreviousInstallations.psm1` (add and export `Initialize-SetupHelperEnvironment`)
- Modify: `scripts/alpha/Remove-PreviousInstallations.ps1` (call it first; catch-all that logs and rethrows)
- Modify: `scripts/alpha/Test-CleanupEnvironment.ps1`

**Interfaces:**
- `Initialize-SetupHelperEnvironment [-RequiredCommands string[]]` sets the process `PSModulePath` to `$PSHOME\Modules;$env:WINDIR\System32\WindowsPowerShell\v1.0\Modules`, imports `Microsoft.PowerShell.Utility`, `Microsoft.PowerShell.Management` and `CimCmdlets` by absolute `.psd1` path, resolves the required-command list, and returns `{psVersion; is64Bit; languageMode; psHome; psModulePathInherited; psModulePathEffective}`. On a miss it throws `Setup helper cannot resolve <cmdlet> (module <m>). <diagnostics on one line>`.
- `Remove-PreviousInstallations.ps1` gains `-ReleaseId` (default: leaf of `-AppDirectory`) used to name `installer-logs\<release>-retire.log`.

- [ ] **Step 1: Tests first.** In `Test-CleanupEnvironment.ps1` add: (1) the Task 1 polluted `PSModulePath` passes; (2) a `-RequiredCommands` entry naming a nonexistent cmdlet makes the script exit nonzero, the message contains the cmdlet name, `Is64Bit`, `LanguageMode` and the effective path, and the log file exists with the same block; (3) both hosts. Run: expect failures.
- [ ] **Step 2: Implement.** Add the function; call it inside a `try` at the top of the script; on catch append diagnostics + message to the log (create the folder; a log-write failure is itself written to stderr), then rethrow.
- [ ] **Step 3: Run** `powershell.exe -NoProfile -File scripts/alpha/Test-CleanupEnvironment.ps1` and the SysWOW64 variant. Expected: `passed=true`.
- [ ] **Step 4: Regression.** `Test-PreviousInstallations.ps1` and `Test-PreviousInstallations.ps1 -WithoutHashCommand`.
- [ ] **Step 5: Commit** (when asked): `Pin the setup cleanup helper's module environment and log diagnostics (#883)`.

### Task 3: Shared metadata list and residual classification (pure, no writes)

**Recommended executor:** Opus 5, high effort (safety-critical classifier).

**Files:**
- Modify: `scripts/alpha/PreviousInstallations.psm1`
- Create: `scripts/alpha/Test-ResidualInstallations.ps1`

**Interfaces:**
- `Get-OwnedInstallMetadataNames` -> `string[]`: `Uninstall.exe`, `.camera-registration-before.json`, `.camera-desktop-registration-before.json`, `.corevideo-prerelease-install`, `build-manifest.json`, `previous-installations-cleanup.json`. `Remove-PreviousInstallation` (`:131`) switches to it.
- `Get-ResidualInstallationPlan -CurrentDirectory -InstallRoot -RegistryRoot -RunningPaths` -> objects `{releaseId; directory; status ('ready'|'skipped'|'retained'); reason; metadata[]; payload[]; unknown[]}`. Read-only.

- [ ] **Step 1: Tests first.** Using the `New-Fixture` pattern from `Test-PreviousInstallations.ps1`: (a) folder with only `.camera-desktop-registration-before.json` -> metadata=1, unknown=0; (b) folder with `Recordings\a.mp4` -> unknown, never metadata; (c) full payload with valid manifest and one modified file -> payload lists the matching files, unknown lists the modified one; (d) no manifest -> every non-metadata file unknown; (e) registered folder not returned; (f) current folder not returned; (g) name not matching the release regex not returned; (h) running exe under it -> `skipped`, reason matches `running`; (i) junction inside it -> `retained`, reason `reparse`, junction target untouched; (j) a metadata-named file inside `Recordings\` -> unknown; (k) regression: the shared list contains the desktop camera file and `Remove-PreviousInstallation` removes it from a registered fixture. Run: all fail.
- [ ] **Step 2: Implement** the list and planner. Walk with an explicit stack and `Get-ChildItem -Force`; test `ReparsePoint` on every directory before descending; reuse `Get-OwnedInstallPath` for every path and `Get-InstallPayloadSha256` for hashes.
- [ ] **Step 3: Run** under both hosts. Expected: all pass.
- [ ] **Step 4: Commit** (when asked): `Classify residual install folders without touching them (#883)`.

### Task 4: Residual removal with recoverable backup (BLOCKED on owner rulings Q1, Q2)

**Recommended executor:** Opus 5, high effort (data-moving code; review-heavy).

**Files:**
- Modify: `scripts/alpha/PreviousInstallations.psm1`
- Modify: `scripts/alpha/Test-ResidualInstallations.ps1`

**Interfaces:**
- `Remove-ResidualInstallation -Plan <Task 3 plan> -BackupRoot <path> [-LeaveUnknown] [-FailAfterMoves <int>]` (last switch is a test hook) -> the plan with `status` in `removed|backed-up|retained`, `reason`, `backedUpTo`, `moved`. Layout: `<BackupRoot>\retired-install-remnants-<guid>\<releaseId>\<relative>` plus `backup-manifest.json` (`source`, `destination`, `length`, `sha256` per file).

- [ ] **Step 1: Tests first** (extend the Task 3 file; temp `BackupRoot`): (a) metadata-only folder -> folder gone, no backup folder created; (b) `Recordings` folder -> files byte-identical at destination (SHA-256), manifest lists each, source folder gone, `status=backed-up`; (c) `-LeaveUnknown` -> nothing moved, folder kept, `status=retained`, reason names the count; (d) destination collision -> numeric suffix, no overwrite; (e) `-FailAfterMoves 1` -> every file is either still at source or listed in the manifest; a locked unknown file -> folder `retained`; (f) different-volume `BackupRoot` (stub the volume check if only one volume) -> `retained`, nothing moved; (g) running version never reaches removal; (h) junction never entered. Run: fail.
- [ ] **Step 2: Implement.** Order: guards -> create backup root -> write manifest (planned list) -> move files one at a time (`Move-Item -LiteralPath`, no overwrite) -> rewrite manifest with outcomes -> delete metadata and proven payload files -> remove empty directories deepest first, non-recursive -> remove the version folder. Every step in `try`; failure sets `retained` + reason and stops that folder only.
- [ ] **Step 3: Run** both hosts; expect pass. Re-run Task 2-3 tests.
- [ ] **Step 4: Commit** (when asked): `Back up operator content from stale install folders instead of stranding it (#883)`.

### Task 5: Wire the sweep into the helper, the report and the installer message

**Recommended executor:** Sonnet 5.5, medium effort.

**Files:**
- Modify: `scripts/alpha/Remove-PreviousInstallations.ps1`
- Modify: `scripts/alpha/installer.nsi:267-273` (message text only)
- Modify: `scripts/alpha/Test-PreviousInstallations.ps1` (end-to-end case)
- Modify: any reader of `previous-installations-cleanup.json` (grep first; `Test-AlphaInstaller.ps1` is a candidate)

**Interfaces:**
- Report becomes `{summary:{registeredRetired,residualRemoved,residualBackedUp,retained}, registered:[...], residual:[...]}`.
- Last stdout line: `Previous installation cleanup: <n> retired, <n> backed up, <n> retained.` (captured into NSIS `$2`).
- Exit 1 when any folder is `retained` for an error reason.

- [ ] **Step 1: Tests first.** End-to-end: a root with one registered old install, three metadata-only residual folders, one `Recordings` folder and the current install -> afterwards only the current folder plus the backup folder remain; summary counts exact; JSON parses; a locked file gives exit 1 and a last line saying `1 retained`. Run: fail.
- [ ] **Step 2: Implement** the registered pass, then the residual pass, then report and `installer-logs` copy; update the NSIS warning text (folder counts, backup hint). Keep `/SD IDOK`.
- [ ] **Step 3: Compile the installer** with `scripts/package-alpha-installer.ps1` against the current beta zip (short staging root from #1026) to prove the `.nsi` builds. Do not run `Test-AlphaInstaller.ps1` yet.
- [ ] **Step 4: Run** all three PowerShell tests under both hosts. Expect pass.
- [ ] **Step 5: Commit** (when asked): `Retire residual version folders and report them honestly (#883)`.

### Task 6: CI and documentation

**Recommended executor:** Sonnet 5.5, low effort.

**Files:**
- Modify: `.github/workflows/ci.yml` (the `native-shell-windows` steps next to `Test-PreviousInstallations.ps1` and the `Test-CrashDumps.ps1` step from #1033)
- Modify: `docs/reference/installer.md` (retirement section, lines 10-27)
- Modify: `docs/BACKLOG.md` (the #883 row, line 361)
- Modify: `CLAUDE.md` only if it lists installer helpers (grep first)

- [ ] **Step 1:** Add CI steps for `Test-CleanupEnvironment.ps1` and `Test-ResidualInstallations.ps1`, each `powershell.exe -NoProfile -File ...`.
- [ ] **Step 2:** Document the pinned environment and log location, residual sweep rules, backup layout and manifest, the shared metadata list rule, the new JSON schema and the Task 1 finding. Replace "implementation shipped" in the BACKLOG row with the real state and the pending installed acceptance.
- [ ] **Step 3:** Confirm no doc still says setup uses `Get-FileHash` (`Grep Get-FileHash docs`).

### Task 7: Installed acceptance on the dev box (owner present; this closes #883)

**Recommended executor:** Sonnet 5.5, medium effort, owner approves the install. No code.

**Files:** evidence under `artifacts/installer-validation/<id>/`. No source changes.

- [ ] **Step 1: Before.** Save an inventory of `%LOCALAPPDATA%\Programs\CoreVideoPro` (names, file counts), the uninstall-key list, `Get-CameraMachineRegistration`, both shortcut targets and a settings-file hash; run `Remove-PreviousInstallations.ps1 -AppDirectory <current> -InspectOnly`. Expected on 2026-10-23: 1 full folder, 29 metadata-only, 2 with `Recordings\` (10 files), 1 uninstall entry.
- [ ] **Step 2: Install** the next beta containing this work with the STANDARD interactive installer (not `/S`, not `Test-AlphaInstaller.ps1`) launched from Explorer. Repeat one later upgrade launched from a PowerShell 7 shell to cover the inherited-environment case.
- [ ] **Step 3: Verify all of:** exactly one version folder remains and it is the new one; exactly one uninstall entry; both stable shortcuts and HKLM/HKCU camera ownership point at the new install; settings hash unchanged; `summary.retained = 0`; `installer-logs\<release>-retire.log` shows the diagnostics block (`Is64Bit` false, effective `PSModulePath`); if Q1 is yes, one `retired-install-remnants-<guid>` folder whose manifest lists the 10 recording files with matching SHA-256s. Uninstall-entry count alone is NOT accepted; the folder inventory is.
- [ ] **Step 4: Post** the evidence on #883 and close it only if every item holds. If Task 1 never reproduced the original fault, say so in the closing comment.

## Effort and executors

**Recommended executor:** model + effort per task.

| Task | Executor | Effort | AI-assisted hours |
|---|---|---|---|
| 1 Reproduce / bound the failure | Opus 5 | high | 1.5 |
| 2 Deterministic environment + diagnostics | Sonnet 5.5 | medium | 1.0 |
| 3 Metadata list + residual classifier | Opus 5 | high | 1.5 |
| 4 Residual removal with backup (blocked on Q1/Q2) | Opus 5 | high | 2.0 |
| 5 Wire helper, report, NSIS message | Sonnet 5.5 | medium | 1.0 |
| 6 CI + docs | Sonnet 5.5 | low | 0.5 |
| 7 Installed acceptance | Sonnet 5.5 (owner approves) | medium | 1.0 |
| **Total** | | | **8.5** |

Critical path: 3 -> 4 -> 5 -> 7. Tasks 1 and 2 can run in parallel with Task 3. A fresh-context
review (Opus 5, high, about 0.75 h, not counted above) is recommended on Task 4 before merge.

## Self-review

- Spec section A -> Tasks 1-2; B -> Tasks 3-4; C and D -> Task 5; testing and acceptance -> Tasks 3-7.
- No task requires elevation or changes the 32-bit installer.
- The only file-moving code is Task 4, gated on owner rulings Q1/Q2.
- The Get-FileHash fix already shipped (`91db55bf`); this plan does not redo it. It proves the environment and removes the second symptom (stranded folders).
