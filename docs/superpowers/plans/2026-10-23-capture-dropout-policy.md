# Capture Dropout Policy (#562 + #758) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A native or bridged capture device that dies leaves Program within 5.0 s of its last sample, through the operator's per-source hold-or-black choice and then the named failed slate. A static but live picture is never treated as dropout, and the Sources page reports exactly what the core did.

**Architecture:** Each capture adapter stamps a small `CaptureLiveness` record on every frame it emits. The record holds a state, whether cadence is trustworthy, the time of the last new sample, the loss time and a reason, all in core steady-clock time. Native UVC derives it from `ReadSample` results and flags plus a CM device-interface removal notification. The bridge derives it from a signal word that the shell writes into the reserved SHM header field, plus the sequence advance that the core observes. One pure function in `core/CaptureLivenessPolicy.h` turns the record into an on-air health (`producing` / `stalled` / `failed`) inside `annotateLayerSource`. The existing slate rules, plus one new shared rule (`failed` never draws a frame), do the rest. Sources that report no liveness behave exactly as today.

**Tech Stack:** C++20 core (`native/`), Media Foundation + CfgMgr32 on Windows, the in-house gtest shim (`native/tests/gtest/gtest.h`), .NET 9 WinUI shell (`native-shell/`) with xUnit test projects.

**Spec:** `docs/superpowers/specs/2026-10-23-capture-dropout-policy-design.md`
**Depends on:** #993 reader lane (`docs/superpowers/specs/2026-10-23-capture-readsample-lane-isolation-design.md`) for acceptance A1 only. Every other task can land without it.

## Global Constraints

- **Liveness is reported, never inferred.** No core code may derive capture liveness from `frameId` equality, bus `lastNewFrameNs`, pixels or `timestampMs`. Only the adapter's `CaptureLiveness` record counts. `Unreported` must reproduce today's behaviour bit for bit.
- **Constants, exact:** `kCaptureStallNs = 1'500'000'000` (equal to `kOnAirStallNs`), `kCaptureLostHoldNs = 2'000'000'000`, `kCaptureSilentFailNs = 5'000'000'000`. In the shell, `BridgeLostReleaseDelay = 6 s`, which must be greater than the silent-fail constant. A test pins that.
- **SHM header word 12:** `0` legacy, `1` live, `2` lost, `3` stopped. Single aligned 32-bit store and load, outside the seqlock.
- **Clock:** core `std::chrono::steady_clock` ns only. The shell's `TickCount64` never crosses the process boundary.
- **Policy is honoured exactly:** stalled + hold → held frame; stalled + black → black, on Program only (R3); failed → named slate on every surface, under both policies.
- **Do not change** Zoom on-air logic, `SourceBus::kStaleAfterNs`, R1, R3 or R4, or the slice-2 removal rule for `Unreported` sources.
- **CM notification callback** sets an atomic and returns. No adapter mutex, no MF call, no `nativeLogf` while holding a lock.
- **Any new `ICaptureDevice` behaviour goes on the frame,** not a new virtual (`WinUiCaptureDeviceAdapter.h:73-77` records two forwarding bugs).
- **Windows build required** for Tasks 1, 4, 5 and 9 (MF, CfgMgr32, real SHM). Build Release: `cmake --build native\build-dev --config Release --target corevideo-native-tests corevideo-native`. Debug and Release share the exe path. Check the size (Release ~2.3 MB, Debug ~8 MB) before trusting a result. Also run the stub gate `scripts/test-native.ps1`.
- **The gtest shim's `--gtest_filter`** takes ONE positive wildcard. No `:` lists, no `-` negation. Run suites separately.
- **Never run the native suite while the owner's installed app has the virtual camera on** (native tests touch the production vcam slot unless `COREVIDEO_VCAM_SHM_DIR` redirects it). Never use input injection. Drive the app through the control API.
- **Shell UI changes follow** `docs/reference/winui-crash-class-0xc000027b.md`: no new bound collections on recycled containers, and the flyout captures identity at open.
- **Honest reporting:** the shell displays `sources[]` fields verbatim. It computes no ages or states.
- Do not commit until the owner asks. Commit messages end with the harness-supplied attribution line.

## Review Focus

- **The pure decision (Task 2).** Every row of the spec's state table, both policies, `cadenceTrusted` false, and the recovery rule `lastSampleNs > lostAtNs`. Check that the static-picture case is impossible to stall while samples advance.
- **Thread safety (Tasks 4, 5).** The CM callback, the MF read loop and the render tick, and the preparation owner versus `latest()`. No new lock on the render path, and no lock held across MF or CM calls.
- **Fail-safe direction.** Every early release, unregister or respawn must move Program toward the slate, never toward a frozen frame.
- **`Unreported` parity.** `ANonZoomKindSourceNeverReadsStalledOnAirFromCadenceAlone` and every existing `SourceBus*`, `CaptureIngest*` and `ShmCapturePreparation*` test stays green, unchanged.
- **Compositor parity (Task 3).** `suppressFrameFor` sits at the same site as `blackOnStalled` in D3D11, the CPU preview and Metal.
- **The shell lifecycle (Task 6).** Every bridge stop path goes through `StopBridgeCaptureAsync`. A late `FrameArrived` cannot write after a reconnect.

---

### Task 1: Spike — observe the loss signals on the dev box (gates constants and `cadenceTrusted`)

**Recommended executor:** Opus 5.5, high effort (live hardware evidence; may change the design).

**Files:**
- Create (scratch, not committed): a throwaway logging build flag `COREVIDEO_CAPTURE_LIVENESS_SPIKE=1` read in `UvcCaptureDeviceAdapter.cpp` and `CaptureDeviceFrameReaderService.cs`. Revert at the end of the task.
- Output: append a "Task 1 evidence" section to the spec (this is the only spec edit the plan makes before Task 10).

**Interfaces:** none produced. The task decides three things: whether `UvcAsyncRead::OnEvent` receives `MEVideoCaptureDeviceRemoved` / `MEError`, whether every camera-class device on the rig keeps delivering samples on a static picture, and the measured signal-to-observation latency per adapter.

- [ ] **Step 1: Inventory.** `pnputil /enum-devices /class Camera` and `/class Image`. Record each instance id, friendly name and the MF symbolic link from `GET /snapshot` `captureDevices[].nativeDeviceId`. Note whether `capture:decklink-1` is real hardware or a dev pattern.
- [ ] **Step 2: Native path.** Spike build. For each camera, connect it natively, then `pnputil /disable-device`. Log with steady timestamps: the last sample, the `ReadSample` HRESULT and flags, any `OnEvent` event type, and the CM removal callback (registered in the spike only). Re-enable the device. Run 3 times per device.
- [ ] **Step 3: Bridge path.** `COREVIDEO_NATIVE_UVC=0`. Log `MediaCapture.Failed` (code, message), `DeviceWatcher.Removed` and the last `FrameArrived`, relative to the disable.
- [ ] **Step 4: Static picture cadence.** Run 2 minutes per device with a static scene (lens covered, and a fixed HDMI test card on the capture card). Record the maximum inter-sample gap. Any gap of 1.0 s or more marks the device `cadenceTrusted = false` (record its name pattern).
- [ ] **Step 5: Decide and record.** Decide whether `OnEvent` is used. Keep the constants, or raise a finding to the owner if a measured explicit-loss latency exceeds 1.5 s. Revert the spike flag.

### Task 2: The pure decision function

**Recommended executor:** Opus 5.5, high effort (the core of the design; small and exact).

**Files:**
- Modify: `native/src/modules/Interfaces.h` (add `CaptureLivenessState`, `CaptureLossReason`, `CaptureLiveness`, and `VideoFrame::captureLiveness`)
- Create: `native/src/core/CaptureLivenessPolicy.h`
- Create: `native/tests/CaptureLivenessPolicyTest.cpp`
- Modify: `native/CMakeLists.txt` (register the test with both the stub and the dev test targets)

**Interfaces:**
- Produces:
  ```cpp
  namespace corevideo::core {
  inline constexpr int64_t kCaptureStallNs = kOnAirStallNs;
  inline constexpr int64_t kCaptureLostHoldNs = 2'000'000'000;
  inline constexpr int64_t kCaptureSilentFailNs = 5'000'000'000;
  enum class CaptureOnAirPhase : uint8_t { Unreported, Live, Stalled, LostHolding, Failed };
  struct CaptureOnAirDecision {
    CaptureOnAirPhase phase; SourceHealth health; int64_t failInNs; bool policyApplies;
  };
  CaptureOnAirDecision decideCaptureOnAir(const modules::CaptureLiveness&, int64_t nowNs);
  const char* captureLivenessName(CaptureOnAirPhase);   // "unreported"|"live"|"stalled"|"lost"
  const char* captureLossReasonName(modules::CaptureLossReason);
  }
  ```
- Consumes: `SourceHealth`, `kOnAirStallNs` (`SourceBus.h:14-24`, `:221`).

- [ ] **Step 1: Write the failing tests.**

```cpp
#include <gtest/gtest.h>
#include "core/CaptureLivenessPolicy.h"
using namespace corevideo::core;
using corevideo::modules::CaptureLiveness;
using corevideo::modules::CaptureLivenessState;
using corevideo::modules::CaptureLossReason;
namespace {
constexpr int64_t kMs = 1'000'000;
CaptureLiveness live(int64_t lastSampleNs, bool trusted = true) {
  CaptureLiveness l; l.state = CaptureLivenessState::Live; l.cadenceTrusted = trusted; l.lastSampleNs = lastSampleNs; return l;
}
CaptureLiveness lost(int64_t lastSampleNs, int64_t lostAtNs, bool trusted = true) {
  auto l = live(lastSampleNs, trusted); l.state = CaptureLivenessState::Lost; l.lostAtNs = lostAtNs;
  l.reason = CaptureLossReason::DeviceRemoved; return l;
}
}  // namespace

TEST(CaptureLivenessPolicy, UnreportedIsTodaysBehaviourAndNeverApplysPolicy) {
  const auto d = decideCaptureOnAir(CaptureLiveness{}, 100'000 * kMs);
  EXPECT_EQ(d.phase, CaptureOnAirPhase::Unreported);
  EXPECT_EQ(d.health, SourceHealth::Producing);
  EXPECT_FALSE(d.policyApplies);
}
TEST(CaptureLivenessPolicy, AdvancingSamplesStayLiveWhateverThePictureIs) {
  // A static slide still delivers samples: only sample time matters.
  for (int64_t t = 0; t < 600'000; t += 33) {
    const auto d = decideCaptureOnAir(live(t * kMs), t * kMs + 33 * kMs);
    ASSERT_EQ(d.phase, CaptureOnAirPhase::Live);
  }
}
TEST(CaptureLivenessPolicy, SilenceStallsAtExactlyTheStallBoundAndFailsAtTheSilentBound) {
  EXPECT_EQ(decideCaptureOnAir(live(0), kCaptureStallNs - 1).phase, CaptureOnAirPhase::Live);
  EXPECT_EQ(decideCaptureOnAir(live(0), kCaptureStallNs).phase, CaptureOnAirPhase::Stalled);
  EXPECT_EQ(decideCaptureOnAir(live(0), kCaptureStallNs).health, SourceHealth::Stalled);
  EXPECT_EQ(decideCaptureOnAir(live(0), kCaptureSilentFailNs - 1).phase, CaptureOnAirPhase::Stalled);
  EXPECT_EQ(decideCaptureOnAir(live(0), kCaptureSilentFailNs).phase, CaptureOnAirPhase::Failed);
  EXPECT_EQ(decideCaptureOnAir(live(0), kCaptureSilentFailNs).health, SourceHealth::Failed);
}
TEST(CaptureLivenessPolicy, UntrustedCadenceNeverStallsOrFailsOnSilence) {
  const auto d = decideCaptureOnAir(live(0, /*trusted=*/false), 3'600'000 * kMs);
  EXPECT_EQ(d.phase, CaptureOnAirPhase::Live);
  EXPECT_TRUE(d.policyApplies);
}
TEST(CaptureLivenessPolicy, ExplicitLossStallsImmediatelyThenFailsAfterTheHoldWindow) {
  const int64_t lostAt = 100 * kMs;
  EXPECT_EQ(decideCaptureOnAir(lost(90 * kMs, lostAt), lostAt).phase, CaptureOnAirPhase::LostHolding);
  EXPECT_EQ(decideCaptureOnAir(lost(90 * kMs, lostAt), lostAt).health, SourceHealth::Stalled);
  EXPECT_EQ(decideCaptureOnAir(lost(90 * kMs, lostAt), lostAt + kCaptureLostHoldNs - 1).phase,
            CaptureOnAirPhase::LostHolding);
  EXPECT_EQ(decideCaptureOnAir(lost(90 * kMs, lostAt), lostAt + kCaptureLostHoldNs).phase,
            CaptureOnAirPhase::Failed);
}
TEST(CaptureLivenessPolicy, ExplicitLossAfterLongSilenceNeverExceedsTheSilentBound) {
  // Last sample at 0, loss reported at 4.5 s: fail at 5.0 s, not 6.5 s.
  const auto l = lost(0, 4'500 * kMs);
  EXPECT_EQ(decideCaptureOnAir(l, kCaptureSilentFailNs - 1).phase, CaptureOnAirPhase::LostHolding);
  EXPECT_EQ(decideCaptureOnAir(l, kCaptureSilentFailNs).phase, CaptureOnAirPhase::Failed);
}
TEST(CaptureLivenessPolicy, ExplicitLossFailsEvenWithUntrustedCadence) {
  EXPECT_EQ(decideCaptureOnAir(lost(0, 0, /*trusted=*/false), kCaptureLostHoldNs).phase, CaptureOnAirPhase::Failed);
}
TEST(CaptureLivenessPolicy, FailInNsCountsDownToTheDeadlineWhileHolding) {
  const auto d = decideCaptureOnAir(lost(0, 0), 500 * kMs);
  EXPECT_EQ(d.failInNs, kCaptureLostHoldNs - 500 * kMs);
  EXPECT_EQ(decideCaptureOnAir(live(0), 0).failInNs, 0);
}
TEST(CaptureLivenessPolicy, RecoveryNeedsALiveSampleNewerThanTheLoss) {
  auto l = lost(0, 100 * kMs);
  l.state = CaptureLivenessState::Live;          // adapter says live again...
  l.lastSampleNs = 50 * kMs;                     // ...but on a sample older than the loss
  EXPECT_NE(decideCaptureOnAir(l, 200 * kMs).phase, CaptureOnAirPhase::Live);
  l.lastSampleNs = 150 * kMs;                    // a genuinely new sample
  EXPECT_EQ(decideCaptureOnAir(l, 200 * kMs).phase, CaptureOnAirPhase::Live);
}
```

- [ ] **Step 2: Run, and watch them fail to compile.** Stub gate, then the dev suite filter `CaptureLivenessPolicy.*`.
- [ ] **Step 3: Implement** the header (integer-only, no allocation), and the `Interfaces.h` types with the defaults from the spec §1. `Live` with `lostAtNs != 0 && lastSampleNs <= lostAtNs` is treated as `Lost`.
- [ ] **Step 4: Run** both gates, green. Confirm `VideoFrame` copies still compile everywhere (the whole dev build).

### Task 3: Bus, annotation, wire and compositor rule

**Recommended executor:** Opus 5.5, high effort (touches the on-air path and three compositors).

**Files:**
- Modify: `native/src/core/CaptureDeviceSource.h` (store `latest_.captureLiveness`; `lastDecision()` / `recordDecision()`; tombstone support)
- Modify: `native/src/core/CaptureBusRoster.h` (`endHeldCaptureFrame` keeps a video-less tombstone when the last liveness was reported; cap 32, oldest evicted; replaced by the next frame)
- Modify: `native/src/core/SourceBus.h` (`OnAirStatus` gains `const modules::CaptureLiveness* captureLiveness` and `bool tombstone`; a `capture`-kind lookup via `dynamic_cast` only inside `onAirStatusFor`)
- Modify: `native/src/core/MediaCore.cpp` (`annotateLayerSource` `:1827-1860`, `setSourcePolicy` `:1784-1825`; clear the tombstone on `disconnect-capture-device`)
- Modify: `native/src/core/SourceVideoIngress.cpp` (`sourceHealthState` `:96-117`: the new fields from the recorded decision)
- Modify: `native/src/compositor/CompositorLayout.h` (`suppressFrameFor`), `native/src/modules/D3D11CompositorAdapter.cpp` (`:1418-1428`), `native/src/modules/ProgramFramePreview.cpp` (`:415-425`), `native/src/compositor/MetalCompositorAdapter.mm` (same site)
- Test: `native/tests/SourceBusTest.cpp`, `native/tests/MediaCoreCommandTest.cpp` (update `:8310-8365`, extend `:8562`), `native/tests/ProgramFramePreviewHealthTest.cpp`

**Interfaces:**
- Consumes: `decideCaptureOnAir` (Task 2).
- Produces: `sources[]` fields `liveness`, `onAir`, `lastSampleAgeMs`, `lossReason`, `failInMs`, `dropoutPolicyApplies`; `set-source-policy` accepting `capture:<id>`; `compositor::suppressFrameFor(std::string_view health)`.

- [ ] **Step 1: Write the failing tests.**
  - `SourceBus.CaptureLivenessTravelsFromFrameToOnAirStatus`
  - `SourceBus.ReportedCaptureSourceLeavesATombstoneOnEndAndAFrameReplacesIt`
  - `SourceBus.UnreportedCaptureSourceIsStillRemovedOnAbsence` (slice-2 parity)
  - `MediaCoreCommand.CaptureDropoutPolicyIsAcceptedAndAppliedOnlyWhenLivenessIsReported` (replaces the refusal assertion at `:8317`; `media:` is still refused)
  - `MediaCoreCommand.LostCaptureWithHoldShowsHeldFrameThenFailedSlate`
  - `MediaCoreCommand.LostCaptureWithBlackIsBlackOnProgramHeldOnPreviewThenSlateEverywhere`
  - `MediaCoreCommand.StaticLiveCaptureNeverStallsUnderBlackPolicy`
  - `MediaCoreCommand.TombstonedCaptureReadsFailedEvenWithNoOtherFrames`
  - `MediaCoreCommand.SourcesNodeReportsTheAppliedDecisionNotARecomputation`
  - `ProgramFramePreviewHealth.FailedNeverDrawsAMatchedFrame`

  Drive time with an injected `nowNs`, as the existing slice-4a tests do. Feed frames through `addSourceForTest` and the capture roster helpers, never through a real device.
- [ ] **Step 2: Run, and watch them fail.**
- [ ] **Step 3: Implement.**
  - In `annotateLayerSource`, for `kind == "capture"` with reported liveness, health comes from `decideCaptureOnAir`, and the decision is recorded on the source under `coreMutex`. All other kinds take today's branch, untouched.
  - `setSourcePolicy` accepts `capture:` verbatim, and updates the warning text for other prefixes.
  - `suppressFrameFor` runs before `blackOnStalled` in all three compositors.
- [ ] **Step 4: Run** the dev suite (all of `SourceBus*`, `MediaCoreCommand*`, `ProgramFramePreviewHealth*`, `CaptureIngest*`, one filter per run) and the stub gate. Run `node scripts/validate-multiview.mjs` and `node scripts/validate-tiles.mjs`.

### Task 4: Native UVC liveness

**Recommended executor:** Opus 5.5, high effort (MF + CM threading; Windows build).

**Files:**
- Modify: `native/src/modules/UvcCaptureDeviceAdapter.cpp`
  - session: `lastSampleNs_` and `lostAtNs_` atomics, `lossReason_`; set in the publish path (`:534-544`) and in `fail()` (`:197-204`), with the reason mapped per the spec §2 table
  - adapter: one `CM_Register_Notification` for the adapter lifetime; the callback matches the symbolic link (folded) and stamps the session's loss atomically
  - `captureVideoTick` (`:714-753`): stamp `frame.captureLiveness` and keep re-emitting after a loss; `connectionState` / `signalPresent` / `warning` derived from the same record
  - discovery skip (`:841-846`) gated on `COREVIDEO_WITH_DECKLINK`
- Modify: `native/src/modules/UvcAsyncRead.h` (`OnEvent` forwards the device-removed / error event to a callback, only if Task 1 found it fires)
- Modify: `native/src/modules/UvcCaptureSupport.h` (a pure `uvcLossReasonFor(HRESULT, DWORD flags)` helper)
- Modify: `native/CMakeLists.txt` (link `cfgmgr32` for the UVC target)
- Test: `native/tests/UvcCaptureSupportTest.cpp`, `native/tests/UvcAsyncReadTest.cpp`, `native/tests/CaptureSessionLifecycleTest.cpp`

**Interfaces:**
- Consumes: the #993 lane's last-sample time, terminal error and error class, if merged. Otherwise today's session fields.
- Produces: frames keyed `capture:<id>` carrying `CaptureLiveness{Live|Lost, cadenceTrusted = true (unless Task 1 listed the device), ...}`.

- [ ] **Step 1: Write the failing tests.**
  - `UvcCaptureSupport.LossReasonMapsInvalidatedErrorFlagAndEndOfStream`: pure, every row of the spec table.
  - `UvcCaptureSupport.RemovalNotificationMatchesSymbolicLinkCaseInsensitively`: pure matcher.
  - `CaptureSessionLifecycle.FailedSessionKeepsHeldFrameAndReportsLostWithTheFirstLossTime`: with the existing fake reader seam; a second loss signal never moves `lostAtNs`.
  - `CaptureSessionLifecycle.NewSampleAdvancesLastSampleButReEmitDoesNot`.
  - `CaptureSessionLifecycle.RemovalCallbackNeverTakesTheAdapterMutex`: the callback runs while a test holds the adapter mutex and must return within 10 ms.
- [ ] **Step 2: Run** on the Windows dev build, and watch them fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the dev suite `Uvc*`, `CaptureSession*` and `CaptureDeviceLifecycle*`, plus the stub gate. Then run a live check on one webcam: connect natively, read `GET /snapshot` `sources[]` (liveness `live`), disable the device, and watch it go to `lost` then `failed`. Re-enable it.

### Task 5: Bridge liveness in the core

**Recommended executor:** Opus 5.5, high effort (cross-process memory; preparation-owner concurrency).

**Files:**
- Modify: `native/src/modules/ShmCapturePreparation.h/.cpp`
  - `Mapping` gains `lastSampleNs` and `lostAtNs`, and `signal` (read volatile from `view + 12` each loop)
  - an immutable `liveness_` snapshot republished on change
  - `latest()` (`:107-120`) stamps each returned frame, including `held`
- Modify: `native/src/modules/WinUiCaptureDeviceAdapter.cpp` (no logic change expected; confirm the stamp survives `:43-48`)
- Test: `native/tests/ShmCapturePreparationTest.cpp`

**Interfaces:**
- Consumes: the header word written by Task 6.
- Produces: bridge frames with `CaptureLiveness`:
  - word 0 → `Unreported`
  - word 1 → `Live, cadenceTrusted`
  - word 2 → `Lost / ShellReaderLost`
  - word 3 → `Lost / ShellStopped`

- [ ] **Step 1: Write the failing tests** with a real named mapping, as the existing tests do:
  - `ShmCapturePreparation.LegacyZeroSignalWordStaysUnreported`
  - `ShmCapturePreparation.LiveWordWithAdvancingSequenceIsLiveAndHeldReEmitDoesNotAdvanceLastSample`
  - `ShmCapturePreparation.LostWordStampsLossOnceAtFirstObservationAndKeepsTheHeldFrame`
  - `ShmCapturePreparation.StoppedWordReportsShellStopped`
  - `ShmCapturePreparation.ReRegisterAfterLossStartsLiveOnlyOnANewSequence`
  - `ShmCapturePreparation.SignalWordIsReadOutsideTheSeqlockWithoutTearingTheFrame`
- [ ] **Step 2: Run, and watch them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ShmCapturePreparation*` and `ShmGpuPreparation*` (separately) and the stub gate.

### Task 6: Shell bridge lifecycle (#758)

**Recommended executor:** Opus 5.5, high effort (async lifecycle across several shell services; the frozen-frame root cause).

**Files:**
- Modify: `native-shell/CoreVideoPro.WinUI/Services/CaptureDeviceSharedMemoryWriter.cs`
  - header doc: reserved → signal
  - `Write` stores `1`, and takes a session token
  - `MarkSignal(deviceId, CaptureBridgeSignal)`
  - `BeginSession(deviceId)` returns the current token
- Modify: `native-shell/CoreVideoPro.WinUI/Services/CaptureDeviceFrameReaderService.cs`
  - subscribe `MediaCapture.Failed`
  - a `Lost` event `(deviceId, reason)`
  - give-up (`:979-990`) raises `Lost` instead of "leave the last frame frozen"
  - session token passed with each published frame
  - `CaptureDeviceExclusiveControlStatusChanged` logged only
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs`
  - one `StopBridgeCaptureAsync(deviceId, BridgeStopReason)` used by native takeover (`:6790-6796`), loss, device removal, operator disconnect and shutdown
  - `ApplyDiscoveredCaptureDevices` (`:7238-7290`) diffs vanished bridged ids and stops them with reason `Removed`
  - lost → write `2`, release after `BridgeLostReleaseDelay`
  - deliberate stop → write `3`, release now
  - `ReleaseCaptureBridgeBuffer` (`:10015`) becomes the release half of that path
- Create: `native-shell/CoreVideoPro.WinUI/Services/BridgeCaptureStopPolicy.cs` (pure: reason → signal word, delay)
- Test: `native-shell/CoreVideoPro.WinUI.Tests/CaptureDeviceSharedMemoryWriterTests.cs`, `CaptureReaderStallPolicyTests.cs`, and a new `BridgeCaptureStopPolicyTests.cs`

**Interfaces:**
- Consumes: `IMediaCoreBridge.UnregisterCaptureShmAsync` (exists).
- Produces: the SHM signal word contract for Task 5; `BridgeLostReleaseDelay`; `CoreCaptureLiveness.SilentFailMs = 5000` mirrored as a C# constant with a comment pointing at `CaptureLivenessPolicy.h`.

- [ ] **Step 1: Write the failing tests.**
  - `Write_StoresLiveSignalWordAtOffset12`
  - `MarkSignal_WritesOnlyTheSignalWordAndLeavesSequenceAndPixels`
  - `Write_RefusesAStaleSessionToken`
  - `StopPolicy_LossWritesLostAndDelaysRelease`
  - `StopPolicy_DeliberateStopWritesStoppedAndReleasesNow`
  - `StopPolicy_ReleaseDelayExceedsCoreSilentFailBound`
  - `StallPolicy_GiveUpRaisesLostNotFrozen`

  The view-model diff of vanished ids gets a pure helper, `VanishedBridgedDeviceIds(prior, discovered, liveSessions)`, with its own tests. The view model is not constructible in tests (slice 4a R5).
- [ ] **Step 2: Run** `dotnet test native-shell/CoreVideoPro.WinUI.Tests`, and watch them fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the WinUI tests and `dotnet build -c Release -p:Platform=x64`. Then run a live check with `COREVIDEO_NATIVE_UVC=0`: disable the bridged camera, and in `launch.log` see `signal=2` followed by `unregister-capture-shm` 6 s later. `sources[]` goes `lost` → `failed`.

### Task 7: Sources page — lift the guard, one control, honest status

**Recommended executor:** Sonnet 5.5, high effort (WinUI crash class; several small sites).

**Files:**
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs`
  - `SetSourceDropoutPolicy` (`:4808-4820`) accepts `capture:`
  - the prune (`:12163-12177`) keeps `capture:`
- Modify: `native-shell/CoreVideoPro.WinUI/Services/StudioControlSurface.cs` (`:317-330`: accept `zoom:` and `capture:`; rename the validator)
- Modify: `native-shell/CoreVideoPro.WinUI/Models/ProductionModels.cs` (`PopulateCaptureDeviceDropoutPolicy` `:1078-1080` resolves the stored value; `BuildSourcePolicyWires` includes capture policies)
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/ShowInputSlotViewModel.cs` (`ShowSourcePolicies` `:378` true for a capture slot; a canonical `PolicySourceId`)
- Modify: `native-shell/CoreVideoPro.WinUI/Views/SourcePolicyFlyout.cs` and `Controls/SourcesWorkspaceControl.Inspector.cs` (`:127-135`)
  - key by `PolicySourceId`
  - the role combo only for Zoom
  - the policy combo enabled iff `dropoutPolicyApplies`
  - capture labels and the disabled note, per the spec §5
- Modify: the `sources[]` model in `native-shell/CoreVideoPro.MediaCore` (parse the six new fields) and the Sources capture row status line (`Views/SourcesInputsPage.xaml` around `:115`; remove the stale R2 comment)
- Test: `native-shell/CoreVideoPro.WinUI.Tests/StudioControlSurfaceCoverageTests.cs` (`:43` refusal set changes), new `CaptureDropoutStatusTextTests.cs` (pure formatter from the snapshot fields), and the existing `BuildSourcePolicyWires` tests

**Interfaces:**
- Consumes: the Task 3 `sources[]` fields.
- Produces: the operator surface.

- [ ] **Step 1: Write the failing tests.**
  - The control surface accepts `capture:`, and refuses `media:` and empty.
  - The prune keeps `capture:`.
  - The wires include a capture black policy.
  - The status formatter: one case per state, plus unreported → no line.
  - The flyout model: the combo is disabled with the note when `dropoutPolicyApplies` is false.
- [ ] **Step 2: Run, and watch them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the WinUI, MediaCore and Control tests, and the Release build. Then launch (`npm run app -- -StubOnly`), open Sources, and screenshot the inspector flyout for a native camera, a bridged camera and a WGC screen. Run an axe or contrast check on the flyout text, per memory "run a11y scans on UI".

### Task 8: Vendor-named devices on the native path

**Recommended executor:** Sonnet 5.5, medium effort.

**Files:**
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/NativeUvcCapturePolicy.cs` (`:44-53`: `ShouldPreferNativeCapture` takes the core's native list; prefers native for `blackmagic`/`aja` when the device appears there as vendor `uvc`)
- Modify: the caller at `StudioViewModel.cs:6688`
- (The core half shipped in Task 4: the skip is gated on `COREVIDEO_WITH_DECKLINK`.)
- Test: `native-shell/CoreVideoPro.MediaCore.Tests/NativeUvcCapturePolicyTests.cs` (create if absent)

- [ ] **Step 1: Write the failing tests.**
  - A blackmagic-named device listed by the core as `uvc` → native.
  - The same device not listed → bridge.
  - SRT → never native.
  - Opt-out env `0` → bridge.
- [ ] **Step 2: Run, and watch them fail. Step 3: Implement. Step 4: Run** the tests and the build.

### Task 9: Acceptance on the dev box

**Recommended executor:** Opus 5.5, high effort (live evidence; the owner performs the physical replug).

**Files:**
- Create: `scripts/qa/capture-dropout-acceptance.py`
  - drives the control API
  - toggles `pnputil /disable-device` / `/enable-device`
  - records Program for each run, and samples luma plus the slate signature per decoded frame (reuse `zoom-gap-hold-ab.py`'s sampler)
  - polls `sources[]` at 50 ms
  - writes a JSON verdict per spec acceptance row
- Output: evidence under `artifacts/capture-dropout-562-758/`

- [ ] **Step 1: Preconditions.** Confirm a Release core (size check) and record the build sha. The owner's installed app must not have the virtual camera on. Check whether #993 has merged: if not, mark A1 and A2 as blocked and continue.
- [ ] **Step 2: Run A1-A4 three times each, per policy.** Assert the bounds. Record the measured explicit-loss latency (signal → core observation → first non-frozen Program frame).
- [ ] **Step 3: Run A5** (10 minutes per device per policy) **and A6.**
- [ ] **Step 4: Run A7 and A8.** For A7, the owner physically unplugs and replugs once per adapter.
- [ ] **Step 5: Run A9.** Screenshots of each state on the Sources page, next to the matching `sources[]` JSON.
- [ ] **Step 6: Run A10,** or record `MISSING_EVIDENCE`.
- [ ] **Step 7: Post the evidence** on #758 and #562. Close each only if every one of its rows passes. A blocked A1 keeps #758 open with that note.

### Task 10: Documentation

**Recommended executor:** Sonnet 5.5, low effort.

**Files:**
- Modify: `docs/reference/source-bus.md` (a "Capture liveness (#562/#758)" section: the record, the state machine, the tombstone, and that R2 is lifted for capture only)
- Modify: `docs/reference/gotchas.md` (SHM signal word; never infer capture liveness from cadence; hold is bounded for capture)
- Modify: `docs/qa/installed-acceptance-checklist.md` (row #758 → unplug test; a new #562 static-picture row)
- Modify: `docs/reference/operator-reporting-behavior-plan.md` (mark the #758/#562 row's dependency settled)
- Modify: the spec (an Outcome section with the measured numbers)

- [ ] **Step 1: Write. Step 2:** grep for any remaining "Zoom-only this slice" comments in the code (`MediaCore.cpp`, `StudioViewModel.cs`, `SourcesInputsPage.xaml`, `StudioControlSurface.cs`) and update them to describe the new rule.

## Effort and executors

**Recommended executor:** model + effort per task.

| Task | Executor | Effort | AI-assisted hours |
|---|---|---|---|
| 1 Spike: loss signals and static cadence on the rig | Opus 5.5 | high | 1.5 |
| 2 Pure decision function | Opus 5.5 | high | 1.0 |
| 3 Bus, annotation, wire, compositor rule | Opus 5.5 | high | 2.0 |
| 4 Native UVC liveness (Windows build) | Opus 5.5 | high | 2.0 |
| 5 Bridge liveness in the core (Windows build) | Opus 5.5 | high | 1.5 |
| 6 Shell bridge lifecycle (#758) | Opus 5.5 | high | 2.5 |
| 7 Sources page: guard lift, one control, status | Sonnet 5.5 | high | 2.0 |
| 8 Vendor-named devices native | Sonnet 5.5 | medium | 0.75 |
| 9 Acceptance on the dev box (owner replugs) | Opus 5.5 | high | 2.0 |
| 10 Documentation | Sonnet 5.5 | low | 0.5 |
| **Total** | | | **15.75** |

Critical path: 1 → 2 → 3 → (4 ∥ 5 → 6) → 7 → 9. Task 8 can run any time after Task 4. Task 1 can run in parallel with Task 2 because the constants are already fixed; Task 1 can only tighten `cadenceTrusted` or raise a finding. A fresh-context review (Opus 5.5, high, about 1.0 h, not counted above) is recommended on Tasks 3 and 6 before merge.

## Self-review

- Spec §1 (record) → Task 2. §2 native → Task 4; bridge → Tasks 5-6. §3 (state machine, bounds, `suppressFrameFor`, tombstone) → Tasks 2-3. §4 (wire and snapshot) → Task 3. §5 (Sources page) → Task 7. §7 (vendor names) → Tasks 4 and 8. Acceptance → Task 9. Documentation → Task 10.
- Every #758 done-when is covered. The shell unregisters on disconnect and reader stop (Task 6). The adapter's held buffer leaves Program at the policy deadline (Tasks 3-5; the core drops it by decision rather than by the adapter clearing it, which honours hold). A stale bridged frame ages out through the dropout policy (Tasks 3, 5). Blackmagic-named UVC devices go native while #537 is unbuilt (Tasks 4, 8).
- Every #562 item is covered. Liveness is surfaced per tick without `enumerate()` (Task 2's frame field). The capture-row combo is restored, as one inspector control (Task 7). The `zoom:` guard is lifted in the core, the shell and the control surface (Tasks 3, 7). Media and still exemptions stay `Unreported`, unchanged.
- Owner questions Q1-Q3 in the spec do not block Tasks 1-6. Q1 can change only one branch in Task 2 (failed + black → black) and one label in Task 7.
- No task infers liveness from cadence or pixels. `Unreported` parity is asserted by existing tests that stay unchanged.
