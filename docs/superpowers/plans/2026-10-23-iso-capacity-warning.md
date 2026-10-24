# ISO Capacity Warning (#935) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A nonfatal ISO software-encoder admission notice never stops or fails a recording. The operator sees which ISO is on the software encoder and its measured per-frame cost. The headless validator and the outputs drill pass with the note, and fail only on measured loss.

**Architecture:** The core already decides placement and admission once at arm (`IsoEncoderPlacement.h`, `IsoEncoderAdmission.h`). This plan publishes that verdict as a structured snapshot node (`recording.isoAdmission`) plus a per-stream `isoIndex`, so no reader parses `recording.warning`. The shell gets a pure readout (`IsoEncoderPlacementReadout`) feeding one InfoBar. The validator and the drill judge read the structured fields and move encoder placement from "gate" to "note". The shell start predicate fix (`935b2800`, already on `main`) is pinned at the coordinator level and not otherwise changed.

**Tech Stack:** C++20 core (`native/`) with the in-house gtest shim, Media Foundation encoders on Windows, .NET 9 / WinUI 3 shell with xUnit, Node `node:test` for `scripts/qa/*.mjs`, Windows PowerShell 5.1 + Python `unittest` for the outputs drill.

**Spec:** `docs/superpowers/specs/2026-10-23-iso-capacity-warning-design.md`

**Recommended executor:**

| Task | Model | Effort | Reason |
|---|---|---|---|
| 1 Baseline evidence (why the 4090 spills) | sonnet | medium | Run two commands and read three fields; no code. It needs care to quote the measured values exactly |
| 2 Native structured `isoAdmission` + `isoIndex` | opus | high | Touches the recording read model that `AsyncEncoderSink` copies across threads, plus a one-slot warning with overwrite rules. MF tests only run on a real Windows build |
| 3 Shell model + `IsoEncoderPlacementReadout` + InfoBar | sonnet | high | Pure readout with a table of cases. The honesty rules (measured or absent) live in the tests |
| 4 Coordinator start-path pins | sonnet | medium | Two xUnit tests in an existing fixture pattern (`TransportCoordinatorTests.cs:327`) |
| 5 `validate-iso-record.mjs` rule change | sonnet | high | Extract pure rules and test them. The only risk is waiving too much, and the tests carry it |
| 6 Outputs drill: demote placement, measure cost | opus | high | Changes a release-gate verdict rule. A mistake turns a real FAIL into a PASS |
| 7 Documentation | haiku | low | Mechanical edits taken from the spec |
| 8 Acceptance on the dev box | fable | high | Uses the owner's GPU and a 30-min drill, and decides what the software ISO's CPU numbers mean. A false PASS is expensive |

**Estimated AI-assisted effort:** 0.75 + 2.5 + 2.0 + 0.75 + 1.5 + 2.0 + 0.5 + 2.0 = **about 12 h**. Owner time is needed only to free the box for Task 8: no show running, and the virtual camera off.

## Global Constraints

- **Windows build required.** Tasks 1, 2 and 8 need a real `COREVIDEO_STUB=OFF` Release core on Windows. Stub or mac CI cannot compile or run the `EncoderRecordingSession.MediaFoundation*` tests (see `cvp-recording-prs-need-windows-build`). Build:
  ```powershell
  $vs = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
  cmd /c "call `"$vs`" -arch=amd64 >nul 2>&1 && cmake --build native\build-dev --config Release --target corevideo-native-tests corevideo-native corevideo-zoom-engine-fake 2>&1" | Select-String " error |-> "
  ```
  **Always `--config Release`.** Debug and Release write the same exe path. Check the size of `corevideo-native.exe` (about 2.2 MB Release vs about 8 MB Debug) before believing any number.
- New MF tests go **inside** the existing `#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_MF_ENCODER` block in `native/tests/EncoderRecordingSessionTest.cpp` (opens `:648`, closes `:2497`). Non-MF snapshot tests go after `:2497`, beside `EncoderRecordingWarningSurfacesInSnapshotRecordingSection`.
- A test that arms a real recording **must pin capacity** with `corevideo::testing::ForcedEncoderCapacity` (`tests/EncoderCapacityProbeTestSupport.h`). Otherwise it races the asynchronous probe.
- The gtest shim's `--gtest_filter` takes ONE positive wildcard. No `:` lists and no `-` negation. Run suites separately.
- **Do not change** `EncoderCapacityProbe` discovery, its cap (9), the unprobed fallback (8), `softwareSessionBudget`, `planIsoEncoders` or `evaluateIsoAdmission` decisions. This plan only publishes what they already decide. `COREVIDEO_ENCODER_PROBE_MAX_SESSIONS` is an existing QA override; using it to force a spill in a test run is allowed.
- **Honesty rule:** every value shown or printed is measured, or shown as absent ("not measured yet"). Nothing is estimated or computed from an assumption. A missing field is never shown as zero.
- **No new cancel path.** Only `lifecycle.state == "failed"`, recording `OutputHealth.status == "failed"`, `Recording.Status == "failed"`, `MediaCoreOnAirPolicy.RecordBlockReason`, and the disk pre-flight block may stop a start.
- The per-frame identity judges (`iso-pixel-verdict.py`, `recording-pixel-verdict.py`, `camera-pixel-receiver.mjs`) are **not edited**.
- `recording.warning` text is unchanged. All new JSON is additive.
- Never run the native suite or the drill while the owner's installed app is recording, streaming, or has the virtual camera on.
- Shell UI-thread rule: anything after an `await` that writes bound state resumes on the UI thread (`ConfigureAwait(true)` or `_dispatcher.RunOnUiThread`), per the 0xc000027b rule.
- Update `CLAUDE.md`/`docs/reference` in the same change (Task 7). Docs being updated is part of done.

## Review Focus

- Task 2: is `isoAdmission` reset at every new session start (the same place `session_.recordingWarning.clear()` runs, `MediaFoundationEncoderAdapter.cpp:1628`), so a later Program-only take does not show the previous take's spill?
- Task 2: does the `AsyncEncoderSink` snapshot copy (`fresh` in `AsyncEncoderSink.cpp:540-830`) carry the new fields without being rebuilt field by field? If it rebuilds, the node silently disappears in the product while the direct-sink MF test still passes. The stub-sink MediaCore test must go through the same path the product uses.
- Task 3: does any readout path show a number that was not in the snapshot (for example a frame budget from a default fps when `frameRate` is absent)?
- Task 5: can any warning other than the exact admission message pass? Prepend, append and partial-match cases must all fail.
- Task 6: does removing `hardwareEncodersThroughout` from gating change `missing_flags` (`outputs-drill-judge.py:265`, `THROUGHOUT_FLAGS[:4]`)? Does any identity FAIL still produce overall FAIL?
- Task 8: are CPU and per-frame cost reported with hardware, workload and duration, and is a software-ISO identity FAIL reported as a finding, not waived?

---

### Task 1: Baseline evidence — measure why the dev box spills (no code)

**Files:**
- Create (scratch, not committed): `D:\CoreVideo-QA\iso935\baseline-default.json`, `D:\CoreVideo-QA\iso935\baseline-allow.json`

**Interfaces:**
- Consumes: `scripts/validate-iso-record.mjs --evidence`, the session `manifest.json` (`encoderCapacity`, `isoAdmission`, `MediaFoundationEncoderAdapter.cpp:2465-2471`), `media-core.log` lines `[recording] iso-admission` and `[encoder-probe]`.
- Produces: a short written record (in the Task 8 report) of `isoAdmission` code, `probed`, probe status, and the capacity summary at arm time.

- [ ] **Step 1: Build Release (Global Constraints) and confirm the exe size.**
- [ ] **Step 2: Run without and with the flag, keeping artifacts**

```powershell
New-Item -ItemType Directory -Force D:\CoreVideo-QA\iso935 | Out-Null
node scripts/validate-iso-record.mjs --sources 8 --seconds 20 --keep-artifacts --evidence D:\CoreVideo-QA\iso935\baseline-default.json
node scripts/validate-iso-record.mjs --sources 8 --seconds 20 --keep-artifacts --allow-capacity-warning --evidence D:\CoreVideo-QA\iso935\baseline-allow.json
```

Expected: the first run FAILs only on `recording.warning surfaced: 1 ISO source will record on the CPU software encoder...`, and the second run PASSes.

- [ ] **Step 3: Read the provenance.** Open `manifest.json` in the kept session folder (under `native\build-dev\Recordings\CoreVideoPro\validate-iso-record\`). Record `isoAdmission` (`iso-encoder-software-spill` vs `iso-encoder-software-spill-unprobed`) and `encoderCapacity`. Read the full `recording.warning` from the evidence JSON: an unprobed arm contains "Encoder capacity could not be measured on this machine (probe pending)".
- [ ] **Step 4: Report, do not fix.** If the code is `-unprobed`, the arm-time lookup lost a race with the 1080p60 prewarm (`:1549`). File a GitHub issue (label `backlog`) titled "ISO arm reads unprobed capacity when Record follows configure within ~2 s". Quote the evidence and do **not** change the probe in this plan. If it is probed with a ceiling of 8, quote the `[encoder-probe]` line. Either way, continue to Task 2.
- [ ] **Step 5: Delete the kept MP4s** after copying the manifest and evidence to `D:\CoreVideo-QA\iso935\`.

---

### Task 2: Native — publish the admission verdict and `isoIndex`

**Files:**
- Modify: `native/src/modules/Interfaces.h` (`IsoStreamStatus` near `:600-615`; `OutputSession` near `:617`)
- Modify: `native/src/modules/IsoEncoderAdmission.h` (add `isoAdmissionDecisionId`)
- Modify: `native/src/modules/MediaFoundationEncoderAdapter.cpp` (`IsoWriterEntry` near `:1236-1255`; `publishIsoTrack` `:1258-1274`; session reset `:1628`; arm `:2239-2257`; entry loop `:2280-2299`)
- Modify: `native/src/modules/AsyncEncoderSink.cpp` (only if Review Focus finds a field-by-field rebuild)
- Modify: `native/src/core/CoreStateCapture.cpp` (streams `:1935-1960`; recording node near `:2055-2080`)
- Test: `native/tests/IsoEncoderAdmissionTest.cpp`, `native/tests/EncoderRecordingSessionTest.cpp`

**Interfaces:**
- Produces (C++):
  ```cpp
  // Interfaces.h
  struct IsoAdmissionStatus {
    std::string decision;          // "admit" | "admit-with-warning" | "refuse-iso"
    std::string code;              // verdict.code, e.g. "iso-encoder-software-spill"
    std::string message;           // verdict.message, verbatim (same text as recording.warning)
    int hardwareCount = 0, softwareCount = 0, unplacedCount = 0;
    int admissibleIsoCount = -1;   // -1 = unknown, published as null
    bool probed = false;
    std::string probeStatus;       // IsoCapacitySource::status
    std::string capacity;          // probe.describe()
    std::string workload;          // probeKey.describe()
  };
  // IsoStreamStatus:  int isoIndex = 0;   // 1-based, matches ISO-NN in the file name; 0 = not an ISO
  // OutputSession:    std::optional<IsoAdmissionStatus> isoAdmission;  // absent when no ISO was selected
  ```
  ```cpp
  // IsoEncoderAdmission.h
  inline const char* isoAdmissionDecisionId(IsoAdmissionDecision d);  // "admit" | "admit-with-warning" | "refuse-iso"
  ```
- Produces (snapshot JSON): `recording.isoAdmission` object as in spec section 2 (`admissibleIsoCount: null` when -1). `recording.streams[].isoIndex` is a number on ISO streams.
- Consumes: `IsoAdmissionVerdict`, `IsoCapacitySource`, `probe.describe()` (all computed already at `:2226-2247`).

- [ ] **Step 1: Write the failing pure test** in `native/tests/IsoEncoderAdmissionTest.cpp`:

```cpp
TEST(IsoEncoderAdmission, DecisionIdsAreStableStrings) {
  using corevideo::modules::IsoAdmissionDecision;
  using corevideo::modules::isoAdmissionDecisionId;
  EXPECT_STREQ(isoAdmissionDecisionId(IsoAdmissionDecision::Admit), "admit");
  EXPECT_STREQ(isoAdmissionDecisionId(IsoAdmissionDecision::AdmitWithWarning), "admit-with-warning");
  EXPECT_STREQ(isoAdmissionDecisionId(IsoAdmissionDecision::RefuseIso), "refuse-iso");
}
```

- [ ] **Step 2: Write the failing snapshot test (non-MF, after `:2497`)**, using a sink like `WarningEncoderSink` that sets both fields:

```cpp
namespace {
class AdmissionEncoderSink final : public corevideo::modules::IEncoderSink {
 public:
  void configureRecording(const corevideo::modules::RecordingSessionRequest& r) override { session_.recordingSessionId = r.sessionId; }
  corevideo::modules::OutputSession start(const std::vector<std::string>& d, const std::vector<std::string>&) override {
    session_.active = true; session_.destinations = d; session_.recordingStatus = "recording"; return session_;
  }
  void submit(const corevideo::modules::ProgramFrame&) override {}
  corevideo::modules::OutputSession session() const override { return session_; }
  corevideo::modules::OutputSession session_;
};
}  // namespace

TEST(EncoderRecordingSession, IsoAdmissionAndIsoIndexSurfaceInSnapshotRecordingSection) {
  auto modules = corevideo::modules::createStubModules();
  auto owned = std::make_unique<AdmissionEncoderSink>();
  auto* sink = owned.get();
  modules.encoder = std::move(owned);
  corevideo::core::MediaCore mediaCore(std::move(modules));
  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-recording-session"}, {"sessionId", "adm-1"},
      {"isoParticipantIds", corevideo::rpc::Json::Array{}}});

  const std::string note = "1 ISO source will record on the CPU software encoder, not the GPU (7 on hardware): x.";
  corevideo::modules::IsoAdmissionStatus adm;
  adm.decision = "admit-with-warning"; adm.code = "iso-encoder-software-spill"; adm.message = note;
  adm.hardwareCount = 7; adm.softwareCount = 1; adm.admissibleIsoCount = -1;
  adm.probed = true; adm.probeStatus = "ready"; adm.capacity = "hw=yes sessions<=8"; adm.workload = "h264 1920x1080@60";
  sink->session_.isoAdmission = adm;
  sink->session_.recordingWarning = note;
  corevideo::modules::IsoStreamStatus iso;
  iso.sourceId = "zoom:8"; iso.displayName = "Guest 8"; iso.path = "ISO-08-Guest-8.mp4";
  iso.encoderPath = "software"; iso.fallbackReason = "hardware-capacity-exhausted"; iso.isoIndex = 8;
  sink->session_.isoStreams.push_back(iso);

  const auto state = mediaCore.applyCommands(corevideo::rpc::Json::Array{}, 16.0);
  const auto* recording = state.get("recording");
  ASSERT_NE(recording, nullptr);
  const auto* node = recording->get("isoAdmission");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->getString("decision"), "admit-with-warning");
  EXPECT_EQ(node->getString("code"), "iso-encoder-software-spill");
  EXPECT_EQ(node->getString("message"), note);
  EXPECT_EQ(node->getNumber("softwareCount"), 1);
  EXPECT_TRUE(node->get("admissibleIsoCount")->isNull()) << "unknown must be null, never -1 or 0";
  EXPECT_EQ(recording->getString("warning"), note);          // text unchanged
  EXPECT_EQ(recording->getString("status"), "recording");    // a notice, not a failure
  // streams[]: find the ISO and check its index (adapt accessor names to rpc::Json).
}
```

Adapt the accessor calls (`get`, `getString`, `getNumber`, `isNull`) to the real `rpc::Json` API; the existing test at `:2768-2793` shows the pattern. Before writing the stream assertion, check how `CoreStateCapture` reads the encoder session (`streams` built at `:1935-1960`).

- [ ] **Step 3: Extend the MF test** `MediaFoundationEightZoomIsoWritersSurviveEncoderCapacity` (`:1470`, inside the MF guard). After the existing loop:

```cpp
  ASSERT_TRUE(session.isoAdmission.has_value());
  EXPECT_EQ(session.isoAdmission->decision, "admit-with-warning");
  EXPECT_EQ(session.isoAdmission->code.rfind("iso-encoder-software-spill", 0), 0u) << session.isoAdmission->code;
  EXPECT_EQ(session.isoAdmission->hardwareCount, 7);
  EXPECT_EQ(session.isoAdmission->softwareCount, 1);
  EXPECT_EQ(session.isoAdmission->message.rfind("1 ISO source will record on the CPU software encoder", 0), 0u);
  for (const auto& iso : session.isoStreams) {
    EXPECT_GE(iso.isoIndex, 1) << iso.sourceId;
    if (iso.encoderPath == "software") EXPECT_EQ(iso.isoIndex, 8) << iso.sourceId;  // the overflow is the last placed
  }
```

Add a second MF test in the same block, `MediaFoundationProgramOnlyTakeAfterSpillClearsIsoAdmission`. Arm the 8-ISO forced-capacity request, stop, then arm a request with no `isoSources`. Assert `!session.isoAdmission.has_value()`.

- [ ] **Step 4: Run, expect FAIL (does not compile / assertions fail)**

```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='IsoEncoderAdmission.*'
native\build-dev\corevideo-native-tests.exe --gtest_filter='EncoderRecordingSession.IsoAdmission*'
native\build-dev\corevideo-native-tests.exe --gtest_filter='EncoderRecordingSession.MediaFoundationEightZoom*'
```

- [ ] **Step 5: Implement**
  - `isoAdmissionDecisionId` in `IsoEncoderAdmission.h`.
  - `IsoAdmissionStatus`, `IsoStreamStatus::isoIndex`, `OutputSession::isoAdmission` in `Interfaces.h`.
  - `IsoWriterEntry` gains `int isoIndex = 0`. Set `entry.isoIndex = static_cast<int>(i + 1)` beside `idxBuf` (`:2293`), and `status.isoIndex = entry.isoIndex` in `publishIsoTrack`.
  - At `:1628` (where `recordingWarning` is cleared at session start), add `session_.isoAdmission.reset()`.
  - After `evaluateIsoAdmission` (`:2246`), when `!isoSourceIds.empty()`, fill `session_.isoAdmission` from `admission`, `capacitySource`, `isoCapacitySummary_`. Set it **before** `if (!admission.armIso())` clears the selection, so a refusal is published too. When ISO is refused for the unwritable folder (`:2201-2208`), leave it absent: that refusal already has its own warning and placement never ran.
  - The per-session ISO status vector is rebuilt near `:2415-2440`. Make sure `isoIndex` survives into `target.isoStreams`.
  - `CoreStateCapture.cpp`: emit `{"isoIndex", iso.isoIndex}` in the stream object when `iso.isoIndex > 0`. Emit `recording.isoAdmission` when present, with `admissibleIsoCount` as `nullptr` when `< 0`.
  - `AsyncEncoderSink`: confirm the session copy is wholesale (`fresh = inner->session()` or equivalent). If it rebuilds fields, copy `isoAdmission` there.
- [ ] **Step 6: Run the three filters above, then the full recording suites**, all PASS:

```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='EncoderRecordingSession.*'
native\build-dev\corevideo-native-tests.exe --gtest_filter='IsoEncoder*'
native\build-dev\corevideo-native-tests.exe --gtest_filter='AsyncEncoderSink.*'
```

Report the exact pass counts. If any `EncoderRecordingSession.MediaFoundation*` test reports skipped (MF unavailable), the task is not done.

---

### Task 3: Shell — model fields, pure readout, InfoBar

**Files:**
- Modify: `native-shell/CoreVideoPro.MediaCore/Models/NativeMediaCoreProtocol.cs` (`NativeMediaCoreRecordingStream` `:383-405`; `NativeMediaCoreRecordingSession` `:407+`)
- Create: `native-shell/CoreVideoPro.WinUI/ViewModels/Transport/IsoEncoderPlacementReadout.cs`
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs` (beside `IsoHealthWarning`, `:234-277`; raise change notifications where `IsoHealthWarning` is raised, `:196`)
- Modify: `native-shell/CoreVideoPro.WinUI/Views/StudioWorkspace.xaml` (one InfoBar after `:1162`)
- Test: create `native-shell/CoreVideoPro.WinUI.Tests/IsoEncoderPlacementReadoutTests.cs`. Add one deserialization case to `native-shell/CoreVideoPro.MediaCore.Tests/TransportMappingTests.cs`.

**Interfaces:**
- Produces (C#):
  ```csharp
  // NativeMediaCoreProtocol.cs
  public sealed record NativeMediaCoreIsoAdmission
  {
      public string? Decision { get; init; }  public string? Code { get; init; }
      public string? Message { get; init; }   public int HardwareCount { get; init; }
      public int SoftwareCount { get; init; } public int UnplacedCount { get; init; }
      public int? AdmissibleIsoCount { get; init; }
      public bool Probed { get; init; }       public string? ProbeStatus { get; init; }
      public string? Capacity { get; init; }  public string? Workload { get; init; }
  }
  // NativeMediaCoreRecordingSession: public NativeMediaCoreIsoAdmission? IsoAdmission { get; init; }
  // NativeMediaCoreRecordingStream:  public int? IsoIndex { get; init; }  public long? VideoWorkUs { get; init; }
  //                                  public double? FrameRate { get; init; }   (JSON "frameRate", already emitted)
  ```
  ```csharp
  // IsoEncoderPlacementReadout.cs
  public enum PlacementSeverity { None, Informational, Warning }
  public sealed record PlacementReadout(PlacementSeverity Severity, IReadOnlyList<string> Lines);
  public static class IsoEncoderPlacementReadout
  {
      public static PlacementReadout Describe(NativeMediaCoreRecordingSession? recording, bool programHardwareAccelerated);
  }
  ```
- Consumes: `recording.Streams[]` (`Kind`, `IsoIndex`, `DisplayName`, `SourceId`, `EncoderPath`, `FallbackReason`, `VideoWorkUs`, `MuxVideoFrameCount`, `DroppedFrames`, `FrameRate`). `recording.Encoder.HardwareAccelerated` (check the property name in `NativeMediaCoreRecordingEncoder`, `:460`).

- [ ] **Step 1: Write failing tests** (`IsoEncoderPlacementReadoutTests.cs`):

```csharp
[Fact] public void AllHardware_NoReadout()
    => Assert.Equal(PlacementSeverity.None, IsoEncoderPlacementReadout.Describe(Rec(Iso(1, "hardware")), true).Severity);

[Fact] public void SoftwareIsoUnderBudget_IsInformational_WithMeasuredCost()
{
    // 600 frames, 5,640,000 us of work => 9.4 ms per frame; 60 fps => 16.7 ms budget.
    var r = IsoEncoderPlacementReadout.Describe(Rec(Iso(8, "software", "Guest 8", work: 5_640_000, frames: 600, fps: 60)), true);
    Assert.Equal(PlacementSeverity.Informational, r.Severity);
    Assert.Equal("ISO-8 Guest 8 is on the CPU software encoder (GPU sessions used up) · 9.4 ms per frame of a 16.7 ms budget · 0 frames dropped", Assert.Single(r.Lines));
}

[Fact] public void SoftwareIsoOverBudget_IsWarning()
    => Assert.Equal(PlacementSeverity.Warning,
        IsoEncoderPlacementReadout.Describe(Rec(Iso(8, "software", work: 12_000_000, frames: 600, fps: 60)), true).Severity);

[Fact] public void SoftwareIsoThatDropped_IsWarning()
    => Assert.Equal(PlacementSeverity.Warning,
        IsoEncoderPlacementReadout.Describe(Rec(Iso(8, "software", work: 1_000, frames: 600, fps: 60, dropped: 3)), true).Severity);

[Fact] public void NoFramesYet_SaysNotMeasured_NeverZero()
{
    var line = Assert.Single(IsoEncoderPlacementReadout.Describe(Rec(Iso(8, "software", work: null, frames: 0, fps: 60)), true).Lines);
    Assert.Contains("cost not measured yet", line);
    Assert.DoesNotContain("0.0 ms", line);
}

[Fact] public void MissingFrameRate_OmitsBudget_DoesNotAssume60()
{
    var line = Assert.Single(IsoEncoderPlacementReadout.Describe(Rec(Iso(8, "software", work: 5_640_000, frames: 600, fps: null)), true).Lines);
    Assert.Contains("9.4 ms per frame", line);
    Assert.DoesNotContain("budget", line);
}

[Fact] public void ProgramOnSoftware_GetsItsOwnLine()
    => Assert.Contains(IsoEncoderPlacementReadout.Describe(Rec(), programHardwareAccelerated: false).Lines,
        l => l.StartsWith("Program is on the CPU software encoder", StringComparison.Ordinal));

[Fact] public void MissingIsoIndex_FallsBackToDisplayName()
    => Assert.StartsWith("Guest 8 is on", Assert.Single(
        IsoEncoderPlacementReadout.Describe(Rec(Iso(null, "software", "Guest 8", work: 1, frames: 1, fps: 60)), true).Lines));
```

Write the `Rec(...)` / `Iso(...)` builders in the test file with `required` members filled (`Kind`, `Path`, `Status`). Add a `TransportMappingTests` case that deserializes a snapshot JSON containing `recording.isoAdmission` and `streams[].isoIndex` and asserts the mapped values. Follow the existing JSON-fixture pattern in that file.

- [ ] **Step 2: Run, expect FAIL**

```powershell
dotnet test native-shell/CoreVideoPro.WinUI.Tests/CoreVideoPro.WinUI.Tests.csproj -p:WindowsAppSDKBootstrapAutoInitializeOptions_Default=false -p:WindowsAppSDKBootstrapAutoInitializeOptions_None=true --filter "FullyQualifiedName~IsoEncoderPlacementReadout"
dotnet test native-shell/CoreVideoPro.MediaCore.Tests/CoreVideoPro.MediaCore.Tests.csproj --filter "FullyQualifiedName~TransportMapping"
```

- [ ] **Step 3: Implement.** Model fields first, then the readout (pure, no WinUI types). Rules:
  - Lines only for `Kind == "iso"` streams with `EncoderPath == "software"`, in `IsoIndex` order. Program line when `!programHardwareAccelerated` and a Program stream exists.
  - Label: `ISO-{IsoIndex} {DisplayName ?? SourceId}`. With no `IsoIndex`, the name alone.
  - Reason map: `hardware-capacity-exhausted` -> "GPU sessions used up"; `hardware-unavailable` -> "no GPU encoder"; `operator-selected` -> "set to software"; anything else -> the raw string.
  - Cost: `VideoWorkUs / MuxVideoFrameCount / 1000` formatted `0.0`, only when both > 0. Budget `1000 / FrameRate` only when `FrameRate > 0`. "{DroppedFrames} frames dropped" always (it is a measured counter).
  - Severity: Warning if any line has cost > budget or `DroppedFrames > 0`. Otherwise Informational if any line exists. Otherwise None.
  - `StudioViewModel`: `IsoEncoderPlacementMessage` (joined lines), `IsoEncoderPlacementSeverity` (mapped to `InfoBarSeverity`), `IsoEncoderPlacementVisibility`. Raise them wherever `IsoHealthWarning` is raised.
  - XAML: `<InfoBar IsOpen="True" IsClosable="False" Severity="{x:Bind ViewModel.IsoEncoderPlacementSeverity, Mode=OneWay}" Message="{x:Bind ViewModel.IsoEncoderPlacementMessage, Mode=OneWay}" Visibility="{x:Bind ViewModel.IsoEncoderPlacementVisibility, Mode=OneWay}" AutomationProperties.Name="ISO encoder placement" />`.
- [ ] **Step 4: Run Step 2 filters, PASS. Then run the full `npm run test:native-shell`, PASS.** Build the WinUI project Release (`dotnet build ... -c Release -p:Platform=x64`) so the XAML compiles.
- [ ] **Step 5: UI check (owner not interrupted):** launch with the fake engine per `CLAUDE.md` "Synthetic meeting setup", with `COREVIDEO_ENCODER_PROBE_MAX_SESSIONS=8`. Arm 8 ISOs through the control API (no input injection) and record. Screenshot the Record panel's ISO area through the app's own capture path, or UIA read of the InfoBar `Message`. Run an axe-equivalent contrast check on the InfoBar colors (Informational and Warning, light and dark).

---

### Task 4: Shell — pin the start path at the coordinator

**Files:**
- Test: `native-shell/CoreVideoPro.WinUI.Tests/TransportCoordinatorTests.cs` (beside `:327`)
- Modify: none expected. If a test fails, fix only `TransportStatusFormatter.TryFormatRecordingStartHealthFailure`, and stop to report it.

**Interfaces:**
- Consumes: `TransportCoordinator.ToggleRecordingAsync`, the fake host's `SyncResult`, `OutputStatus`, `Recording`. The `Build()` helper.

- [ ] **Step 1: Write the tests**

```csharp
[Fact]
public async Task ToggleRecording_DoesNotRollBack_OnSoftwareIsoAdmissionNotice_935()
{
    var (coordinator, _, host) = Build();
    host.Recording = false;
    const string notice = "1 ISO source will record on the CPU software encoder, not the GPU (7 on hardware): x.";
    host.SyncResult = new NativeMediaCoreStateSnapshot
    {
        Recording = new NativeMediaCoreRecordingSession
        {
            SessionId = "s-935", Active = true, Status = "warning", WriterStatus = "preparing",
            TargetFolder = @"C:\r", FilenamePrefix = "show", Format = "mp4", Quality = "high",
            ProgramPath = @"C:\r\show.mp4", Warning = notice,
            Lifecycle = new() { State = "producing", Health = "degraded" /* fill required members */ },
            IsoAdmission = new() { Decision = "admit-with-warning", Code = "iso-encoder-software-spill", Message = notice, HardwareCount = 7, SoftwareCount = 1 },
        },
        OutputHealth = [ new NativeMediaCoreOutputHealth { Destination = "recording", Status = "warning", Message = notice } ],
    };
    var syncsBefore = host.SyncCount;   // use whatever the fake host counts; add a counter if absent

    await coordinator.ToggleRecordingAsync();
    await Task.Delay(800);              // longer than SyncFailedRecordingStartRollbackAsync's 4 x 150 ms

    Assert.True(host.Recording);
    Assert.Equal("Recording start requested.", host.OutputStatus);
    Assert.Equal(syncsBefore + 1, host.SyncCount);   // the start sync only, no rollback sync
}

[Fact]
public async Task ToggleRecording_StillRollsBack_WhenLifecycleFailed_935()
{
    // Same snapshot, but Lifecycle.State = "failed", Error = "Media Foundation could not open program MP4 writer".
    // Assert: host.Recording == false and OutputStatus starts with "Recording start failed:".
}
```

Check `NativeMediaCoreRecordingSession.Lifecycle`'s type and required members in `NativeMediaCoreProtocol.cs` before writing the initializer.

- [ ] **Step 2: Run, expect PASS for both** (the behaviour shipped in `935b2800`). These are pins. To prove they would catch a regression, temporarily revert `TransportStatusFormatter.cs:125` to `item.Status is "failed" or "warning"`. Confirm the first test FAILs, then restore the line. Record that in the report.

```powershell
dotnet test native-shell/CoreVideoPro.WinUI.Tests/CoreVideoPro.WinUI.Tests.csproj -p:WindowsAppSDKBootstrapAutoInitializeOptions_Default=false -p:WindowsAppSDKBootstrapAutoInitializeOptions_None=true --filter "FullyQualifiedName~TransportCoordinatorTests"
```

---

### Task 5: `validate-iso-record.mjs` — note, not failure; fail on measured loss

**Files:**
- Create: `scripts/qa/iso-record-rules.mjs`, `scripts/qa/iso-record-rules.test.mjs`
- Modify: `scripts/validate-iso-record.mjs` (`:46`, `:243-264`, `:280-292`, `:336-347`, `:350-393`)
- Modify: `package.json` (`test:scripts`: append `scripts/qa/iso-record-rules.test.mjs`)

**Interfaces:**
- Produces:
  ```js
  // iso-record-rules.mjs
  export function classifyRecordingWarning(warning, isoAdmission)  // -> { kind: 'none'|'note'|'failure', text }
  export function isoPlacementNotes(streams, fps)                  // -> string[] (same wording as the shell readout)
  export function isoLossRegressions(baselineStreams, stopStreams) // -> string[] failures (droppedFrames / writeQueue.dropped grew)
  ```
- Consumes: snapshot `recording.warning`, `recording.isoAdmission`, `recording.streams[]`.

- [ ] **Step 1: Write failing tests** (`iso-record-rules.test.mjs`):

```js
import test from 'node:test';
import assert from 'node:assert/strict';
import { classifyRecordingWarning, isoPlacementNotes, isoLossRegressions } from './iso-record-rules.mjs';

const note = '1 ISO source will record on the CPU software encoder, not the GPU (7 on hardware): x.';
const adm = { decision: 'admit-with-warning', code: 'iso-encoder-software-spill', message: note };

test('no warning is none', () => assert.equal(classifyRecordingWarning(null, adm).kind, 'none'));
test('exact admission message is a note', () => assert.equal(classifyRecordingWarning(note, adm).kind, 'note'));
test('admission text with the #1030 loss sentence prepended fails', () =>
  assert.equal(classifyRecordingWarning('Recording lost video frames or audio packets: ... ' + note, adm).kind, 'failure'));
test('admission text with anything appended fails', () =>
  assert.equal(classifyRecordingWarning(note + ' Could not write recording manifest to x.', adm).kind, 'failure'));
test('the sentence without a structured admission fails (no regex waiver)', () =>
  assert.equal(classifyRecordingWarning(note, undefined).kind, 'failure'));
test('a refusal is never a note', () =>
  assert.equal(classifyRecordingWarning(note, { ...adm, decision: 'refuse-iso' }).kind, 'failure'));
test('notes name the ISO and the measured cost', () =>
  assert.deepEqual(isoPlacementNotes([{ kind: 'iso', isoIndex: 8, displayName: 'Guest 8', encoderPath: 'software',
    fallbackReason: 'hardware-capacity-exhausted', videoWorkUs: 5640000, muxVideoFrameCount: 600, droppedFrames: 0 }], 60),
  ['ISO-8 Guest 8 is on the CPU software encoder (GPU sessions used up) · 9.4 ms per frame of a 16.7 ms budget · 0 frames dropped']));
test('no frames yet says not measured', () =>
  assert.match(isoPlacementNotes([{ kind: 'iso', isoIndex: 8, encoderPath: 'software', muxVideoFrameCount: 0 }], 60)[0], /cost not measured yet/));
test('a writer whose drops grew fails', () =>
  assert.deepEqual(isoLossRegressions([{ kind: 'iso', sourceId: 'zoom:8', droppedFrames: 0, writeQueue: { dropped: 0 } }],
                                      [{ kind: 'iso', sourceId: 'zoom:8', droppedFrames: 2, writeQueue: { dropped: 0 } }]),
  ['ISO zoom:8 steady-state droppedFrames 0 -> 2']));
test('missing counters are a failure, not zero', () =>
  assert.equal(isoLossRegressions([{ kind: 'iso', sourceId: 'zoom:8' }], [{ kind: 'iso', sourceId: 'zoom:8' }]).length > 0, true));
```

- [ ] **Step 2: Run, expect FAIL:** `node --test scripts/qa/iso-record-rules.test.mjs`
- [ ] **Step 3: Implement** `iso-record-rules.mjs`, then wire it into `validate-iso-record.mjs`:
  - Replace `:262-263` with `classifyRecordingWarning(lastWarning, snap?.recording?.isoAdmission)`. Collect `failure` texts into a `Set` and push each once after the loop. Collect `note` into `notes`.
  - If `isoAdmission?.decision === 'refuse-iso'`, push `ISO admission refused: <message>`.
  - Keep the baseline ISO streams with `lossBaseline`. After the existing loss checks (`:288-292`), push `isoLossRegressions(isoStreamsOf(lossBaseline), finalStreams)`.
  - Print `Note          : <line>` for each `isoPlacementNotes(finalStreams, 60)` before `Result`.
  - `--allow-capacity-warning`: keep parsing it. Print `allow-capacity-warning: now the default; the flag waives nothing more`. Its only effect is to be recorded in the evidence.
  - Evidence JSON adds `notes`, `isoAdmission` (from the stop snapshot), and `isoPlacement: finalStreams.map(({sourceId, isoIndex, encoderPath, fallbackReason, videoWorkUs, muxVideoFrameCount, droppedFrames}) => ...)`.
  - Update the header comment `:14` ("recording.warning stays empty") to the new rule.
- [ ] **Step 4: Run tests PASS, then `npm run test:scripts` PASS.** The live run is Task 8.

---

### Task 6: Outputs drill — placement is a note, cost is measured

**Files:**
- Modify: `scripts/qa/outputs-drill-lib.ps1` (`New-DrillThroughout` `:353-360`; `Update-DrillThroughout` `:362-395`; `Get-DrillSampleRow` `:397-414`)
- Modify: `scripts/qa/outputs-drill.ps1` (`param(` `:32`; env setup `:132`, `:173`; per-second loop: core process CPU sampling)
- Modify: `scripts/qa/outputs-drill-judge.py` (`THROUGHOUT_FLAGS` `:40`; `:265`; `aggregate` `:157-185`; `summary_text`; ISO rows)
- Test: `scripts/qa/outputs-drill.test.ps1` (`:321-333`), `scripts/qa/test_outputs_drill.py` (`:295-298`, new cases)

**Interfaces:**
- Produces (measurement.json, additive):
  ```json
  "hardwareEncodersThroughout": false,
  "encoderPlacement": { "zoom:16778240": { "paths": ["hardware"], "changed": false }, "zoom:8": { "paths": ["software"], "changed": false } },
  "programEncoderHardwareThroughout": true,
  "encoderCost": { "samples": 1800, "logicalProcessors": 28,
                   "coreCpuPercent": { "mean": 31.2, "peak": 47.9 },
                   "software": { "zoom:8": { "isoIndex": 8, "meanVideoWorkUs": 9400, "peakIntervalVideoWorkUs": 14100, "droppedFramesStart": 0, "droppedFramesEnd": 0 } } }
  ```
- Produces (judge): `GATING_FLAGS = ('remotes1080Throughout', 'captureProgressThroughout', 'programSourcesAdmittedThroughout', 'streamHealthyThroughout')`. Missing-flag detection names `('remotes1080Throughout', 'captureProgressThroughout', 'programSourcesAdmittedThroughout')` explicitly. The schema becomes `outputs-drill-verdict-v3`. Each software ISO row gets `encoder: 'software'`. Notes gain one line per software ISO.
- New drill parameter: `-ProbeMaxSessions <int>` (optional, 1..16). It sets `COREVIDEO_ENCODER_PROBE_MAX_SESSIONS` for the launch, is recorded in `drill.json`, and is printed in the summary workload line.

- [ ] **Step 1: Change the PowerShell test first** (`outputs-drill.test.ps1:328-331`). The software-ISO and software-Program envelopes must now leave every **gating** flag true. They must record `hardwareEncodersThroughout = $false`, and `encoderPlacement['capture:f4978a7f6d614c0d'].paths` must equal `@('software')`:

```powershell
$t = New-DrillThroughout -Plan $plan; $null = Update-DrillThroughout -Throughout $t -Envelope (New-Envelope -EncoderPath 'software') -Plan $plan -Progress @{} -ElapsedMs 0
Assert (-not $t.hardwareEncodersThroughout) 'software ISO not recorded as a fact'
Assert (@($t.encoderPlacement['capture:f4978a7f6d614c0d'].paths) -join ',' -eq 'software') 'placement not recorded'
foreach ($k in 'remotes1080Throughout', 'captureProgressThroughout', 'programSourcesAdmittedThroughout', 'streamHealthyThroughout') { Assert $t[$k] "$k failed on a placement-only difference" }
```

Add a check that an ISO path changing between samples sets `changed = $true`. Add a check that `Get-DrillEncoderCost` (new, pure, takes two CPU samples and two snapshots) computes `meanVideoWorkUs` only when `muxVideoFrameCount > 0`, and returns `$null`, not 0, otherwise.

- [ ] **Step 2: Change the Python tests first** (`test_outputs_drill.py`):

```python
def test_software_iso_placement_alone_does_not_fail_a_clean_run(self):
    verdict, rows = self.run_drill(flags=dict(FLAGS_TRUE, hardwareEncodersThroughout=False),
                                   extra_measurement={'encoderCost': {'software': {WEBCAM: {'isoIndex': 1, 'meanVideoWorkUs': 9400}}}})
    self.assertEqual(verdict['overall'], 'PASS')
    self.assertEqual(verdict['throughoutFailures'], [])
    self.assertEqual(rows['ISO ' + WEBCAM]['encoder'], 'software')
    self.assertTrue(any('on software encoder' in n for n in verdict['notes']))
    self.assertEqual(verdict['schema'], 'outputs-drill-verdict-v3')

def test_software_iso_with_missing_identity_still_fails(self):
    # Reuse the fixture that drops one ISO identity; add hardwareEncodersThroughout=False.
    ...
    self.assertEqual(verdict['overall'], 'FAIL')
```

Update `test_missing_throughout_flags_fail` (`:295-298`) to assert `'remotes1080Throughout (not recorded)'` instead of the hardware flag. Add `extra_measurement` to `run_drill` if it is absent.

- [ ] **Step 3: Run both, expect FAIL**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\outputs-drill.test.ps1
python scripts\qa\test_outputs_drill.py
```

- [ ] **Step 4: Implement.**
  - The lib keeps computing the hardware fact, under `hardwareEncodersThroughout` and `programEncoderHardwareThroughout`, but writes them as facts outside the gating loop: they are updated, and they do not add to `firstFailures`. It accumulates `encoderPlacement`.
  - `Get-DrillEncoderCost` is a pure function. The per-second loop in `outputs-drill.ps1` reads `(Get-Process -Id $corePid).TotalProcessorTime` and `[Environment]::ProcessorCount`, computes `delta CPU / (delta wall * logical)` as a percentage, and records mean and peak. Each software ISO gets `videoWorkUs` and `muxVideoFrameCount` deltas per interval, so a peak-interval mean exists, plus `droppedFrames` at start and end.
  - The judge uses `GATING_FLAGS`, sets schema v3, and adds the `encoder` field to ISO rows. It writes notes in the form `ISO-<n> <source> on software encoder: mean <x> ms of <y> ms per frame, core CPU mean <m> % / peak <p> %`, with "not measured" when absent. The summary row gets ` encoder=software`.
  - Update `docs/qa/outputs-drill.md` in Task 7, not here.
- [ ] **Step 5: Run Step 3, PASS.** Re-judge one existing evidence folder offline (for example a copy of `D:\CoreVideo-QA\beta-20261021-build\artifacts\qualification-1008-25` if it is in drill layout, else skip and say so) and confirm the overall verdict is unchanged for an all-hardware run.

---

### Task 7: Documentation

**Files:**
- Modify: `docs/reference/iso-recording.md` (`:97-100` validator paragraph; add a short "#935 capacity notice" section)
- Modify: `docs/qa/outputs-drill.md` (throughout list `:73-79`, verdict rule `:96-104`, new `-ProbeMaxSessions` row in the parameter table, `encoderCost` in measurement)
- Modify: `docs/reference/encoder-capacity-probe.md` (the `recording.isoAdmission` node, and that probe semantics are unchanged)
- Modify: `CLAUDE.md` only if a reference-topic index line needs the new section name

- [ ] **Step 1:** Write the rule in one sentence in each doc: "Encoder placement is reported, never gated; a run fails only on measured loss." Name the fields (`isoAdmission`, `isoIndex`, `encoderCost`) and the tests that pin them.
- [ ] **Step 2:** Under "Not yet proven" in `outputs-drill.md`, state that the software-ISO 30-minute run is the result Task 8 reports, and copy its numbers there when Task 8 completes.

---

### Task 8: Acceptance on the dev box

**Files:**
- Create (evidence, not committed): `D:\CoreVideo-QA\iso935\accept-*.json`, `D:\CoreVideo-QA\drills\<timestamp>-iso935\`

**Interfaces:**
- Consumes: the Release build with Tasks 2-6, the installed app built from this branch (for the drill), the test meeting (full pwd link, see `cvp-test-meeting-link`).

- [ ] **Step 1: Validator, default spill (no flag):**

```powershell
node scripts/validate-iso-record.mjs --sources 8 --seconds 20 --evidence D:\CoreVideo-QA\iso935\accept-default.json
$env:COREVIDEO_ENCODER_PROBE_MAX_SESSIONS = '8'
node scripts/validate-iso-record.mjs --sources 8 --seconds 20 --evidence D:\CoreVideo-QA\iso935\accept-forced.json
Remove-Item Env:COREVIDEO_ENCODER_PROBE_MAX_SESSIONS
```

Expected: both print `ISO-RECORD VALIDATION PASS`. The forced run prints exactly one `Note :` line naming ISO-8 with a measured ms/frame. Record the ms/frame, the `isoAdmission.code` and `probed`.

- [ ] **Step 2: Validator, loss still fails:** with the forced spill, `--stall-source zoom:<software ISO source id from Step 1> --stall-ms 6000`. Expected: FAIL, naming loss or abandonment, not the note. Quote the failure lines.
- [ ] **Step 3: 30-minute outputs drill with one software ISO** (owner box free; this is disruptive):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\outputs-drill.ps1 `
  -RemoteIds <8 ids> -MeetingUrl '<full test-meeting link with pwd>' -ProbeMaxSessions 8
```

Report, per output, the identity verdict. For the software ISO, report mean and peak ms/frame, `droppedFrames` start and end, and core CPU mean and peak. Also report Program `programMissingFrames`, `programBuffer.underruns`, the Program GPU deadlines line, the hardware (RTX 4090, 28 logical CPUs), the workload and the measured duration. If the software ISO FAILs identity, report it as a finding: the software budget formula admits a stream this machine cannot carry at 1080p60. Do not waive it, and file it as a separate issue.
- [ ] **Step 4: Shell smoke (installed app):** start a recording with Program + 8 ISOs and `COREVIDEO_ENCODER_PROBE_MAX_SESSIONS=8` in the launch environment. Confirm through the control API snapshot and UIA that the recording stays producing, that no `Recording start failed` status appears, and that the placement InfoBar names ISO-8. Stop, then delete the MP4s after keeping manifests (owner disk rule).
- [ ] **Step 5:** Put the numbers in `docs/qa/outputs-drill.md` "Not yet proven" (Task 7 Step 2) and in a comment on #935. Leave #935 open for the owner to close.

---

## Self-review

- Spec section 1 (no cancel on a notice; failed stays fatal): Task 4, plus the existing `935b2800` test.
- Spec section 2 (structured node, `isoIndex`): Task 2.
- Spec section 3 (operator sees which ISO and its measured cost): Task 3.
- Spec section 4 (validator and drill rules; measured CPU): Tasks 5 and 6, run in Task 8.
- Spec section 5 / Acceptance 1-7: Tasks 8, 8, 4, 2, 3, 6/8 and 7 respectively.
- Probe semantics untouched: no task edits `EncoderCapacityProbe.*`, `IsoEncoderPlacement.h` decisions or `evaluateIsoAdmission`. Task 1 only measures.
- Identity judges untouched: no task edits `*-pixel-verdict.py` or `camera-pixel-receiver.mjs`.
