# Shell Forced-Stop Crash Triage (#457) Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (this is evidence work, mostly sequential). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close #457 on evidence. Either confirm it as the post-Exit dispatcher-drain crash fixed by #462, with a standing gate that would catch a regression, or leave it open with a named capture plan.

**Architecture:** No product code is needed to triage. Dump 17304 and its build are gone. Classification rests on three surrogate sources that survived: event 1000/1001, the shell's `launch.pre-live-confirm.log` and the post-fix close record. If the dump is found, cdb and dotnet-dump re-read it. Task 5 adds one regression gate: a graceful close at the end of a long in-meeting session, judged from the log and the Event Log. A one-line fallback hardening is optional and needs an owner ruling.

**Tech Stack:** PowerShell 5.1, cdb (Windows Kits 10 x64), dotnet-dump 10.0.745401, `gh`, the WinUI shell (`native-shell/CoreVideoPro.WinUI`), xUnit (`CoreVideoPro.WinUI.Tests`).

**Spec:** `docs/superpowers/specs/2026-10-23-shell-forced-stop-crash-triage-design.md`

**Recommended executor:**

| Task | Model | Effort | Reason |
|---|---|---|---|
| 1 Locate and validate dump + symbols | haiku | low | Mechanical file and registry checks with fixed expected outputs. |
| 2 Extract evidence (surrogate, or the dump if found) | opus | high | Reading a stowed-exception fail-fast or a managed/native thread set is judgement work. A misread thread or stowed HRESULT changes the classification. If a dump is found: **fable, high**, because the read has to tell a UI-thread drain from a finalizer release from a supervisor-gate hang with no app frames on the stack, and the #513 history shows how easily that goes wrong. |
| 3 Classify against the three hypotheses | opus | high | It must weigh second-hand stack evidence against first-hand log timing and say plainly where the evidence stops. |
| 4 Write the finding into the issue and docs | sonnet | medium | Prose from a settled table. The facts are fixed by Task 3. |
| 5a Graceful-close gate script + test | sonnet | high | Small script, but it reuses the outputs-drill close guard and has to respect the no-input-injection rule. |
| 5b (optional) ForceExit hardening | sonnet | medium | A one-call change behind an existing seam test pattern. |
| 5c Run the gate live (≥30 min in-meeting) | opus | medium | A live run on the owner's box, judging an all-or-nothing gate. |

**Estimated AI-assisted effort:** 0.25 + 0.75 + 0.5 + 0.5 + 1.5 + 0.5 (optional) + 1.0 (mostly wall-clock soak) = about **4.5 h**, or 4.0 h without 5b. If the dump is found in Task 1, add about 2 h to Task 2.

## Global Constraints

- **Read-only on all evidence.** Never move, open-in-place or delete an original `.dmp`, log or WER folder. Copy first to `D:\CoreVideo-QA\dumps\457\`.
- Check free space before copying a full dump (the C: drive is often nearly full): `Get-PSDrive C,D | Select Name,Free`. Copy to D: only.
- **Never send keys or steal focus** (owner rule). Close the app with `WM_CLOSE` (`Process.CloseMainWindow()`), the control API, or UIA Invoke only.
- Do not close the owner's app while it is recording or streaming. Gate on `Test-DrillSafeToClose` (`scripts/qa/outputs-drill-lib.ps1`).
- Do not rebuild the shell before analyzing a newly found dump (the PDB rule).
- Closing #457 is the owner's decision. Task 4 drafts and posts the evidence comment. It closes the issue only on the owner's word.
- A stability claim is bounded by the window watched. Say "N closes, 0 recurrences". Never say "fixed forever".

---

### Task 1: Locate and validate the dump and symbols

**Files:** none (read-only).

- [x] **Step 1: Search for the dump.**

```powershell
Get-ChildItem "$env:LOCALAPPDATA\CrashDumps","$env:LOCALAPPDATA\CoreVideoPro\crashes" -Filter 'CoreVideoPro.WinUI.exe.*.dmp' -ErrorAction SilentlyContinue | Select Name,Length,LastWriteTime
Get-ChildItem D:\ -Recurse -Filter '*17304*.dmp' -ErrorAction SilentlyContinue | Select FullName
```

Expected (2026-10-23): only `CoreVideoPro.WinUI.exe.28528.dmp` (2026-10-16, about 384 MB). No 17304 anywhere.

- [x] **Step 2: Check for the WER archive copy.**

```powershell
Get-ChildItem C:\ProgramData\Microsoft\Windows\WER\ReportArchive -Directory | ? Name -like '*9ecd7d6e*'
```

Expected: nothing (the 1001 event's folder `AppCrash_CoreVideoPro.Win_698abece..._9ecd7d6e-...` is gone).

- [x] **Step 3: Check for the build folder and PDBs.**

```powershell
Test-Path "C:\Users\walla\OneDrive\Documents\ChatGPT\CoreVideo Pro\beta-release\native-shell\CoreVideoPro.WinUI\bin\Release\net9.0-windows10.0.19041.0\win-x64\publish\CoreVideoPro.WinUI.pdb"
git -C D:\CoreVideo-QA\wt-plans branch -r --contains 46b07ff5
```

Expected: `False`, and only `origin/backup/419-pre-rebase-1d164b7`. Ask the owner ONCE whether the dump or that folder exists elsewhere (another machine, a OneDrive version). If not, record "dump and symbols unavailable" and go to Task 2 surrogate mode. Do not spend more time searching.

- [x] **Step 4: Confirm the tooling.** `Test-Path "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe"` → `True`. `dotnet tool list -g` lists `dotnet-dump`.

### Task 2: Extract the evidence

**Files:** scratch only, `D:\CoreVideo-QA\dumps\457\evidence.txt`.

**Surrogate mode (dump unavailable — the expected path):**

- [x] **Step 1: Event Log signature.**

```powershell
Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000,1001; StartTime=[datetime]'2026-09-10 08:50'; EndTime=[datetime]'2026-09-10 08:56'} | Format-List TimeCreated,Id,Message
```

Expected: 1000 at 08:52:36 with `CoreMessagingXP.dll` 10.0.27200.1048, `0xc000027b`, offset `0x97de6`, PID `0x4398`. 1001 at 08:52:40 with P4 `combase.dll`, P7 `80004004`, report `7f60387a-...`.

- [x] **Step 2: Last shell lines of PID 17304.**

```powershell
Select-String -Path "$env:LOCALAPPDATA\CoreVideoPro\launch.pre-live-confirm.log" -Pattern 'pid=17304 session=4118cc2c' | Select -Last 6 | % Line
```

Expected: the build stamp `46b07ff5...`. The last five lines run from `shutdown: close requested` (08:52:35.764) to `shutdown: resources released` (08:52:36.125), and nothing follows.

- [x] **Step 3: Recurrence census, shell crashes since the fix.**

```powershell
Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000; StartTime=[datetime]'2026-09-10'} | ? Message -like '*CoreVideoPro.WinUI.exe*' | % { ($_.Message -split "`n" | Select-String 'Faulting module name|Fault offset|Exception code').Line -join ' | ' }
```

Expected: exactly one `CoreMessagingXP.dll ... 0x97de6` entry (17304). Every other entry is `Microsoft.UI.Xaml.dll 0x3a9c5d`, `coreclr 0xc0000005` or `CoreMessagingXP 0xc0000602`. Before treating a new entry as unrelated, match its time against a close in the log.

- [x] **Step 4: Post-fix close census.** Count `shutdown: cleanup complete; terminating the process directly` across the retained logs, and check that the fallback never fired:

```powershell
$logs = Get-ChildItem "$env:LOCALAPPDATA\CoreVideoPro\launch*.log"
(Select-String $logs -Pattern 'terminating the process directly').Count
(Select-String $logs -Pattern 'force-exit fallback|cleanup did not complete|TerminateProcess returned').Count
```

Expected: at least 41, and 0. Then list the in-meeting closes after 30 min or more. For each session id with a terminate line: first-line time, `zoom-join: Connected` present, terminate time. Expected: at least 7 (60, 612, 123, 63, 41, 225, 67 min). Write the table into `evidence.txt`.

**Dump mode (only if Task 1 found the dump):**

- [ ] ~~**Step 5: Native.**~~ (not run: no dump) Copy the dump and publish folder to `D:\CoreVideo-QA\dumps\457\`, then:

```powershell
$cdb = "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe"
& $cdb -z D:\CoreVideo-QA\dumps\457\CoreVideoPro.WinUI.exe.17304.dmp -y "srv*D:\symcache*https://msdl.microsoft.com/download/symbols;D:\CoreVideo-QA\dumps\457\publish" -logo D:\CoreVideo-QA\dumps\457\native.txt -c "!analyze -v; .exr -1; .ecxr; kPn 60; ~; ~*kn 30; lmvm CoreMessagingXP; q"
```

Expected for the drain: `ExceptionCode: c000027b`, a `STOWED_EXCEPTION` with HRESULT `80004004`, and on the faulting thread `CoreMessagingXP!...DeferInvokeCallback` under `DispatcherQueueController::ShutdownQueue` under `FrameworkApplication::StartDesktop`.

- [ ] ~~**Step 6: Managed.**~~ (not run: no dump)

```powershell
dotnet-dump analyze D:\CoreVideo-QA\dumps\457\CoreVideoPro.WinUI.exe.17304.dmp -c "clrthreads" -c "clrstack -all" -c "pe -nested" -c "syncblk" -c "exit" > D:\CoreVideo-QA\dumps\457\managed.txt
```

Expected for the drain: the faulting OS thread is the STA UI thread with no managed frames above `StartDesktop`. `pe` is empty. No `Finalize` or `RunFinalizers` on the faulting thread. `syncblk` holds nothing. No thread is in `WaitForExit` or the supervisor gate.

### Task 3: Classify against the three hypotheses

**Files:** scratch `evidence.txt`.

- [x] **Step 1:** Fill the spec's hypothesis table from Task 2. Each verdict cites a row of evidence.
  - H1 (#513 finalizer) is excluded when the module/offset is not `Microsoft.UI.Xaml+0x3a9c5d`, the stowed code is not `0x8000000E`, and the fault came within seconds of a lifecycle event.
  - H2 (engine-off deadlock) is excluded when every `ShutdownAsync` step logged and the fault came after `resources released`.
  - H3 (new member: post-Exit drain) is confirmed when the fault came after `resources released` with an E_ABORT/E_UNEXPECTED stowed code in combase/CoreMessagingXP.
- [x] **Step 2:** Record the confidence statement. In surrogate mode the stack is second-hand (the 2026-09-10 report) and consistent with three first-hand sources. Say exactly that.
- [x] **Step 3:** Decide the outcome. Choose (a) close as fixed by #462 when the census is ≥5 long in-meeting closes with 0 recurrences and 0 fallbacks. Otherwise choose (b) keep open plus the capture plan.

### Task 4: Write the finding into the issue and the docs

**Files:**
- Modify: `docs/reference/winui-crash-class-0xc000027b.md` (the T1.7 bullet only)
- Modify: `docs/BACKLOG.md` (the #457 row only)

- [x] **Step 1: Post the evidence comment** (no close yet). Use `gh issue comment 457 -R iamfatness/CoreVideoPro --body-file <tmp>` with:
  - the corrections (build `46b07ff`, not `c36d80a`; a graceful close, not a forced-stop artefact; dump and PDBs not retained);
  - the three-hypothesis verdict;
  - the post-fix census table;
  - a link to the spec.

  Expected: `gh` prints the comment URL.
- [x] **Step 2: Add one sentence to the T1.7 bullet** of the crash-class doc: "Live confirmation (2026-10-23 census): N in-meeting closes after 41–612 min on the fixed path, 0 recurrences of `CoreMessagingXP+0x97de6`, 0 fallbacks. The original dump was not retained." Update the BACKLOG row to match the outcome.
- [x] **Step 3: Ask the owner** whether to close #457 now (outcome a) or after Task 5c. Close only on their word: `gh issue close 457 -R iamfatness/CoreVideoPro --reason completed`.

### Task 5: Fix proposal and regression gate (or the capture plan)

**5a. The graceful-close gate (recommended).**

**Files:**
- Create: `scripts/qa/graceful-close-check.ps1`
- Create: `scripts/qa/graceful-close-check.test.ps1`

**Interfaces:** consumes `Test-DrillSafeToClose` from `scripts/qa/outputs-drill-lib.ps1` and the control API `/state` and `/snapshot`. Produces a verdict JSON: `{ pass, terminateLine, exitMs, newDumps[], newEvent1000[] }`.

- [x] **Step 1: Write the failing test.** Feed the judge a fixture log with `shutdown: resources released` but no terminate line, plus one fake 1000 event. Expected: `pass=false` with both reasons. A fixture with the terminate line, exit within 6 s and no new dump gives `pass=true`. Run `powershell -File scripts/qa/graceful-close-check.test.ps1`. It should FAIL before the judge exists.
- [x] **Step 2: Implement.**
  1. Record the shell PID, the existing `CrashDumps` and `crashes` file names, and the time.
  2. Refuse the run if `Test-DrillSafeToClose` returns reasons.
  3. Close with `(Get-Process -Id $pid).CloseMainWindow()`.
  4. Wait up to 6 s for exit.
  5. Wait 60 s, then diff the dump folders and query event 1000 for `CoreVideoPro.WinUI.exe` since the recorded time.
  6. Find this PID's last `launch.log` line.

  The pass condition: the last line is `terminating the process directly`, the exit took 6 s or less, and there is no new dump and no new event.
- [x] **Step 3:** Run the test again. Expected: PASS.

**5b. (Optional, owner ruling) ForceExit hardening.**

**Files:** Modify `native-shell/CoreVideoPro.WinUI/Services/ApplicationLifecycle.cs`. Test in `native-shell/CoreVideoPro.WinUI.Tests/`.

- [x] **Step 1:** Make the final exit an injected seam, defaulting to `ShutdownCompletion.TerminateCurrentProcess`. Write a test that asserts the log line is written before the terminate call and that a second call is a no-op. It should fail first.
- [x] **Step 2:** Run `dotnet test native-shell/CoreVideoPro.WinUI.Tests --filter "FullyQualifiedName~ApplicationLifecycle|FullyQualifiedName~ShutdownCompletion"`. Expected: all pass.

**5c. Run the gate live.**

- [ ] **Step 1:** In the authorized test meeting (full pwd link), with Engine on, no recording and no stream, let the app run for 30 min or more. Then run `powershell -File scripts/qa/graceful-close-check.ps1`. Expected: `pass=true`. Record the build stamp and session length in the issue comment.
- [ ] **Step 2:** If it FAILS, keep #457 open. Apply the spec's capture plan:
  - copy the dump, publish folder and logs to `D:\CoreVideo-QA\dumps\457\`;
  - run Task 2 in dump mode;
  - re-classify in Task 3.

**Capture plan (if the outcome is b, or 5c fails):** Confirm that the HKLM LocalDumps key for `CoreVideoPro.WinUI.exe` has DumpType 2. On this box it is `%LOCALAPPDATA%\CrashDumps`, count 20. On a #1033 install it is `%LOCALAPPDATA%\CoreVideoPro\crashes`, count 5. Copy any new shell dump with its exact publish folder before any rebuild or rotation. Pull events 1000/1001 for the signature even if the dump is lost.

---

## Execution record (2026-10-23, branch `codex/rc-457`)

Evidence copies are in `D:\CoreVideo-QA\triage-457\` (never on C:). Originals were only read.

### Task 1 result: dump and symbols unavailable, surrogate mode

- `%LOCALAPPDATA%\CrashDumps` holds only `CoreVideoPro.WinUI.exe.28528.dmp` (383,866,429 bytes, 2026-10-16 18:55). `%LOCALAPPDATA%\CoreVideoPro\crashes` does not exist. No `*17304*.dmp` anywhere on `D:\`.
- No WER ReportArchive or ReportQueue folder matching `9ecd7d6e` or `7f60387a`.
- The `beta-release` build folder and its `CoreVideoPro.WinUI.pdb` do not exist.
- `46b07ff5` is reachable only from `origin/backup/419-pre-rebase-1d164b7`; it is not an ancestor of `origin/main`.
- cdb present; `dotnet-dump` 10.0.745401 installed.
- The owner question ("does the dump exist elsewhere?") is superseded by owner ruling D12 (2026-10-23 23:50 EDT): close on the retained-log evidence. Dump mode (Task 2 steps 5-6) is not run.
- Evidence: `D:\CoreVideo-QA\triage-457\task1.txt`.

### Task 2 result: surrogate evidence re-verified

- **Event 1000** 2026-09-10 08:52:36: `CoreVideoPro.WinUI.exe` 0.1.0.0, module `CoreMessagingXP.dll` 10.0.27200.1048 (WindowsAppRuntime 2.4.0.0 package path), `0xc000027b`, offset `0x97de6`, PID `0x4398` (17304), start time `0x1DD41181B1DB7B6` = 2026-09-10 07:32:48 -04:00. Matches the spec.
- **Event 1001** 08:52:40: APPCRASH, report `7f60387a-6831-4b72-9da0-92429402129b`, P4 `combase.dll` 10.0.26100.9444, P7 `80004004` (E_ABORT), P8 `0x579e4`. Matches the spec.
- **Shell log**, `launch.pre-live-confirm.log`, session `4118cc2c`: 86 lines (the spec's 87 `pid=17304` matches include one line from a later, unrelated session that reused PID 17304 on 2026-09-11; filter on the session id). Build `1.0.0+46b07ff5e7b2...`. In meeting from 07:33:01 (`meetingState=in_meeting participants=7`). Last five lines 08:52:35.764 `close requested` to 08:52:36.125 `resources released`; nothing after. Matches the spec.
- **Event 1000 census** since 2026-09-10 00:00, shell only: **15** entries (the spec said 16; one fewer is present today, consistent with log retention). Exactly one is `CoreMessagingXP.dll+0x97de6` (17304). The rest: 12 x `Microsoft.UI.Xaml.dll+0x3a9c5d`, 1 x `coreclr.dll c0000005` (10-13, #889), 1 x `CoreMessagingXP.dll c0000602` (09-18, `+0xd87d`). No shutdown-drain signature after the fix.
- **Close census** over 9 retained `launch*.log` files: 41 `terminating the process directly` lines; 0 `cleanup did not complete` / `TerminateProcess returned` lines (the ShutdownCompletion fallback never fired). In-meeting closes after 30 min or more: 09-10 20:10 (60 min), 09-11 08:37 (612), 11:02 (123), 15:26 (63), 19:50 (41), 09-13 11:39 (225), 14:38 (67). Seven, matching the spec.
- **New fact not in the spec:** `ApplicationLifecycle.ForceExit` (which ends in `Environment.Exit`) ran twice on post-fix builds, both via the "close requested while cleanup is in progress — forcing exit" path (a second close arriving during `ShutdownAsync`): 2026-09-10 21:31 (build `2d558eb2`, after a recording finalize) and 2026-10-23 11:40 (build `1072a50a`, after a stream stop). Neither produced an event 1000. The three other `force exit code` lines are July pre-fix soak logs. This is the `Environment.Exit` exposure that Task 5b removes, and it means a second WM_CLOSE during cleanup must never be sent by a gate.
- Evidence: `task2-events.txt`, `task2-pid17304-log.txt` (meeting number line removed), `task2-event1000-census.txt`, `task2-close-census.txt`, `launch.pre-live-confirm.log.copy`, all in `D:\CoreVideo-QA\triage-457\`.

### Task 3 result: H3 confirmed, outcome (a)

| # | Verdict | Evidence row |
|---|---|---|
| H1 #513 finalizer | Excluded | Module `CoreMessagingXP+0x97de6`, not `Microsoft.UI.Xaml+0x3a9c5d` (event 1000). P7 `80004004`, not `8000000E` (event 1001). Fault 0.4 s after `resources released` (log), not time-delayed. |
| H2 engine-off deadlock | Excluded | Every `ShutdownAsync` step logged, core stop 337 ms, fault after the last step (log). |
| H3 post-Exit dispatcher drain | Confirmed | Fault after `resources released`, stowed E_ABORT in combase, CoreMessagingXP fail-fast. Fixed by #462. |

Confidence: the stack is second-hand (the 2026-09-10 report) and consistent with three first-hand sources (event 1000, event 1001, log timing). None conflicts.

Outcome (a): 7 long in-meeting closes (threshold 5), 0 recurrences, 0 ShutdownCompletion fallbacks. Owner ruling D12 confirms closing on this evidence. Evidence: `D:\CoreVideo-QA\triage-457\evidence.txt`.

### Task 4 result: posted, docs updated, #457 closed on the owner's word

- Evidence comment posted to #457 with the corrections, the verdict table and the close census.
- `docs/reference/winui-crash-class-0xc000027b.md`: T1.7 bullet gained the live-confirmation sentence.
- `docs/BACKLOG.md`: #457 row marked closed; #444 and #446 stay open on the same row.
- Closed with `gh issue close 457 --reason completed --comment` citing owner ruling D12 (2026-10-23 23:50 EDT).

### Task 5a result: gate script and test

- `scripts/qa/graceful-close-check.ps1`: refuses (exit 2, nothing closed) unless exactly one shell is running, the control API answers, `Test-DrillSafeToClose` has no reasons other than `engine on` (the shell's close guard prompts only for recording/streaming), the shell has been up `-MinSessionMinutes` (default 30) and the snapshot says `in_meeting` (or `-AllowOutsideMeeting`). It then sends ONE `WM_CLOSE` (`CloseMainWindow`), never a second (a second close during cleanup takes `ForceExit`), waits 6 s (and up to 60 s more, sending nothing), watches 60 s from the close, and writes `before.json`, `check.log` and `verdict.json` under `D:\CoreVideo-QA\graceful-close\<timestamp>\`. Exit 0 pass, 1 fail.
- The session is keyed on the log's `session=` id, not the PID (PID 17304 itself was reused a day later).
- `scripts/qa/graceful-close-check.test.ps1`: 17 cases (failed first with the script missing; 17/17 after). The parsers were also checked against the real `launch.log` (the 2026-10-23 09:00 close is read as a terminate line) and the real Event Log (the 2026-10-16 entry is decoded).

### Task 5b result: ForceExit ends in TerminateProcess (owner-approved optional)

- `ApplicationLifecycle.ForceExit(int)` now delegates to an injected overload: log `shutdown: force exit code=N`, `PrepareShutdown`, then `ShutdownCompletion.TerminateCurrentProcess`; `Environment.Exit` runs only if the terminate call returns (logged). A run-once `ExitOnce` latch replaces the static int.
- `ApplicationLifecycleForceExitTests` (4 cases) failed first (CS0426, no `ExitOnce`), then passed. Filtered run `ApplicationLifecycle|ShutdownCompletion`: 10/10. Full `CoreVideoPro.WinUI.Tests`: 1765/1765. `dotnet build` of the shell (Release x64): succeeded.
- Not exercised live: no forced-exit path was triggered on the real app.

### Task 5c: NOT RUN (owner)

Left unchecked on purpose. Owner steps:

1. Build and launch this branch (`npm run app`), join the authorized test meeting with the full pwd link, turn Engine on. No recording, no stream.
2. Leave it running 30 minutes or more. Do not close or restart it in that time.
3. Run `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\graceful-close-check.ps1` from the repo root. It closes the app itself (one WM_CLOSE) and takes about 70 s. Do not touch the window while it runs.
4. Expect `Verdict: PASS` and exit 0. Evidence: `D:\CoreVideo-QA\graceful-close\<timestamp>\verdict.json`. Exit 2 means it refused and closed nothing (the reason is printed). Exit 1 is a FAIL: copy any new dump, the publish folder and the logs to `D:\CoreVideo-QA\dumps\457\` before rebuilding, then reopen #457.
5. Post the build stamp, session minutes and verdict to #457.
