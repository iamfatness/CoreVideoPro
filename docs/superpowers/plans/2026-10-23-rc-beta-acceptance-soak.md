# RC Beta Acceptance Soak Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce the first per-frame identity verdict for a shipped beta, accept #1030 live, and close every "installed acceptance pending" row left by the 2026-10-23 PRs.

**Architecture:** One beta cut from `main` after #1038 merges; one 90-second local shakeout of the drill; one 30-minute stamps-on drill in the test meeting with scripted roster events; one clean-picture show run with the existing 1 Hz sampler; a write-up that either passes the gate or files issues.

**Tech Stack:** `scripts/qa/outputs-drill.ps1` + `outputs-drill-judge.py` + `iso-pixel-verdict.py`; the scratchpad `Cut-Beta-*.ps1` pipeline (`package-alpha.ps1`, `package-alpha-installer.ps1`, `Test-AlphaPackage.ps1`, `Test-AlphaInstaller.ps1`); the soak harness in `D:\CoreVideo-QA\soak-20261023-01\` (`Launch-Installed.ps1`, `sampler.py`, `Start-LocalSrtSink.ps1`); cdb for any dump.

**Spec:** `docs/superpowers/specs/2026-10-23-rc-beta-acceptance-soak-design.md`

**Recommended executor:** model + effort per task

| Task | Model | Effort | Why |
|---|---|---|---|
| 1 Cut the beta | sonnet | medium | Scripted pipeline that has run twice; judgement needed only on a red test |
| 2 Drill shakeout (local sources, 90 s) | sonnet | medium | Harness debugging, no live stakes |
| 3 Stamps-on drill with roster script | opus | high | Live run with the owner and helpers on the clock; every anomaly must be read correctly in real time |
| 4 Installed acceptance observations | sonnet | medium | Checklist with exact expected values |
| 5 Clean-picture show run | sonnet | low | Reuses the harness; evidence only |
| 6 Write-up, issues, BACKLOG | opus | medium | Decides what is a gate failure versus a source fact |

## Global Constraints

- Never run the drill or the native test suite while the owner's installed app is running (the vcam SHM unlink incident; `COREVIDEO_VCAM_SHM_DIR`).
- Evidence and recordings on `D:` only; `-Out` must be under `D:\CoreVideo-QA\drills\`.
- RTMP stays off; the stream goes to the loopback SRT sink the drill starts.
- Never SendKeys or steal focus; the control API on `127.0.0.1:8011` drives everything.
- Identity stamps (`COREVIDEO_QA_ISO_COUNTER`, `COREVIDEO_QA_PROGRAM_COUNTER`) are set only by the drill, never for the show run.
- The Zoom meeting link and passcode never appear in an issue, a commit or a doc; join with the full `pwd` link.
- Preferences: the drill backs up and restores `production-output-preferences.json` and its `.bak`; verify the restore by hash before declaring the run closed.
- Merge and publish only on the owner's word; the owner has said: cut the beta when #1038 is merged.

## Review Focus

- A helper leaves during the measured window and the shell re-forms the ISO set: recording must still be `producing` 30 s later, and a Stop/Start after the window must succeed (Task 3 asserts both).
- A camera-off span under 1.5 s produces no holds; over 1.5 s produces holds at about one per second (Task 3 reads `heldFrames` from the finalized snapshot and checks the judge row).
- The drill's preference patch is refused because a value type differs on this beta (the `StreamSrtPort` string rule): Task 2 proves the patch applies on the installed beta before Task 3.
- The camera receiver from `build-dev` is from a different commit than the beta: Task 2 records both hashes and the receiver's version output.
- A WER dump from a deliberate crash lands under the installing user's `%LOCALAPPDATA%` and nowhere else: Task 4 checks the 64-bit registry view only.

---

### Task 1: Cut the beta from main

**Files:**
- Create: `<scratchpad>\Cut-Beta-1024.ps1` (copy of `Cut-Beta-1023.ps1`; tag and output folder changed)
- Create: `D:\CoreVideo-QA\beta-20261021-build\artifacts\qualification-1024-01\` (pipeline output)
- Create: `<scratchpad>\release-notes-<sha7>.md`

**Interfaces:**
- Consumes: `origin/main` head after the #1038 merge; `scripts/alpha/package-alpha.ps1`, `package-alpha-installer.ps1`, `Test-AlphaPackage.ps1`, `Test-AlphaInstaller.ps1` (installer scripts changed in #1033, so `Test-AlphaInstaller` must run this time, not be skipped).
- Produces: tag `beta-2026-10-24-<sha7>`, a GitHub pre-release with 6 assets, the installer sha256, `nativeSha256`, release notes that say "outputs soak pending; see the drill folder".

- [ ] **Step 1: Confirm #1038 is merged and main is green**
Run: `gh pr view 1038 --json state,mergeCommit` and `gh run list --branch main --limit 2`
Expected: `MERGED`; CI `success` on the merge commit.
- [ ] **Step 2: Detach the main clone at the merge commit**
Run: `git -C D:\CoreVideo-QA\beta-20261021-build fetch origin; git checkout --detach origin/main; git log --oneline -1`
Expected: the #1038 merge commit.
- [ ] **Step 3: Run the pipeline with the full native suite and capture tests**
Run: `powershell -NoProfile -File <scratchpad>\Cut-Beta-1024.ps1`
Expected: native suite `N passed, 0 failed`; `dotnet publish` Release win-x64; PRI size check; `--verify-runtime` ok; `test-studio-layout` ok; package and installer built; `Test-AlphaPackage` and `Test-AlphaInstaller` pass.
- [ ] **Step 4: Tag and publish the pre-release**
Run: `git tag beta-2026-10-24-<sha7>; git push origin beta-2026-10-24-<sha7>; gh release create beta-2026-10-24-<sha7> --prerelease --notes-file <scratchpad>\release-notes-<sha7>.md <6 assets>`
Expected: release URL; notes say the soak is pending and list the PRs since 1072a50.
- [ ] **Step 5: Install silently and verify**
Run: `<Setup.exe> /S`, then `Install-MediaRuntime.ps1`, then the per-file hash check against the zip manifest.
Expected: 0 hash mismatches; previous install retired; Start-menu shortcut repointed; ffmpeg present.

### Task 2: Drill shakeout on local sources (90 seconds)

**Files:**
- Create: `D:\CoreVideo-QA\drills\<ts>-shakeout\` (drill `-Out`)
- Modify (only if the drill breaks): `scripts/qa/outputs-drill.ps1`, `scripts/qa/outputs-drill-lib.ps1`, `scripts/qa/outputs-drill.test.ps1`

**Interfaces:**
- Consumes: the installed beta from Task 1; `native\build-dev\corevideo-shared-camera-receiver.exe` (build target `corevideo-shared-camera-receiver`).
- Produces: `verdict.json` for a 90 s local-sources run; `preferences-applied.json` showing the patch held on this beta; receiver hash and version recorded in `shakeout.json`.

- [ ] **Step 1: Confirm nothing is running**
Run: `Get-Process CoreVideoPro.WinUI,corevideo-native,corevideo-zoom-engine -ErrorAction SilentlyContinue; Test-NetConnection 127.0.0.1 -Port 8011`
Expected: no processes; port closed.
- [ ] **Step 2: Run the drill for 90 s**
Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\outputs-drill.ps1 -Seconds 90 -Out D:\CoreVideo-QA\drills\<ts>-shakeout`
Expected: completes; `verdict.json` present; `preferences-applied.json` shows the patch with a string `StreamSrtPort`.
- [ ] **Step 3: Verify the restore**
Run: `Get-FileHash` on `production-output-preferences.json` and its `.bak` against the copies under `outputs-drill-backups`.
Expected: identical hashes.
- [ ] **Step 4: Fix any harness defect, test, commit**
Run: `powershell -File scripts\qa\outputs-drill.test.ps1`
Expected: all pass; commit on `codex/drill-fix-<n>`; PR; the owner merges before Task 3 if the fix is needed for the long run.

### Task 3: Stamps-on 30-minute drill with the roster script

**Files:**
- Create: `D:\CoreVideo-QA\drills\<ts>-gate\` (drill `-Out`); `roster-script.md` in it (the timed instructions the helpers follow); `roster-events.ndjson` (1 Hz `participants` and admission from `/snapshot`, written by `sampler.py` pointed at the drill folder).

**Interfaces:**
- Consumes: the working drill from Task 2; the test meeting link (owner provides; never written into the repo); helpers' Zoom participant ids read from `/snapshot` after they join.
- Produces: `verdict.json` PASS or FAIL; `finalized.json` with `heldFrames` per ISO; the per-row judge summary; recording-state samples proving `producing` through the departure.

- [ ] **Step 1: Brief the helpers**
Script: T+12:00 helper A leaves; T+15:00 helper A rejoins; T+20:00 helper B camera off; T+22:00 helper B camera on; everyone stays to T+30:00. The owner keeps camera and mic on throughout.
- [ ] **Step 2: Start the drill with remote ids**
Run: `outputs-drill.ps1 -Seconds 1800 -RemoteIds <ids> -MeetingUrl '<link>' -Out D:\CoreVideo-QA\drills\<ts>-gate`
Expected: readiness passes; all outputs armed; recording `producing`.
- [ ] **Step 3: Watch the departure**
Run: 1 Hz samples; at T+12:30 check `recState == producing`, `encoderEvidence.completedCalls.start` advanced by 1 (the re-formed session) and no `failed`.
Expected: no wedge; any warning names the departed source, never "encoder load".
- [ ] **Step 4: Judge**
Run: the drill's own judge step, then `python -I scripts\qa\outputs-drill-judge.py <Out>`.
Expected: PASS; helper B's ISO row reads `held=N, source stalled (...)` with N within 2 of the camera-off seconds minus 1; every other row `held=0`; Program and the virtual camera zero missing.
- [ ] **Step 5: Post-window Stop/Start**
Run: `POST /invoke transport.record.set [false]`, then `[true]` after finalize.
Expected: a new take is `producing` within 15 s; the file finalizes on the final stop.

### Task 4: Installed acceptance observations

**Files:**
- Create: `D:\CoreVideo-QA\drills\<ts>-gate\installed-acceptance.md`

**Interfaces:**
- Consumes: the installed beta; 64-bit PowerShell; `launch.log`.
- Produces: one line per row of the spec table with the observed value and PASS/FAIL.

- [ ] **Step 1: WER keys and folder (#433)**
Run: 64-bit `Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\<exe>'` for the three exes; `Test-Path` on the app default recording folder.
Expected: `DumpFolder` `%LOCALAPPDATA%\CoreVideoPro\crashes` as REG_EXPAND_SZ, `DumpType` 2, `DumpCount` 5, the installer marker value present; the folder exists.
- [ ] **Step 2: Deliberate core crash lands a dump (#433)**
Run: with the app idle and no outputs armed, trigger the core's existing fault path (the supervisor crash-lock test command in `docs/reference/core-restart-drill.md`).
Expected: a `.dmp` under `%LOCALAPPDATA%\CoreVideoPro\crashes`; the supervisor respawns the core.
- [ ] **Step 3: Offline recording (#1009)**
Run: Zoom disconnected; `transport.record.set [true]` for 60 s, then `[false]`.
Expected: the file finalizes; `ffprobe` shows about 60 s; it plays.
- [ ] **Step 4: GPU tier line (#439)**
Run: open Settings and read the tier status line; `Select-String gpu-tier-defaults launch.log`.
Expected: "16 GB+ ... tiles up to 10 (product limit)"; `switch=off canvasAbove=no tilesAbove=no`; saved canvas and tiles unchanged against `preferences-before`.
- [ ] **Step 5: Telemetry endpoint line (#434)**
Run: `Select-String 'crash-report: telemetry endpoint' launch.log`
Expected: `host=telemetry.corevideo.io apiKeyPresent=false`.

### Task 5: Clean-picture show run (the owner's current-state test)

**Files:**
- Create: `D:\CoreVideo-QA\soak-<date>-02\` from the `soak-20261023-01` harness (`Launch-Installed.ps1` with the new install root, `sampler.py`, `Start-LocalSrtSink.ps1`).

**Interfaces:**
- Consumes: the installed beta; the owner's real meeting.
- Produces: `samples.ndjson` and `summary.txt` in the 2026-10-23 format; no identity verdict.

- [ ] **Step 1: Launch clean** (`qaCounters=$false`, delivery trace on, monitor diagnostics on); arm the stream to the loopback sink and the virtual camera; the owner presses Record.
- [ ] **Step 2: Sample at 1 Hz**; key on `recState in (recording, producing)`.
- [ ] **Step 3: Teardown**: stream off, sink stopped, prefs restored and hashed, logs copied, `summary.txt` written.

### Task 6: Write-up, issues, BACKLOG

**Files:**
- Create: `docs/qa/rc-beta-acceptance-2026-10-24.md`
- Modify: `docs/BACKLOG.md` (close rows whose acceptance passed; add a row per new FAIL); `README.md` reference-run sentence in Capabilities & status.

- [ ] **Step 1: Write the report**: beta tag, drill verdict per output, roster events with times, held-frame rows, the installed-acceptance table, show-run evidence, open items.
- [ ] **Step 2: File one issue per FAIL** with evidence paths (no meeting link) and add a BACKLOG tier row for each.
- [ ] **Step 3: PR the docs**; the owner merges.
