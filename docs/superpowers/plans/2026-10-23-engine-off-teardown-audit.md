# Engine-off Teardown Audit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A written, per-rule, file:line audit of the engine-off teardown path against the five ZoomISO rules, covering Leave, Engine off, app close, core restart and a WinUI crash with EngineHost, with every cell classified `holds` / `violates` / `accepted exception` / `N/A` and one regression test for each violation.

**Architecture:** Read-only tracing, one task per rule, each task producing a rule section of `docs/reference/engine-off-teardown-audit.md` plus the regression tests for that rule's violations. A final task assembles the matrix, the live checklist, the BACKLOG row and the cross-link. This is a document plus tests, not a refactor: no production file changes.

**Tech Stack:** C++20 core and engine (`native/`, the in-house gtest shim `native/tests/gtest/gtest.h`), C# / .NET shell (`native-shell/`, xUnit), PowerShell for builds.

**Spec:** `docs/superpowers/specs/2026-10-23-engine-off-teardown-audit-design.md` (the five rules, the entry points E1-E5, the finding fields, the acceptance list). Read it first; this plan does not restate the rule text.

## Global Constraints

- **No production edits.** Only these may change: `docs/reference/engine-off-teardown-audit.md` (new), `docs/BACKLOG.md` (row T4.3), `docs/reference/engine-teardown-order.md` (one link line, plus a one-line correction only if Task 2 proves the "Remaining gaps" sentence stale), and test files and test project/CMake registrations. A test seam that needs a production change is reported as a finding and the test is skipped, never "just added".
- **A `holds` claim needs code evidence, not a comment.** The phrase "ZoomISO rule" in `engine-video.cpp` and the prose of `engine-teardown-order.md` are pointers to where to look. Cite the lines that establish the property, the constant if a wait is bounded, and an existing test if one pins it.
- **Label what was not run.** Anything that needs the real SDK or the installed app is `read-only` confidence and goes on the live checklist. Do not write "verified", "stable" or "fixed" for it. Nothing in this item is `run-live`.
- **A violation test must be watched failing.** Write the test, run it against current code, record the failure output in the finding. If the finding cannot be reproduced with any harness, say `not reachable`, name what would reach it, and still commit the test disabled/skipped with the issue number in the reason. Never leave main red: an intentionally failing test is committed `Skip`/`DISABLED_` with its issue number.
- **Rule wording is the spec's.** If the owner corrects R4 or R5 wording before or during execution, re-run only the affected rule task (3 and 4 below depend on R4/R5 text; 1, 2, 3 on the settled text).
- **Pin the audited commit** in Task 0 (`git rev-parse HEAD` on the worktree after fetching main) and cite line numbers valid at that commit. Re-grep before quoting a number from this plan; the numbers in the "Starting anchors" lists below are from 2026-10-23 and rot.
- Native build is Release only: `cmake --build native\build-dev --config Release --target corevideo-native-tests`. Debug and Release write the same exe path; a missing `--config Release` silently builds Debug (~8 MB vs ~2.3 MB). Use `npm run build:native-dev` in a fresh worktree (hand-run cmake silently dropped D3D11).
- The gtest shim's `--gtest_filter` takes ONE positive wildcard: no `:` lists, no `-` negation. Run suites separately.
- Never run the native suite or kill processes while the owner's installed app is streaming or has the virtual camera on, and never kill a core you did not start.
- Shell tests: `dotnet test native-shell\CoreVideoPro.MediaCore.Tests` and `native-shell\CoreVideoPro.WinUI.Tests` with `--filter "FullyQualifiedName~<Class>"`. WinUI tests need the repo's existing test-project build; do not change project targets.
- Every filed GitHub issue: `gh issue create -R iamfatness/CoreVideoPro --label backlog`, title prefixed `[teardown-audit]`, body = the finding block. The audit file references issue numbers; no fix is made here.
- Commit per task on the lane branch; merge and publish only on the owner's word.
- The audit doc may quote short code excerpts only when the exact text is the evidence. No emojis. No em-dash overuse; match the voice of `docs/reference/engine-teardown-order.md`.

## Review Focus

For the reviewer (give this to the review agent verbatim):

1. **Every matrix cell is classified**, and no `HOLDS` rests on a comment or a naming convention. Spot-check five `HOLDS` cells by re-deriving the evidence from the cited lines.
2. **Each `VIOLATES` has a test that was watched failing**, with the failure output pasted, and that test fails for the stated reason (not a harness error). Check it passes when the guard is simulated, where the finding claims that.
3. **Thread names are correct** in every Path. The classic error here is calling a thread-pool continuation "UI" or the other way round (`ConfigureAwait(true)` vs `(false)`, `UiDispatch.Run` running inline when already on the UI thread, the SDK status callback thread vs the engine main STA).
4. **Bounded means bounded.** Each deadline cited is the effective one after nesting (client `CancelAfter(70 s)` vs the 2 s linked token; `WaitForExit(grace)` then `Kill` then `WaitForExit(1500)`), and the sum on the app-close path is checked against `ShutdownBudget`.
5. **`ACCEPTED EXCEPTION` is not a dumping ground.** Each states the failure's cost and leaves the owner sign-off blank.
6. **No production file changed** (`git diff --stat` shows only docs and tests/registrations).
7. **Live checklist honesty:** every `read-only` claim that the audit relies on appears there with a concrete rig step.

---

### Task 0: Pin the commit, create the audit skeleton and the finding template

**Files:**
- Create: `docs/reference/engine-off-teardown-audit.md`

**Interfaces:**
- Consumes: the spec; `git rev-parse HEAD`.
- Produces: the skeleton every later task fills: title, audited commit SHA and date, "How to read this" (the verdict vocabulary and confidence levels, copied from the spec), the five rules verbatim as a table, the empty 5 x 5 matrix (rules x E1..E5), one empty `## R<n>` heading per rule with an `### Findings` placeholder, `## Live checklist` and `## Issues filed` placeholders.

- [ ] **Step 1: Confirm the base.** In the worktree, `git fetch origin main` and record `git rev-parse HEAD` and `git log -1 --format=%cd`. If the worktree is behind `origin/main` on any of the files named in the entry-point table, say so in the doc header (do not rebase; the other agents share this worktree).

- [ ] **Step 2: Re-derive the entry-point anchors.** Run these greps and keep the output for the later tasks (they are the starting anchors; do not trust the spec's names without them):

```powershell
rg -n "LeaveZoomAsync|StopZoomCaptureAsync|UnsubscribeZoomCapture|StopEngineRawMediaAsync|StopMediaCoreSession|ForceShutdownMediaCore|DetachHostedEngineOnExit" native-shell --glob '!**/*Tests*'
rg -n "StopForAppExit|StopChild|DisposeChild|OnChildExited|RecoverChildAsync|KillWaitMilliseconds" native-shell\CoreVideoPro.MediaCore
rg -n "leave\(\)|stopCapture\(\)|stopReader|signalSenderStopAndDropQueue|restartBeforeJoin_|beginShutdown" native\src\modules\ZoomEngineRuntime.cpp
rg -n "stop_raw_media|CleanUPSDK|pump_windows_messages|share_engine.detach|EngineAudio::instance\(\).shutdown|MEETING_STATUS_ENDED" native\zoom-engine\engine\main.cpp
rg -n "unsubscribe_all|~ParticipantSubscription|m_stopping|destroyRenderer" native\zoom-engine\engine\engine-video.cpp
rg -n "JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|TerminateProcess|WaitForSingleObject|void ZoomEngineProcessClient::(terminate|stop)" native\src\modules\ZoomEngineProcess.cpp
rg -n "ControlAsync|ExchangeAsync|CancelAfter|DisposeAsync" native-shell\CoreVideoPro.MediaCore\Services\EngineHost*.cs
```

- [ ] **Step 3: Write the skeleton** with the header (audited SHA, date, "read-only except where a finding says test-proved"), the verdict vocabulary, the rule table copied from the spec, and the empty matrix.

- [ ] **Step 4: Settle the rule text with the owner if possible.** The spec flags R4 and R5 wording as ours. Put one line at the top of the audit: "Rules R4 and R5 are worded by the audit author; awaiting owner correction", and carry on. Do not block.

---

### Task 1: R1 — Stop off the UI thread (E1-E5)

**Files:**
- Modify: `docs/reference/engine-off-teardown-audit.md` (section `## R1`)
- Create (as findings dictate): `native-shell/CoreVideoPro.WinUI.Tests/TeardownUiThreadRuleTests.cs`, and/or `native-shell/CoreVideoPro.MediaCore.Tests/HostedStopBoundednessTests.cs`
- Modify only if a new test class needs project registration: the test `.csproj` (SDK-style projects glob `*.cs`, so normally nothing)

**Interfaces:**
- Consumes: Task 0 anchors; `MediaCoreSupervisor` hosted branch, `EngineHostClient`, `StudioViewModel` (`UnsubscribeZoomCapture`, `StopMediaCoreSession`, `ForceShutdownMediaCore`, `DisposeAsync`), `SettingsViewModel.LeaveZoomAsync`, `TransportCoordinator.ToggleEngineAsync`, `MainWindow` shutdown, `UiDispatch`.
- Produces: classified cells R1xE1..E5 and the R1 regression tests.

Starting anchors (re-grep): `SettingsViewModel.cs` `LeaveZoomAsync` (`.ConfigureAwait(true)` continuations); `StudioViewModel.cs` `UnsubscribeZoomCapture` -> `_ = StopEngineRawMediaAsync(...)`; `StudioViewModel.cs` `StopMediaCoreSession` -> `_bridge.Stop()`; `MediaCoreSupervisor.cs` hosted `Stop()` (`.GetAwaiter().GetResult()`, `CancellationToken.None`); `EngineHostClient.ControlAsync` (`_urgent.WaitAsync`, `ExchangeAsync` 70 s deadline); `MainWindow.xaml.cs` shutdown (`Task.Run(... DisposeAsync)`, `WaitAsync(ShutdownTimeout)`); `UiDispatch.Run` (inline when `HasThreadAccess`).

- [ ] **Step 1: Trace each entry point, naming threads.** For each of E1-E5 list every hop from the click/close to the last wait, and for each hop the thread (UI / pool / supervisor reader / sender / engine STA). Mark each wait: `await` (fine), `.GetAwaiter().GetResult()`, `.Wait()`, `.Result`, `WaitForExit`, `Join`, `lock`, `SemaphoreSlim.Wait` (not fine on UI). Pay specific attention to:
  - Who calls `MediaCoreBridgeService.Stop()` / `MediaCoreSupervisor.Stop()` and on which thread, in both non-hosted and hosted mode (Spec lead 1).
  - Whether `StopMediaCoreSession` can re-enter itself through `StopChild` -> `RaiseHealth` -> `UiDispatch.Run` inline (Spec lead 2), and whether `_shutdownPrepared` is the only brake.
  - `StopEngineRawMediaAsync`'s `Task.Delay` loop: confirm it never resumes on the UI thread with a blocking call, and that the fire-and-forget task cannot fault unobserved.
  - `ForceShutdownMediaCore` as called from `DisposeAsync` (a `Task.Run`/`ConfigureAwait(false)` continuation) vs from any UI-affinitied path; `DetachHostedEngineOnExit`'s `StopForAppExit(TimeSpan.Zero)` -> `EngineHostClient.DisposeAsync().GetAwaiter().GetResult()` and which thread runs it.
  - `HostedControlAsync` and every `ForwardHostedCommandAsync` caller (EngineHost process only, never UI; confirm).

- [ ] **Step 2: Classify and write findings** (`F-R1-E<n>-<k>`) into `## R1` using the spec's field list. Expect at least: the hosted `Stop()` finding (VIOLATES or latent, per reachability), the `StopMediaCoreSession` re-entry finding, and `HOLDS` findings for Leave, Engine off, app close with evidence lines.

- [ ] **Step 3: Write the regression tests, one per violation, failing first.** Candidates (adapt to what Step 1 proved):
  - `HostedStop_FromAnyThread_ReturnsWithinTheCallersBudgetWhenTheHostNeverReplies` (MediaCore.Tests): start an `EngineHostServer`-less named pipe that accepts and never replies (the `EngineHostTests` real-host helper shows how to host a pipe), call the hosted `Stop()` on a worker thread, assert it returns within a budget (e.g. 3 s). Expect it to hang to the 70 s deadline today: record that, then commit with `[Fact(Skip = "violates R1/R4, #<issue>")]` and the budget assertion intact.
  - `StopMediaCoreSession_IsNotReentrantWhenStopRaisesStopped` (WinUI.Tests): drive the view model's health handler with a fake `IMediaCoreBridge`/supervisor whose `Stop()` raises `HealthChanged(Stopped)` synchronously; assert `Stop` is called at most once. Use the same fake-host approach as `TransportCoordinatorTests` or the existing view-model test builders; if `StudioViewModel` cannot be built in a test without a dispatcher, test `UiDispatch.Run`'s inline-reentry contract plus the handler's guard in isolation and record "full path not reachable" in the finding.
  - If neither hosted-`Stop` nor re-entry is reachable by UI callers, the test is a **guard test**: `NoUiThreadCallerBlocksOnTheBridgeStop` that scans the WinUI sources for `.GetAwaiter().GetResult()` / `.Wait(` / `.Result` on bridge/supervisor/host-client calls outside an allow-list that names `DetachHostedEngineOnExit` and `ForceShutdownMediaCore` (both run off the UI thread by the shutdown design). Source-scanning guards exist elsewhere in these test projects; follow their shape.

- [ ] **Step 4: Run and record.**

```powershell
dotnet test native-shell\CoreVideoPro.MediaCore.Tests --filter "FullyQualifiedName~HostedStopBoundedness|FullyQualifiedName~EngineHost|FullyQualifiedName~MediaCoreAppExitStop"
dotnet test native-shell\CoreVideoPro.WinUI.Tests --filter "FullyQualifiedName~TeardownUiThreadRule|FullyQualifiedName~ShutdownBudget|FullyQualifiedName~CloseGuardFlow"
```

Paste the failing output for each violation test into its finding before skipping it. The neighbouring classes must stay green.

- [ ] **Step 5: File an issue per violation**, then put the numbers in the finding and in `## Issues filed`.

---

### Task 2: R2 — Stop-then-destroy order (E1-E5)

**Files:**
- Modify: `docs/reference/engine-off-teardown-audit.md` (section `## R2`)
- Create (as findings dictate): `native/tests/EngineExitOrderGuardTest.cpp` (source-order guard for `main()`), `native/tests/EngineVideoTeardownTest.cpp` (mock-renderer harness for `ParticipantSubscription` / `EngineVideo`), and/or additions to `native/tests/ZoomEngineRuntimeTest.cpp`
- Modify: `native/CMakeLists.txt` (register the new test files beside `tests/EngineShareLifecycleTest.cpp`, ~line 922; link `zoom-engine/engine/engine-video.cpp` into that test target only if the harness test needs it; this is a test-target change, not a production one)

**Interfaces:**
- Consumes: `native/zoom-engine/engine/main.cpp` exit sequence, `engine-video.cpp`, `engine-share.cpp`, `engine-audio.cpp`, `ZoomEngineRuntime` destructor and `stopReader`, `ZoomEngineProcessClient::terminate/stop`, `MediaCore` member destruction order, `JsonRpcServer` shutdown, the EngineHost `Program.Run` tail, the `EngineShareLifecycleTest.cpp` mock pattern.
- Produces: classified R2xE1..E5 cells and the R2 regression tests.

Starting anchors: `main.cpp` end of `main()` (`running.store(false)`, heartbeat join, `Leave`, `stop_raw_media("shutdown")`, bounded pump `for i<5`, `share_engine.detach()`, `EngineAudio::shutdown()`, `CleanUPSDK()`, `ipc_teardown`); `main.cpp` `MEETING_STATUS_ENDED` case; `engine-video.cpp` `~ParticipantSubscription` and `unsubscribe_all`; `engine-share.cpp` `detach()`; `engine-audio.cpp` `reset_subscription` / `shutdown`; `ZoomEngineRuntime.cpp` `~ZoomEngineRuntime` (ingest joins first, then `signalSenderStopAndDropQueue`, `stopReader`, sender join); `ZoomEngineProcess.cpp` `terminate()` / `stop()` (1000 ms wait then `TerminateProcess`); `MediaCore.h` member declaration order (is `zoomEngineRuntime_` destroyed before or after the modules that consume its frames?).

- [ ] **Step 1: Trace the engine exit sequence line by line** against R2 (a)-(e). Write the actual order as a numbered list with line numbers and mark each step's bound. Check specifically:
  - Is `Leave` complete (or merely requested) when `stop_raw_media("shutdown")` runs? Does the 5 x 50 ms pump bound make the drain adequate, or merely hopeful (label it)?
  - After `CleanUPSDK()`, which objects with SDK handles remain alive? `EngineVideo` is a stack local: confirm `~EngineVideo` finds an empty `m_subs` after `stop_raw_media`, including subscriptions added **between** `stop_raw_media` and `CleanUPSDK` (the IPC loop has exited so none, but the SDK's own callbacks could re-subscribe: check whether any callback path calls `subscribe`).
  - `EngineAudio::shutdown` runs after `stop_raw_media`'s `reset_subscription`: confirm idempotence.
  - The status-callback path (Spec lead 4): `stop_raw_media` from `MEETING_STATUS_ENDED` followed by a later `Leave`/exit; confirm a second `stop_raw_media` at shutdown is harmless, and that the callback-thread invocation does not run teardown concurrently with the main-thread exit sequence (two threads in `unsubscribe_all`/`set_raw_media_active` at once: `m_mtx` protects the maps; check `m_raw_media_active` / `m_raw_media_requested` plain bools for a data race).
  - The core side: `~ZoomEngineRuntime` joins ingest threads before retiring the process (consumers before producer, good), then `terminate()` closes IPC **before** `stop()` so `quit` is never sent (Spec lead 3). Determine whether the engine's EOF path reaches the same ordered exit as `quit` (read `ipc_read_line_with_message_pump` and the loop `break`), and whether 1000 ms is enough for `Leave` + 250 ms pump + `CleanUPSDK`. State plainly that the timing is `read, not run`.
  - E4 hard kill: the job object ends the engine without R2. Classify (expected `ACCEPTED EXCEPTION`, Spec lead 7), state the consequence, leave the sign-off blank.
  - E5: the host's final `StopForAppExit(10 s)` vs the engine behind it; a host crash gives the core stdin EOF, which flows into E3's core-side path.
  - `engine-teardown-order.md` "Remaining gaps": decide stale or true from the trace. If stale, propose the one-line correction in the finding and apply it in Task 6.

- [ ] **Step 2: Classify and write the findings.**

- [ ] **Step 3: Write the regression tests.**
  - **Guard test for the exit order** (`EngineExitOrderGuardTest.cpp`): read `native/zoom-engine/engine/main.cpp` as text at the path the build already knows (follow how existing tests locate repo files; if none do, use `CMAKE_CURRENT_SOURCE_DIR` via a compile definition added in the same CMake edit). Assert, within the text after `heartbeat.join()`, that the indices of `Leave(`, `stop_raw_media("shutdown")`, `share_engine.detach()`, `EngineAudio::instance().shutdown()` and `ZOOMSDK::CleanUPSDK()` are strictly increasing, and that `stop_raw_media(` occurs before `CleanUPSDK(`. Watched-failing proof: temporarily swap two lines in a scratch copy (never in the worktree file) and run the guard against the copy, or run the assertion against an inline sample string with the order reversed; record which.
  - **Mock-renderer test for `~ParticipantSubscription`** (`EngineVideoTeardownTest.cpp`), if `engine-video.cpp` can be linked with the `EngineShareLifecycleTest` mock SDK layer: a callback thread running `onRawDataFrameReceived` continuously while `unsubscribe_all()` runs; assert (1) no callback after `destroyRenderer`, (2) `unSubscribe` observed after `m_stopping`, (3) no deadlock when `unSubscribe` itself invokes the delegate (the SDK behaviour `EngineShareLifecycleTest` already models via `notify` inside `unSubscribe`). This is an R2 and R3 test; if it needs production changes to link, record `not reachable` and skip with the reason.
  - **Core destructor test** (`ZoomEngineRuntimeTest.cpp`, beside `DestructorDropsQueuedSendsAndJoinsWithoutHanging`): `DestructorRetiresTheProcessOnlyAfterIngestThreadsStopped` using the fake client to record the order of `running()`/`terminate` against an ingest-thread-alive flag. Add only if Step 1 found the order is not already pinned.
  - **EOF-vs-quit** (`ZoomEngineRuntimeTest.cpp`): with the fake client, assert what `~ZoomEngineRuntime` sends before retiring (today: nothing, `quit` is skipped) so a future change to `terminate()` is a conscious one. If the finding is `VIOLATES` (kill inside `CleanUPSDK` is possible), the test pins the intended behaviour (the quit/grace happens before kill) and is committed disabled.

- [ ] **Step 4: Build and run.**

```powershell
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
cmd /c "call `"$vs`" -arch=amd64 >nul 2>&1 && cmake -S native -B native\build-dev >nul && cmake --build native\build-dev --config Release --target corevideo-native-tests 2>&1" | Select-String " error |tests.vcxproj -> "
native\build-dev\corevideo-native-tests.exe --gtest_filter='EngineExitOrderGuard.*'
native\build-dev\corevideo-native-tests.exe --gtest_filter='EngineVideoTeardown.*'
native\build-dev\corevideo-native-tests.exe --gtest_filter='ZoomEngineRuntime.*'
native\build-dev\corevideo-native-tests.exe --gtest_filter='EngineShareLifecycle.*'
```

Confirm the Release exe size is ~2.3 MB, not ~8 MB.

- [ ] **Step 5: File issues for violations**, record numbers.

---

### Task 3: R3 — No locks across SDK calls (E1-E5, engine and core)

**Files:**
- Modify: `docs/reference/engine-off-teardown-audit.md` (section `## R3`)
- Create (as findings dictate): `native/tests/EngineAudioTeardownTest.cpp`, additions to `native/tests/EngineVideoTeardownTest.cpp` (from Task 2) and `native/tests/ZoomEngineRuntimeTest.cpp`
- Modify: `native/CMakeLists.txt` (register new test sources if not already done in Task 2)

**Interfaces:**
- Consumes: every mutex in `engine-video.cpp` (`m_mtx`, `m_targets_mtx`, per-subscription state), `engine-share.cpp` (`m_mtx` + `RendererCallbackGate`), `engine-audio.cpp` (`m_subscribe_mtx`, `m_targets_mtx`), `main.cpp` (any lock held across `Leave`, `StopRawRecording`, `StopRawLiveStream`, `StopRawArchiving`, `SetEvent`), `ZoomEngineRuntime::mutex_` / `sendMutex_` / `coreMutex` (a lock held across process I/O is the core-side analogue), `EngineMutexGuardrail`.
- Produces: classified R3xE1..E5 cells and the R3 regression tests.

Starting anchors: `engine-video.cpp` `~ParticipantSubscription` (flag, drain, then SDK calls) and `unsubscribe_all` (maps moved out under `m_mtx`); `engine-share.cpp` `detach()` / `set_raw_media_active` with `RendererCallbackGate::Transition`; `engine-audio.cpp` `reset_subscription` / `shutdown` call `helper->unSubscribe()` holding `m_subscribe_mtx` (callbacks take only `m_targets_mtx`); `ZoomEngineRuntime.cpp` `leave()` and `stopCapture()` (hold `mutex_` while only *enqueueing* sends; the pipe write is on the sender thread under `sendMutex_`); `stopReader()` (takes `mutex_` briefly, then terminates and joins outside it); `ZoomEngineRuntime.cpp` around the line that launches the engine (`client->start(...)` with no lock held).

- [ ] **Step 1: Build the lock inventory.** For each mutex in the engine and in `ZoomEngineRuntime`, list: who takes it (callback threads included), and every SDK or blocking call made while it is held. Then reduce to the question R3 asks: *can an SDK call made under lock L wait on, or run synchronously, a callback that takes L?* The SDK's behaviour is not in the repo; use the evidence the repo has: `EngineShareLifecycleTest`'s model (`unSubscribe` invokes the delegate synchronously) and the `RendererCallbackGate` comments. Where the answer depends on undocumented SDK behaviour, say `holds if the SDK never ...; the code protects against the worst case by ...` and mark `read-only`.
  - Engine: `EngineAudio` unsubscribe under `m_subscribe_mtx` (callbacks do not take it: HOLDS, cite the two callback functions and their lock); `EngineShare` single-mutex gate (cite the gate and its test); `EngineVideo` (cite the flag + drain and the move-out); `set_raw_media_active` ordering relative to the maps lock; any `m_raw_media_active`-style plain bool shared between the callback thread and the main thread (data race, not a lock, but note it).
  - Core: is `ZoomEngineRuntime::mutex_` (an `EngineMutex`) ever held across `process_->sendLine`, `readEvent`, `terminate`, `join`? The `ForbidEngineMutexScope` guardrail and existing tests cover the audio-gather path; confirm the **teardown** paths (`leave`, `stopCapture`, `stopReader`, destructor, `join` restart) and `JsonRpcServer`'s `coreMutex` do not hold a lock across the pipe I/O or the process wait.
  - Shell (as the analogue): `MediaCoreSupervisor._gate` vs `DisposeChild`'s `WaitForExit` (the `StopChild` code releases `_gate` before `DisposeChild`: confirm), `EngineHostClient._lane`/`_urgent` held across `ExchangeAsync`'s network wait (they are: that is intended serialisation, but check whether a UI-thread caller can queue behind it; cross-ref R1).
- [ ] **Step 2: Classify and write findings.** Do not stretch the rule: a lock held across a *pipe* write is a different hazard from a lock held across an *SDK* call; record it, classify under R3 only if the pipe peer can wait on the lock holder.
- [ ] **Step 3: Write the regression tests.**
  - `EngineAudioTeardown.UnSubscribeUnderSubscribeLockDoesNotBlockTheCallbackPath`: model `IZoomSDKAudioRawDataHelper` with an `unSubscribe` that blocks until an in-flight `onMixedAudioRawDataReceived` returns; assert no deadlock with a watchdog (join with a 2 s timeout). Fails if a future change makes the callback take `m_subscribe_mtx`. Needs `engine-audio.cpp` linked into the test target with the mock SDK; if it cannot be, `not reachable`, and add a lock-scope guard test instead: scan `engine-audio.cpp` and assert the callbacks' bodies do not reference `m_subscribe_mtx`.
  - `EngineVideoTeardown.NoLockHeldAcrossDestroyRenderer`: from Task 2's mock, `destroyRenderer` asserts both `m_mtx` and `m_targets_mtx` are free (try-lock from inside the hook).
  - `ZoomEngineRuntime.LeaveAndStopCaptureNeverHoldTheRuntimeMutexAcrossAnEngineWrite`: with the fake client blocking `sendLine`, call `leave()` and `stopCapture()`, and in parallel poll `snapshot()` (needs `mutex_`) from a second thread; assert the poll completes in under 100 ms while the pipe is wedged. The existing `SyncSpineNeverBlocksOnAWedgedEnginePipe` covers the spine path; extend to leave/stopCapture only if not already covered (check its body first and cite it instead if it is).
- [ ] **Step 4: Build and run** the same native filters as Task 2, plus `LockHoldGuardrail.*`.
- [ ] **Step 5: File issues for violations.**

---

### Task 4: R4 — Watchdog on async stop confirm (E1-E5)

**Files:**
- Modify: `docs/reference/engine-off-teardown-audit.md` (section `## R4`)
- Create (as findings dictate): `native-shell/CoreVideoPro.MediaCore.Tests/TeardownDeadlineBudgetTests.cs`, additions to `native-shell/CoreVideoPro.MediaCore.Tests/EngineHostTests.cs` or a new `EngineHostShutdownDeadlineTests.cs`, additions to `native/tests/ZoomEngineRuntimeTest.cpp`

**Interfaces:**
- Consumes: `ShutdownBudget` (5 s `ShutdownTimeout`, `ShutdownWatchdog`, `CoreExitGrace` 2 s), `MediaCoreSupervisor.KillWaitMilliseconds` (1500), `MediaCoreSupervisor.StopForAppExit` / `StopChild` / `DisposeChild`, `OutputShutdownCoordinator` (15 s bound, outside the budget by design), `StudioViewModel.StopEngineRawMediaAsync` (12 x 250 ms), `EngineHostClient` (250 ms attach connect, 20 s spawn connect, 2 s exchange connect, 70 s read deadline, 2 s `DisposeAsync`), `EngineHostServer` (2 s lane wait, 10 s request read, 2 s write, `StopForAppExit(10 s)`, `_exit.CancelAfter(3 s)`), `ZoomEngineProcessClient::stop` (1000 ms + 1000 ms), `ZoomEngineRuntime::join` timeouts, the engine's bounded pump.
- Produces: classified R4xE1..E5 cells, a **deadline table** (each async confirm, its bound, its on-expiry action, who sees it), and the R4 regression tests.

- [ ] **Step 1: Enumerate every async stop confirm** on the five paths and build the deadline table with columns: *confirm*, *waiter (thread)*, *bound (constant, file:line)*, *action on expiry*, *visible as*. Required rows: engine `rawMediaActive` after Engine off; `zoom-leave` reply (what bounds a core that never answers? the supervisor `SendAsync` timeout: find it); engine `Leave` completion (no confirm awaited: Spec lead 5, name the backstop); core exit after stdin close (2 s grace, then kill, then 1.5 s kill wait, then 250 ms stderr EOF wait); close-guard output finalisation (15 s, intentionally outside the budget); engine process retirement in `~ZoomEngineRuntime` (1 s then `TerminateProcess`, then another 1 s wait); EngineHost `detach` (2 s), `shutdown` (client 70 s read vs host 10 s stop: which wins; what the console shows), host-side `_exit.CancelAfter(3 s)`.
- [ ] **Step 2: Check the nested sums.** App close: `OutputShutdownCoordinator` (outside) + `ShutdownTimeout` 5 s must contain `CoreExitGrace` 2 s + `KillWaitMilliseconds` 1.5 s + 250 ms stderr wait + descendant sweep; `ShutdownBudgetTests` pins part of it, so cite it and say what it does not cover (the engine child's 2 s inside the core's exit, which is *inside* the core's 2 s grace: if the engine retirement can take 1 s + 1 s and the core's grace is 2 s, the sum is exactly at the edge; compute and state the margin, with `read, not run` on the timing).
- [ ] **Step 3: Classify and write findings.** Expected: engine-off poll `HOLDS`; hosted `Stop()` `VIOLATES` if reachable (shared with R1: cross-reference, do not duplicate the issue); the engine-retire 1 s fuse as a margin finding; host `shutdown` 70 s vs 10 s as a finding on whether the *console* can be left waiting (it cannot if it uses `DisposeAsync`/`detach`, only if it uses hosted `Stop()`: say which).
- [ ] **Step 4: Write the regression tests.**
  - `TeardownDeadlineBudgetTests.AppExitStopWorstCaseFitsInsideShutdownTimeout` (MediaCore.Tests, or WinUI.Tests next to `ShutdownBudgetTests` if `ShutdownBudget` is WinUI-only): pull the constants (`ShutdownBudget.CoreExitGrace`, `MediaCoreSupervisor.KillWaitMilliseconds`, the stderr wait, `ShutdownTimeout`) and assert `grace + kill + stderr <= ShutdownTimeout - margin`. If `ShutdownBudgetTests` already asserts exactly this, cite it and add only the missing term (the engine retirement terms: expose the two 1000 ms values as test-visible constants only if they already are; otherwise record `not reachable without a production constant` as a finding).
  - `EngineHostShutdownDeadline.ConsoleDisposeIsBoundedWhenTheHostNeverReplies` (MediaCore.Tests): a fake pipe server that accepts and stalls; `EngineHostClient.DisposeAsync()` completes within ~2.5 s. (Expected to hold; this pins it.)
  - `EngineHostShutdownDeadline.HostedStopIsBoundedWhenTheHostNeverReplies`: the same stall, through `MediaCoreSupervisor.Stop()` hosted branch, budget 3 s. (Expected to fail today; commit disabled with the issue number: same test as Task 1's, share it and reference it from both findings.)
  - `ZoomEngineRuntime.DestructorCompletesWhileTheEngineNeverExits`: with the fake client whose `terminate()` blocks for N ms, assert the destructor's wall time is bounded by the client's own bound (documents that the runtime adds no unbounded wait of its own).
- [ ] **Step 5: Run**

```powershell
dotnet test native-shell\CoreVideoPro.MediaCore.Tests --filter "FullyQualifiedName~TeardownDeadlineBudget|FullyQualifiedName~EngineHostShutdownDeadline|FullyQualifiedName~EngineHost"
dotnet test native-shell\CoreVideoPro.WinUI.Tests --filter "FullyQualifiedName~ShutdownBudget|FullyQualifiedName~OutputShutdownCoordinator"
native\build-dev\corevideo-native-tests.exe --gtest_filter='ZoomEngineRuntime.*'
```

- [ ] **Step 6: File issues for violations** (reuse the Task 1 issue for the hosted-stop finding).

---

### Task 5: R5 — No STA marshaling (E1-E5)

**Files:**
- Modify: `docs/reference/engine-off-teardown-audit.md` (section `## R5`)
- Create (as findings dictate): `native/tests/EngineThreadingGuardTest.cpp` (source guard), additions to `native-shell/CoreVideoPro.WinUI.Tests` (a project-reference guard)

**Interfaces:**
- Consumes: `main.cpp` (the main-thread loop `ipc_read_line_with_message_pump`, `pump_windows_messages`, `MsgWaitForMultipleObjects`, the `heartbeat` thread body, every thread the engine creates), `engine-video.cpp` / `engine-share.cpp` / `engine-audio.cpp` (which threads call SDK functions), `ZoomEngineRuntime` sender/reader/ingest threads (they must not call the SDK: the core has no SDK), the shell project references (no Zoom SDK in-process), `ZoomMeetingSdkAdapter.cpp` (the core has an in-process SDK adapter: determine whether it is compiled into the shipping core or only a dev path; line 879 calls `CleanUPSDK()`).

- [ ] **Step 1: Enumerate every thread that touches the SDK.** Engine: the main thread (STA with a pump), the SDK's own callback threads, and any thread the engine spawns (`heartbeat`, writers): list the SDK calls each makes. The R5 questions: (a) does any non-owner thread call an SDK function and wait for it (a `std::future`, `SendMessage`, `Invoke`, a condition variable handed to the main thread)? (b) does the main thread ever block without pumping (the `std::this_thread::sleep_for` at shutdown pumps first and last but sleeps between: is a 50 ms non-pumped sleep a violation or the "bounded pumped wait" the spec allows? Decide and justify; note the pump runs before and after each sleep); (c) `Leave()` from the IPC loop vs from `main()`'s exit: same thread? (d) `StopRawRecording` from the SDK status callback thread (Spec lead 4): an SDK call made from the SDK's own callback thread is not marshaling, but it is re-entrancy: record it under R3, cross-reference here; (e) in-process SDK in the core (`ZoomMeetingSdkAdapter`): is it shipping? If compiled and reachable in the shipping core, R5 and R1 apply to it too; if it is dev/legacy, say so and `N/A` with the build-flag evidence.
- [ ] **Step 2: Shell side.** Confirm by project references and `using` scan that the WinUI/MediaCore assemblies contain no Zoom SDK types and no COM interop to it (`rg -n "ZOOMSDK|zoom_sdk|sdk.dll" native-shell --glob '*.cs' --glob '*.csproj'`). The shell reaches the SDK only through core RPC then engine IPC; every hop is an async request with a timeout (cross-ref R4).
- [ ] **Step 3: Classify and write findings.** Expected mostly `HOLDS`/`N/A`; the engine shutdown's non-pumped sleep and the status-callback re-entrancy are the interesting cells.
- [ ] **Step 4: Write the regression tests** (guards, since the SDK is not linkable):
  - `EngineThreadingGuard.OnlyTheMainThreadAndSdkCallbacksCallTheSdkControllers`: text-scan `main.cpp` for `std::thread` creations and assert each thread body (the `heartbeat` lambda) contains no `ZOOMSDK::` call. Watched-failing proof by running the same scan over an inline sample with an SDK call in a thread body.
  - `WinUiAssembliesNeverReferenceTheZoomSdk` (WinUI.Tests): scan the `.csproj` files and `.cs` sources of `CoreVideoPro.WinUI` and `CoreVideoPro.MediaCore` for SDK identifiers (allow-list: comment mentions and the `docs`), failing on any hit.
  - If (e) finds the in-process adapter shipping, add the missing test under R1/R5 for it and file the issue.
- [ ] **Step 5: Build and run** the native `EngineThreadingGuard.*` filter and the WinUI filter. **Step 6:** file issues.

---

### Task 6: Assemble the audit, the live checklist, the BACKLOG row and the cross-links

**Files:**
- Modify: `docs/reference/engine-off-teardown-audit.md` (matrix, live checklist, issues filed, summary)
- Modify: `docs/BACKLOG.md` (row T4.3)
- Modify: `docs/reference/engine-teardown-order.md` (link line; the one-line correction only if Task 2 found the "Remaining gaps" sentence stale)

**Interfaces:**
- Consumes: the five rule sections, the filed issue numbers, the test names and their watched-failing evidence.
- Produces: the finished audit and the updated backlog row.

- [ ] **Step 1: Fill the 5 x 5 matrix.** Rows R1..R5, columns E1..E5, each cell `HOLDS` / `VIOLATES #n` / `ACCEPTED` / `N/A` linking to its finding ID. Count the cells: 25, none blank. Add a summary of counts and the confidence mix (`read-only` / `test-proved`).
- [ ] **Step 2: Write the live checklist** from every `read, not run` claim: for each, the rig step and the observable (for example: leave/rejoin ten times in the test meeting with Engine on and confirm no stale `raw_media_status` and no engine process left; close the app mid-meeting and confirm the engine process is gone and the Zoom participant list drops the producer within the engine's exit time; kill the core and confirm the engine dies and the next join starts clean; kill the WinUI process with an EngineHost-owned core and confirm production continues and a restarted console attaches read-only; a slow `CleanUPSDK` rig step if one can be simulated). Use the test meeting's full pwd link (the bare ID fails) and no input injection (the owner is on the PC): control API, WM_ messages and UIA Invoke only, per the standing rules. State that none were run.
- [ ] **Step 3: Issues filed table**: finding ID, issue number, one-line fix sketch, test name, disabled/enabled status.
- [ ] **Step 4: Owner sign-off block** for every `ACCEPTED EXCEPTION`, left blank.
- [ ] **Step 5: Update `docs/BACKLOG.md` row T4.3.** Re-read the row first (`rg -n "T4\.3" docs/BACKLOG.md`); set the status to "audit written, <n> violations filed (#...), <m> accepted pending owner sign-off", link `docs/reference/engine-off-teardown-audit.md`, and update the line near the bottom that says "#445 teardown audit not started" (`rg -n "#445" docs/BACKLOG.md`) and the "Owner direction" paragraph's "#445 teardown audit" entry in the still-unstarted list. Keep the table format of neighbouring rows.
- [ ] **Step 6: Cross-link.** Add one line to `docs/reference/engine-teardown-order.md` pointing at the audit. If Task 2 proved "Remaining gaps" stale or wrong, replace that single sentence with the audit's verdict and the finding ID, nothing else.
- [ ] **Step 7: Final verification.**

```powershell
git status --short
git diff --stat
```

`git status` shows only: the audit doc, the three doc edits above, the new test files, and CMake/test registrations. No file under `native/src`, `native/zoom-engine`, `native-shell/*/Services`, `ViewModels` or `Models` changed. Run each touched suite one last time (native filters from Tasks 2-5, both `dotnet test` filters) and paste the pass counts into the PR description. Do not commit or push unless asked.

---

## Recommended executor

Model and effort per task. fable is used only where a wrong reading is expensive (a `holds` on SDK re-entrancy or lock-across-SDK that is actually a deadlock would let a freeze class ship as "audited").

| Task | Model | Effort | Why |
|---|---|---|---|
| 0. Pin commit, skeleton, template | sonnet | low | Mechanical: greps, a template, copying the rule table. |
| 1. R1 UI-thread audit + tests | sonnet | high | Thread attribution across `ConfigureAwait`/`UiDispatch` is fiddly but checkable by tests; the hosted-`Stop` and re-entry leads are already named. |
| 2. R2 order audit + guard + mock-renderer tests | opus | high | Needs correct reading of the engine exit sequence, EOF-vs-quit timing and member destruction order; a wrong `holds` is costly. CMake test-target wiring is the fiddly part. |
| 3. R3 locks-across-SDK audit + tests | fable | high | The expensive wrong reading: whether the SDK can synchronously call back under our lock, from only the repo's mock evidence. Needs the most careful lock inventory and honest `read-only` labelling. |
| 4. R4 deadline table + budget tests | opus | high | Arithmetic over nested deadlines across three processes and an easy-to-misread 70 s vs 10 s vs 2 s interplay; tests are straightforward once the table is right. |
| 5. R5 STA audit + guard tests | sonnet | medium | Mostly enumeration and negative evidence; the interesting cells are already flagged. Escalate to opus if the in-process SDK adapter turns out to ship. |
| 6. Assemble, live checklist, BACKLOG, links | sonnet | medium | Assembly and consistency. The one-line BACKLOG row edit alone can go to haiku/low. |
| Review (once, after Task 6) | opus | high | Re-derives five `HOLDS` cells and every violation test's failure reason, per Review Focus. |

**Estimated AI-assisted effort:** Task 0: 0.5 h; Task 1: 2 h; Task 2: 3 h (the CMake/mock linking is the risk); Task 3: 2.5 h; Task 4: 2 h; Task 5: 1.5 h; Task 6: 1.5 h; review and fixes: 1.5 h. **About 14.5 hours total (range 12-18 h)**, run mostly sequentially because all tasks append to one audit file; Tasks 1, 4 and 5 touch only shell/other-language files and may run in parallel with Tasks 2-3 if each writes its own rule section and Task 6 does the merge.

## Self-review

- Every spec acceptance item maps to a task: acceptance 1 (matrix, SHA, fields) -> Tasks 0, 6; 2 (fix sketch, issue, watched-failing test, blank sign-off) -> Tasks 1-5 step "file issues" and Task 6 step 4; 3 (suites pass, disabled tests carry issue numbers) -> Global Constraints and each task's run step; 4 (BACKLOG, link, possible correction) -> Task 6; 5 (live checklist) -> Task 6 step 2; 6 (no production edit) -> Global Constraints, Review Focus 6, Task 6 step 7.
- The five rules and E1-E5 are referenced from the spec, not restated, so the two documents cannot drift.
- The preliminary leads in the spec (hosted `Stop`, `StopMediaCoreSession` re-entry, EOF vs `quit` with a 1 s fuse, `stop_raw_media` in an SDK callback, core `leave()` not awaiting the engine, engine-off poll, hard-kill exception, EngineHost detach) are each assigned to a task: 1 -> Tasks 1 and 4; 2 -> Task 1; 3 -> Tasks 2 and 4; 4 -> Tasks 2, 3 and 5; 5 -> Task 4; 6 -> Tasks 1 and 4; 7 -> Task 2; 8 -> Tasks 1 and 4.
- Placeholders: issue numbers and finding contents are produced by execution and are deliberately not pre-written; every other step names its files, commands and assertions.
