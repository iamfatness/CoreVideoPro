# Capture ReadSample Lane Isolation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** No camera, whether it is unplugged, wedged, or never replies, can block the native command lane, the render tick, or another camera. Every Media Foundation call that can hang runs off the lane. If it never returns, it is abandoned with a bound and reported loudly. Device liveness is measured or absent.

**Architecture:** Keep PR #994's async `IMFSourceReaderCallback` read (`UvcAsyncRead.h`). Move the Media Foundation calls behind an `IUvcCaptureBackend` seam. Make each session's state self-owned (`shared_ptr` captured by its worker). Replace every lane-side `thread.join()` with a non-blocking hand-off to a per-adapter `BoundedSessionReaper` (`joinFor` / abandon / late-exit, the #1030 pattern). Fence reopen while a predecessor is alive. Publish measured liveness (`lastFrameAgeMs`, `readReplyAgeMs`, `readOutstandingMs`, `sessionPhase`, `releaseMs`) for the #758 policy.

**Tech Stack:** C++20 core (`native/`), Media Foundation (`mfreadwrite`), the in-house gtest shim (`native/tests/gtest/gtest.h`), .NET 9 shell (`native-shell/CoreVideoPro.MediaCore`, `CoreVideoPro.WinUI`) with its xUnit test projects, Python QA scripts under `scripts/qa/`.

**Spec:** `docs/superpowers/specs/2026-10-23-capture-readsample-lane-isolation-design.md`

**Recommended executor:**

| Task | Model | Effort | Reason |
|---|---|---|---|
| 1 Release-phase evidence + live baseline | opus | high | Instrumentation plus interpreting live MF teardown timings on two devices; this sets the bound. |
| 2 Backend seam extraction (pure move) | opus | high | A large mechanical refactor of a COM-heavy file where any logic drift is a silent capture regression. |
| 3 `UvcAsyncRead` timing + awaitable flush | sonnet | high | A small, well-specified state-machine extension; the tests carry the risk. |
| 4 `BoundedSessionReaper` | sonnet | high | Generic concurrency helper with a direct precedent (#1035); needs precise lifetime tests. |
| 5 Adapter integration: no lane joins, fence, phase watchdog | opus | xhigh | Lock ordering, abandoned-thread lifetime and reconnect semantics; the core of the defect. |
| 6 Measured liveness fields + signalPresent latch fix | sonnet | medium | Additive fields through known projection points, with an honest-reporting rule. |
| 7 Shell: `connecting` / release-stalled handling | sonnet | high | Changes the fallback contract; a mistake reopens the two-reader collision. |
| 8 RPC lane fault test | sonnet | medium | Follows `DelayedJoinDoesNotBlockCommandsAndCancellationAnswersOriginalId`. |
| 9 Live fault knob, drill and real unplug | fable | high | Runs on the owner's box with real hardware and owner hands; a false pass is expensive. |
| 10 Documentation | sonnet | low | A new reference doc needs judgement about what was and was not proven; not a mechanical edit. |

**Estimated AI-assisted effort:** 2.0 + 3.0 + 1.5 + 2.0 + 3.5 + 2.0 + 2.0 + 1.5 + 3.0 + 0.75 = **about 21.25 h**. The owner is needed for Task 9 (physical unplug) and for the Review Focus ruling on the live fault knob (ruled: D11 allows it).

## Global Constraints

- **The command lane never waits on a device.** No `join()`, `wait()` without a bound, or Media Foundation call may run on the RPC thread, the render thread, or under `coreMutex`, for any UVC lifecycle path (`connect`, `disconnect`, adapter destruction). Joining an already-exited thread is allowed **only** on the reaper thread.
- **Keep #994's read semantics:** exactly one outstanding `ReadSample`, priming before conversion, no priming after `CURRENTMEDIATYPECHANGED` / `ERROR` / `ENDOFSTREAM`, original `IMFSample` references, timestamps, geometry and color hints. Task 2 is a pure move. No logic edits go in the same commit.
- **Never two readers on one device.** No new session may open a symbolic link whose previous session has not exited. The shell must not open its WinRT bridge for a device the core reports `connecting` or release-stalled.
- **Loud, measured, or absent.** Every failure carries its `hr` or phase and duration. An age that was never measured is omitted, never reported as 0. `signalPresent` is derived from a measured age, never latched.
- **Do not fix #811 by delay.** No reconnect sleep and no suppressed `0x8007001F`. Report the reconnect result as observed.
- Constants, exact: `kUvcSessionReleaseBoundMs = 4000` (confirm in Task 1: at least 10x the worst measured healthy release), `kUvcFlushWaitMs = 500`, `kUvcSignalLossMs = 1500`, `kMaxAbandonedUvcSessions = 4`, the existing `kUvcNoFirstFrameTimeoutMs = 4000`, and the read wait `50 ms` (unchanged).
- Lock order: `UvcCaptureDeviceAdapter::mutex_` → `BoundedSessionReaper::mutex_` → session-state mutex. The reaper never holds its mutex while calling back into the adapter.
- Build Release: `cmake --build native\build-dev --config Release --target corevideo-native-tests corevideo-native`. Debug and Release write the **same** exe path, so check the size (Release is about 2.3 MB, Debug about 8 MB) before believing any timing.
- The gtest shim's `--gtest_filter` takes **one** positive wildcard. No `:` lists and no `-` negation. Run suites separately.
- **Windows build required.** UVC code is under `COREVIDEO_WITH_UVC` and `_WIN32`. Stub and mac CI cannot compile it, so every task's tests must be run on the dev core before claiming them.
- Never run the native suite or any drill while the owner's installed app is live (streaming, recording, virtual camera on). Use the control API and headless processes only. No `SendKeys` and no focus stealing.
- Fault seams for unit and RPC tests are **structural** (a test-only factory in a header that nothing outside `native/tests/` includes), per `docs/reference/fault-injection-seams.md`.

## Review Focus

1. **Lifetime of an abandoned worker.** Everything the worker can touch must be co-owned by the `shared_ptr<UvcSessionState>` it captured, or COM ref-counted. Grep the worker body for `this` and for the adapter, `DeviceEntry` and `MediaCore`. Any raw pointer is a use-after-free waiting for a slow driver.
2. **No hidden lane joins.** `entry.session.reset()`, `~UvcCaptureSession`, `~UvcCaptureDeviceAdapter` and any `std::thread` destructor reachable from `connect`/`disconnect` must not join. Check that the destructor of the session *handle* only signals.
3. **Reaper lock order** (Global Constraints). In particular, the successor-start hook runs after the reaper releases its mutex.
4. **The shell fallback change.** Confirm that `connecting` and release-stalled never fall through to `_captureFrameReader` opening the bridge, and that the normal path (`connected`) is byte-for-byte unchanged.
5. **Honest fields.** `lastFrameAgeMs` is absent before the first frame, not 0. `signalPresent` drops within 1.5 s of the last sample. `warning` is no longer cleared while `connectionState == "error"`.
6. **Owner ruling needed:** may the live drill use a headless env knob (`COREVIDEO_QA_UVC_FAULT`, following `RecordingWriteStallQa.h`), or must acceptance item 6 run through the structural seam inside a test-only core? Task 9 is written for the knob, with a fallback.

---

### Task 1: Release-phase evidence and a live baseline (sets the bound)

**Files:**
- Modify: `native/src/modules/UvcCaptureDeviceAdapter.cpp` (`run()` at `:248-309`: timestamps around loop exit, `Flush`, the COM release scope, `MFShutdown`; one `nativeLogf` line)
- Create: `artifacts/uvc-release-993-baseline/` (evidence only, not committed if the repo's artifact rule says so; check `.gitignore`)

**Interfaces:**
- Produces the log line used by Tasks 5 and 9:
  `[uvc-capture] release '<id>' session=<n> stop->loop-exit=<ms> flush=<ms>(onFlush=<0|1>) reader-release=<ms> mf-shutdown=<ms> total=<ms> outcome=<exited|abandoned|late-exit>`
  (In this task `onFlush` is always `0` and `outcome` always `exited`. Task 3 and Task 5 make them real.)

- [ ] **Step 1: Verify the MF semantics the spec relies on.** Read the three Microsoft pages linked in the spec (ReadSample async, Flush, MFCreateSourceReaderFromMediaSource). Note any wording that contradicts the spec's "Media Foundation semantics" list in the task report. If `Flush` in async mode is documented as anything other than "returns immediately; `OnFlush` signals completion", STOP and report.
- [ ] **Step 2: Add the timestamps and the log line.** Use `std::chrono::steady_clock` only. Add no behavior change. The session number is a per-adapter atomic counter.
- [ ] **Step 3: Build Release and confirm the size.**
- [ ] **Step 4: Live baseline (headless owned core, no installed app running).** For the dev-box webcam and the HD60 S+ (native id from #811, `376f43680a4bb8eb`): connect via the control API, wait for `signalPresent`, disconnect, repeat 10 times each. Then run the #811 immediate-reconnect reproducer once per device (`scripts/qa/native-i420-capture.py --duration 30 --warmup 5 --reconnects 1`). Record every release line, plus the `[req]` / `[cmd]` lines for `disconnect-capture-device`.
- [ ] **Step 5: Decide the bound.** Set `kUvcSessionReleaseBoundMs` = max(4000, 10x worst `total`). If any healthy release is above 400 ms, report the measured value. Do not silently raise the bound past 4000 ms, because the owner should see a slow release.
- [ ] **Step 6: Commit**

```bash
git add native/src/modules/UvcCaptureDeviceAdapter.cpp
git commit -m "Log per-phase UVC session release timings (#993, #811 evidence)"
```

---

### Task 2: Move Media Foundation calls behind `IUvcCaptureBackend` (pure move)

**Files:**
- Create: `native/src/modules/UvcCaptureBackend.h` (the interface and `UvcNegotiated`, Windows-only)
- Create: `native/src/modules/UvcCaptureSessionCore.h` (`UvcSessionState` and the worker body: `run`, `readLoop`, conversion, publish; templated on nothing, depends only on `IUvcCaptureBackend`)
- Create: `native/src/modules/UvcCaptureTestFactory.h` (declares `createUvcCaptureDeviceForTest`; included only from `native/tests/`)
- Modify: `native/src/modules/UvcCaptureDeviceAdapter.cpp` (`MfUvcCaptureBackend` keeps the existing `MFCreateDeviceSource` / `negotiate` / `refreshOutputType` / `ReadSample` / `Flush` code verbatim; the adapter takes a `BackendFactory` and a `Discover` function)
- Create: `native/tests/FakeUvcCaptureBackend.h`
- Create: `native/tests/UvcCaptureSessionTest.cpp`
- Modify: `native/CMakeLists.txt` (register `tests/UvcCaptureSessionTest.cpp` beside `tests/UvcAsyncReadTest.cpp` at `:804`, same guard)

**Interfaces:**
```cpp
namespace corevideo::modules {
struct UvcNegotiated { int width = 0, height = 0, fps = 0; std::string fourcc;
  bool nv12 = true, fullRange = false, bt601 = false; uint32_t rateNum = 0, rateDen = 0; };
class IUvcCaptureBackend {
 public:
  virtual ~IUvcCaptureBackend() = default;
  virtual HRESULT open(const std::wstring& symbolicLink, IMFSourceReaderCallback* callback) = 0;
  virtual bool negotiate(UvcNegotiated& out, std::string& error) = 0;
  virtual HRESULT requestRead() = 0;
  virtual bool refreshOutputType(UvcNegotiated& out) = 0;
  virtual HRESULT flush() = 0;
  virtual void release() = 0;
};
using UvcBackendFactory = std::function<std::unique_ptr<IUvcCaptureBackend>()>;
using UvcDiscover = std::function<std::optional<std::vector<UvcDiscoveredDevice>>()>;
// UvcCaptureTestFactory.h — tests only:
std::unique_ptr<ICaptureDevice> createUvcCaptureDeviceForTest(UvcBackendFactory, UvcDiscover,
    std::shared_ptr<CpuSourcePreparation> = {}, int64_t noFirstFrameTimeoutMs = uvc::kUvcNoFirstFrameTimeoutMs);
}
```
`FakeUvcCaptureBackend` has latches `holdOpen`, `holdNegotiate` and `holdRelease` (each a `std::promise<void>` the test fulfils), modes `replyNever`, `replyLateAfterCancel`, `failFirstRead(hr)` and `deliverNv12(width, height, n)`, and counters `opens`, `requests`, `releases` and `concurrentOpenOnLink` (max observed). It invokes the real `UvcAsyncRead` callback from its own thread, as MF does.

- [ ] **Step 1: Write the tests first (against today's behavior):**
  - `UvcCaptureSession.DeliversNv12AsI420WithOriginalGeometryAndColorHints`
  - `UvcCaptureSession.NoFirstFrameWatchdogFailsLoudWhenReadsNeverReply` (timeout 100 ms via the factory parameter)
  - `UvcCaptureSession.FormatChangeRefreshesOutputTypeBeforeNextRequest`
  - `UvcCaptureSession.DeviceInvalidatedMapsToRemovedWarning` (`MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED`)
  - `UvcCaptureSession.FirstReadGenFailureIsLoudWithHr` (`0x8007001F`, warning contains `hr=0x8007001F`)
- [ ] **Step 2: Do the move.** `MfUvcCaptureBackend` contains the moved code verbatim. Diff review: the only allowed changes in moved MF code are `reader` becoming a member and `fail(...)` strings becoming `error` out-params.
- [ ] **Step 3: Build Release, run** `UvcCaptureSession.*`, `UvcAsyncRead.*`, `UvcCaptureSupport.*` and `UvcIsoArrivalQueue.*` (separately). All pass.
- [ ] **Step 4: Smoke-test live:** the webcam connects, `first frame` is logged, and disconnect works (same as the Task 1 baseline).
- [ ] **Step 5: Commit** `Move UVC Media Foundation calls behind a backend seam (no behavior change, #993)`

---

### Task 3: `UvcAsyncRead` reply timing and an awaitable flush

**Files:**
- Modify: `native/src/modules/UvcAsyncRead.h`
- Modify: `native/tests/UvcAsyncReadTest.cpp`

**Interfaces:**
```cpp
struct Timing { int64_t requestedAtMs = -1, repliedAtMs = -1, sampleAtMs = -1; bool pending = false; };
Timing timing() const;                                   // steady ms, -1 = never
bool waitFlushed(std::chrono::milliseconds bound);       // true once OnFlush ran after the last flush request
void noteFlushRequested();                               // called just before backend.flush()
```
`OnFlush` must record completion **even after `cancel()`**. Retirement fences samples, not flush completion. `sampleAtMs` is stamped only for replies that carry an `IMFSample`.

- [ ] **Step 1: Failing tests:**
  - `UvcAsyncRead.TimingStampsRequestReplyAndSampleSeparately` (a stream tick updates `repliedAtMs` and leaves `sampleAtMs` alone)
  - `UvcAsyncRead.PendingAgeIsVisibleWhileARequestNeverReplies`
  - `UvcAsyncRead.FlushCompletionIsObservableAfterCancel`
  - `UvcAsyncRead.WaitFlushedTimesOutWhenOnFlushNeverArrives`
- [ ] **Step 2: Implement.** Keep all six existing tests green, unchanged.
- [ ] **Step 3: In the session core,** call `noteFlushRequested()`, then `backend.flush()`, then `waitFlushed(kUvcFlushWaitMs)` before `backend.release()`. Log `onFlush=1/0` in the Task 1 line.
- [ ] **Step 4: Build and run** `UvcAsyncRead.*`, then `UvcCaptureSession.*`.
- [ ] **Step 5: Commit** `Stamp UVC read reply ages and wait for OnFlush with a bound (#993)`

---

### Task 4: `BoundedSessionReaper`

**Files:**
- Create: `native/src/modules/BoundedSessionReaper.h` (header-only, platform-free)
- Create: `native/tests/BoundedSessionReaperTest.cpp`
- Modify: `native/CMakeLists.txt` (the UNCONDITIONAL test list, beside `tests/CaptureSessionLifecycleTest.cpp`)

**Interfaces:**
```cpp
namespace corevideo::modules {
// A retiring worker: signal() is non-blocking; exited() flips last; join() is
// called only after exited() (so it is instant); detach() is abandonment.
struct ReapableSession {
  virtual ~ReapableSession() = default;
  virtual void signalStop() noexcept = 0;
  virtual bool waitExitedFor(std::chrono::milliseconds) = 0;
  virtual void joinExited() = 0;
  virtual void detach() noexcept = 0;
};
class BoundedSessionReaper {
 public:
  struct Outcome { std::string key; uint64_t session; int64_t releaseMs; enum Kind { Exited, Abandoned, LateExit } kind; };
  using OnOutcome = std::function<void(const Outcome&)>;   // called with NO reaper lock held
  BoundedSessionReaper(std::chrono::milliseconds bound, size_t maxAbandoned, OnOutcome);
  ~BoundedSessionReaper();                 // one shared deadline for all, then abandon; never unbounded
  void retire(std::string key, uint64_t session, std::unique_ptr<ReapableSession>);  // never blocks
  bool releasing(const std::string& key) const;   // retiring, inside the bound
  bool stalled(const std::string& key) const;     // abandoned, not yet late-exited
  size_t abandonedCount() const;
  bool atAbandonCap() const;
};
}
```

- [ ] **Step 1: Failing tests** (fake `ReapableSession` driven by latches):
  - `BoundedSessionReaper.RetireNeverBlocksTheCaller` (a session that never exits; `retire` returns in under 5 ms)
  - `BoundedSessionReaper.ExitWithinBoundJoinsAndReportsExited`
  - `BoundedSessionReaper.NoExitAbandonsAfterBoundAndFencesTheKey` (`stalled(key)` is true; `detach` is called once)
  - `BoundedSessionReaper.LateExitAfterAbandonLiftsFenceAndReportsLateExit`
  - `BoundedSessionReaper.OneStuckSessionDoesNotDelayAnotherKeysExitBeyondOneBound`
  - `BoundedSessionReaper.AbandonCapIsReported`
  - `BoundedSessionReaper.DestructorUsesOneSharedDeadline` (3 stuck sessions, bound 100 ms; the destructor returns in under 300 ms total)
  - `BoundedSessionReaper.OutcomeCallbackRunsWithoutReaperLock` (the callback calls `releasing()` without deadlocking)
- [ ] **Step 2: Implement.** Watching for late exit after abandon: keep a list of abandoned sessions, poll `waitExitedFor(0)` on each reaper wake (at least every `bound`), and `joinExited()` once true. `detach()` gives up the `std::thread`, but the session object stays in the list until it exits.
- [ ] **Step 3: Build and run** `BoundedSessionReaper.*`.
- [ ] **Step 4: Commit** `Add a bounded off-lane session reaper with abandon and late-exit (#993)`

---

### Task 5: Adapter integration — no lane joins, reopen fence, phase watchdog

**Files:**
- Modify: `native/src/modules/UvcCaptureSessionCore.h` (self-owned `shared_ptr<UvcSessionState>`, worker captures a copy; atomic `phase` + `phaseSinceMs`; `exited` set last under the state mutex)
- Modify: `native/src/modules/UvcCaptureDeviceAdapter.cpp` (`connect` `:648-675`, `disconnect` `:677-694`, `~UvcCaptureDeviceAdapter` `:608-613`, `captureVideoTick` `:714-751`; replace the comment at `:683-684` with a true one)
- Modify: `native/tests/UvcCaptureSessionTest.cpp` (adapter-level tests)

**Interfaces:**
- The session handle implements `ReapableSession`. `signalStop()` sets `stop_` and calls `callback->cancel()`. It does no Media Foundation calls and takes no locks except the callback's and the state's leaf mutexes.
- Adapter state per `DeviceEntry`: `std::optional<PendingConnect> pending` (`outputSourceId`, revision).
- Phase-age rule, evaluated in `captureVideoTick` (reads atomics only): `opening`/`negotiating` older than `noFirstFrameTimeoutMs` → `connectionState="error"`, warning `"Camera open did not complete in <n> s (driver not responding)."`, then retire the session through the reaper.

- [ ] **Step 1: Failing adapter tests (fake backend):**
  - `UvcCaptureAdapter.DisconnectReturnsPromptlyWhileReleaseIsBlocked` (`holdRelease`; `disconnect()` < 50 ms wall)
  - `UvcCaptureAdapter.DisconnectReturnsPromptlyWhileOpenIsBlocked` (`holdOpen`)
  - `UvcCaptureAdapter.DisconnectReturnsPromptlyWhileNegotiateIsBlocked`
  - `UvcCaptureAdapter.NeverReplyingReadDoesNotStallAnotherCamerasTick` (two devices; device B's frames keep advancing through `deliverVideo` while A is stuck and disconnected)
  - `UvcCaptureAdapter.OpenHangIsReportedAfterTimeoutWithoutJoining`
  - `UvcCaptureAdapter.ReconnectWhilePredecessorReleasesReportsConnectingAndStartsOneSuccessor` (`concurrentOpenOnLink == 1` throughout)
  - `UvcCaptureAdapter.ReconnectAfterAbandonIsRefusedLoudly` and `...LateExitAllowsReconnect`
  - `UvcCaptureAdapter.DisconnectCancelsAPendingConnect`
  - `UvcCaptureAdapter.LateCallbackFromRetiredSessionNeverReachesSuccessorFrames` (the successor's `sourceEpoch` and `frameId` sequence contains no predecessor frame)
  - `UvcCaptureAdapter.AdapterDestructionIsBoundedWithAHungRelease` (destructor < bound + 200 ms; afterwards the test opens the latch and waits for the fake's thread to finish, so the test exe does not leak)
  - `UvcCaptureAdapter.FirstReadGenFailureAfterReconnectIsLoudNotStalled` (the #811 shape: `failFirstRead(0x8007001F)` on session 2; the lane returns promptly and the state is `error` with the hr)
- [ ] **Step 2: Implement.** `disconnect` = move the handle out of the entry, `signalStop()`, `reaper_.retire(...)`, set `detected`, return. `connect` = consult `reaper_.stalled/releasing/atAbandonCap` per spec Section 5. The successor-start hook (`startPending(key)`) takes `mutex_` and is called from the reaper's `OnOutcome`, which runs with no reaper lock held.
- [ ] **Step 3: Run the Review Focus 1 grep:** the worker body may reference only `state->` and locals.
- [ ] **Step 4: Build and run** `UvcCaptureAdapter.*`, `UvcCaptureSession.*`, `BoundedSessionReaper.*`, `UvcAsyncRead.*`, `UvcIsoArrivalQueue.*` and `CaptureDeviceLifecycle.*` (separately), plus the full `corevideo-native-tests` once. Report the pass/fail counts honestly. A pre-existing flaky failure is reported, not erased by a rerun.
- [ ] **Step 5: Live smoke:** repeat the Task 1 baseline. The release lines now show `onFlush=1` and `outcome=exited`. `[cmd] 'disconnect-capture-device'` never logs ≥ 30 ms.
- [ ] **Step 6: Commit** `Never join a camera session on the command lane; fence reopen until release (#993)`

---

### Task 6: Measured liveness fields and the `signalPresent` latch fix

**Interfaces (exposed for #758):** `UvcSessionState` exposes, as lock-free reads for the adapter's liveness publication:
- `lastSampleSteadyNs` (`std::atomic<int64_t>`): steady-clock time in ns of the last delivered sample, 0 = never.
- `terminalErrorSteadyNs` (`std::atomic<int64_t>`): steady-clock time in ns when the session first reported a terminal error or device removal, 0 = none; set once, a second signal never moves it.
- `errorClass` (`std::atomic<UvcErrorClass>`): `None`, `DeviceRemoved` (`MEVideoCaptureDeviceRemoved` or `ReadSample` removal HRESULT), `ReadError`, `ReleaseTimeout`, set with `terminalErrorSteadyNs`.

The #758 plan (`2026-10-23-capture-dropout-policy.md`) Task 4 consumes these. **#758 Task 4 must follow #993 Task 5** (they share `UvcCaptureDeviceAdapter` regions) and uses the fields above rather than adding its own.

**Files:**
- Modify: `native/src/modules/Interfaces.h` (`CaptureDeviceInfo` `:1023-1060`: add `std::optional<int64_t> readReplyAgeMs, readOutstandingMs, releaseMs;` and `std::string sessionPhase;`)
- Modify: `native/src/core/CoreStateCapture.cpp` (`captureDeviceArray` `:303`) and `native/src/core/MediaCore.cpp:287`: emit each optional only when set; emit `sessionPhase` only when non-empty
- Modify: `native/src/modules/UvcCaptureDeviceAdapter.cpp` (`captureVideoTick`: derive the fields from `UvcAsyncRead::Timing` and the phase; `signalPresent = lastFrameAgeMs && *lastFrameAgeMs <= kUvcSignalLossMs`; do not clear `warning` while `connectionState == "error"`)
- Modify: the contract schema or golden files if `ContractParity.*` covers `capture-devices` (grep `contracts/` for `signalPresent`; additive optional fields only)
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/CoreProtocolParser.cs` and the `NativeCaptureDeviceStatus` model (parse the new optional fields; no UI)
- Tests: `native/tests/UvcCaptureSessionTest.cpp`, `native/tests/ContractParityTest.cpp` (if the schema changed), `native-shell/CoreVideoPro.MediaCore.Tests/NativeMediaCoreStateMapperTests.cs`

- [ ] **Step 1: Failing tests:**
  - `UvcCaptureAdapter.LastFrameAgeIsAbsentBeforeTheFirstFrame`
  - `UvcCaptureAdapter.SignalDropsWithinLossWindowWhenReadsStopReplying` (fake clock or a 1500 ms real wait; assert `signalPresent=false`, `readOutstandingMs` > 1000, `lastFrameAgeMs` > 1500)
  - `UvcCaptureAdapter.ErrorWarningIsNotClearedByAHeldFrame` (vanished from discovery while holding an old frame)
  - `UvcCaptureAdapter.ReleaseStalledPhaseIsReported`
  - a shell parser test: the fields round-trip, and an absent field stays `null`
- [ ] **Step 2: Implement. Build and run** the native and `CoreVideoPro.MediaCore.Tests` suites.
- [ ] **Step 3: Commit** `Report measured UVC liveness; stop latching signalPresent (#993, feeds #758)`

---

### Task 7: Shell — `connecting` is native-pending; never bridge a releasing device

**Files:**
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/NativeUvcCapturePolicy.cs` (add `FindPendingOrConnectedDevice` and `IsReleaseBlocked(status)`; keep `FindConnectedDevice` unchanged)
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs` (`TryConnectNativeUvcCaptureAsync` `:6736-6790` and its no-first-frame fallback; `ConfirmNativeFirstFrameAsync`)
- Tests: `native-shell/CoreVideoPro.MediaCore.Tests/NativeUvcCapturePolicyTests.cs`, plus a view-model test if the existing test project can reach the method (`CoreVideoPro.WinUI.Tests`); otherwise extract the decision into the policy class and test it there

**Interfaces:**
```csharp
public enum NativeConnectDecision { Committed, Pending, ReleaseBlocked, FallBackToBridge }
public static NativeConnectDecision DecideNativeConnect(NativeCaptureDeviceStatus? match);
public static bool MayOpenBridge(IReadOnlyList<NativeCaptureDeviceStatus> devices, string stableId, string? nativeId);
```

- [ ] **Step 1: Failing tests:**
  - `connected` → Committed (unchanged path)
  - `connecting` → Pending; `ConfirmNativeFirstFrameAsync` keeps polling and the bridge is not opened
  - `error` with `sessionPhase == "release-stalled"` → ReleaseBlocked; `MayOpenBridge == false`; the warning is surfaced
  - device absent or unknown → FallBackToBridge (unchanged)
  - no-first-frame fallback: after `DisconnectNativeCaptureDeviceAsync`, the bridge opens only once the core reports `detected` for that device within the bound; otherwise ReleaseBlocked
- [ ] **Step 2: Implement.** WinUI rules: read `docs/reference/winui-crash-class-0xc000027b.md` first. Do not touch bound collections off the UI thread.
- [ ] **Step 3: Build and run** the shell test projects (`dotnet test`).
- [ ] **Step 4: Commit** `Shell: treat native connecting as pending and never bridge a releasing camera (#993)`

---

### Task 8: RPC lane fault test

**Files:**
- Modify: `native/tests/JsonRpcServerTest.cpp` (model it on `DelayedJoinDoesNotBlockCommandsAndCancellationAnswersOriginalId` at `:805`)

- [ ] **Step 1: Write the test** `JsonRpcServer.NeverReplyingCameraDisconnectDoesNotBlockTheLane`: build a `ModuleSet` whose `captureDevice` is `createUvcCaptureDeviceForTest` with a fake that delivers 5 frames, then accepts requests and never replies, with `holdRelease` closed. Input stream: `connect-capture-device`, a short wait (have the fake signal when frame 5 is delivered before feeding the rest: use a blocking `std::istream` adapter, or split into two `handle()` phases if `run()` cannot wait), then `disconnect-capture-device`, `ping`, `media-core-sync`. Assert that all three ids are answered, the elapsed time is < 2 s, and `LockHoldGuardrail::statsForSite("cmd.handle").worstHeldUs < 5000`. Then open the latch and let the reaper finish before the test returns.
- [ ] **Step 2: Prove the test bites:** temporarily restore a join in `disconnect` (a local scratch edit, not committed). The test must fail by timeout. Record that in the task report, then revert.
- [ ] **Step 3: Build and run** `JsonRpcServer.*`.
- [ ] **Step 4: Commit** `Test: a never-replying camera cannot block the RPC lane (#993)`

---

### Task 9: Live fault knob, drill, and real unplug on the dev box

**Files:**
- Create: `native/src/modules/UvcCaptureFaultQa.h` (env `COREVIDEO_QA_UVC_FAULT=read-no-reply|release-hang`, `COREVIDEO_QA_UVC_FAULT_DEVICE=<stable id>`, `COREVIDEO_QA_UVC_FAULT_AFTER_FRAMES=<30..36000>`, `COREVIDEO_QA_UVC_FAULT_MS=<1..10000>` for `release-hang`. Off unless every value is valid. One `nativeLogf` line on arm and on each use. `read-no-reply` = `MfUvcCaptureBackend::requestRead` returns `S_OK` without calling `ReadSample` after N frames)
- Create: `scripts/qa/uvc-lane-isolation-drill.py` (owned headless core; samples `ping` RTT and `/snapshot` at 10 Hz; connects, disconnects and reconnects via the control API; parses `[uvc-capture] release` lines; writes `report.json` with an explicit PASS/FAIL per acceptance clause and the raw samples)
- Create: `scripts/qa/tests/test_uvc_lane_isolation_drill.py` (judge unit tests: a missing ping sample, a single 251 ms ping, a missing release line, and a frozen other-camera counter each FAIL)
- Evidence: `artifacts/uvc-lane-isolation-993/`

**Live-knob rule: never while the owner's app runs; owner-scheduled.** The drill runs an owned headless core in a window the owner has scheduled, never against the owner's installed app, a running show, or an app that has the virtual camera on. D11 allows the env knob (documented in `docs/reference/fault-injection-seams.md`, never set by the installer); set it process-locally from the drill harness only, never as a persistent user or machine environment variable.

**If the owner rules against the env knob (Review Focus 6),** replace the knob with a test-only core build that links `createUvcCaptureDeviceForTest` around the real MF backend (fault wrapper delegating to `MfUvcCaptureBackend`), and keep the drill script unchanged.

- [ ] **Step 1: Judge tests first** (`python -I -m pytest scripts/qa/tests/test_uvc_lane_isolation_drill.py`).
- [ ] **Step 2: Implement the knob and the drill. Build Release.**
- [ ] **Step 3: Injected drill (no owner hands needed; ask before starting if the owner is in a call):** for each of the webcam and the HD60 S+, run `read-no-reply`, then disconnect. Then run `release-hang` 6000 ms, then disconnect and immediately reconnect. Pass criteria: spec acceptance item 6.
- [ ] **Step 4: Real unplug (owner):** ask the owner to unplug and replug each device on cue while the drill samples. Pass criteria: spec acceptance item 7. Record any `0x8007001F` on reconnect as a #811 comment with the release-line evidence. It does not fail #993.
- [ ] **Step 5: Report** with the exact build SHA and size, hardware, durations, every FAIL, and the "Not established" list from the spec. Do not call it stable beyond the observed window.
- [ ] **Step 6: Commit** `QA: UVC lane-isolation drill with injected no-reply and release hangs (#993)`

---

### Task 10: Documentation

**Files:**
- Create: `docs/reference/capture-session-lifecycle.md` (the lane rule, reaper bound and abandon contract, reopen fence, liveness fields, release log line, drill command, and what was and was not proven)
- Modify: `CLAUDE.md` (one row in the reference-topics table; do not grow the file otherwise)
- Modify: `docs/reference/fault-injection-seams.md` (the new structural seam and, if approved, the live knob)
- Modify: `docs/reference/gotchas.md` (one entry: "MF teardown can hang in the driver; never join a capture session on the lane; a fully released reader still may not be reopenable (#811)")
- Modify: the spec's Status line and an "Outcome" section with measured numbers from Tasks 1 and 9

- [ ] **Step 1: Write the docs.** Each claim must point at a test name or an artifact path.
- [ ] **Step 2: Commit** `Docs: capture session lifecycle and lane isolation (#993)`

---

## Self-review

- Spec coverage: blocking chain (Task 1 evidence, Tasks 2 and 5), mechanism C (Tasks 3, 4, 5), fence (Tasks 5, 7), liveness for #758 (Task 6), acceptance 1-7 (Tasks 8, 5, 6, 9), risks (Review Focus, Task 5 destructor test, Task 9 knob ruling).
- No task fixes #811 or adds a reconnect delay. Task 9 only reports.
- Every live step has an owner gate or "ask first", and none uses input injection.
- The only `join()` left in the UVC path is on the reaper thread, after `exited()`.
