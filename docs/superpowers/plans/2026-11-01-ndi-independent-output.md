# NDI Output Independent of Streaming — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** NDI out becomes its own persisted on/off output (like the virtual camera): it runs with or without Stream or Record, and the stream no longer validates, proves, counts or stops NDI.

**Architecture:** Shell-only change. The shell splits NDI out of the stream destination builders into an NDI builder gated on a new `NdiOutputEnabled` flag; the command builder includes the NDI destinations in the one `start-program-output` desired-state list whenever NDI is on. Stream validation, proof, telemetry and the close guard filter NDI out through one shared predicate; NDI gets its own validation, status label, footer tile and control actions. The native core is unchanged (it already runs NDI-only destination lists and edges per destination id); one native test pins that.

**Tech Stack:** C# / .NET 9, WinUI 3 (code + XAML), xUnit; C++17 native core with the repo's gtest shim.

**Spec:** `docs/superpowers/specs/2026-11-01-ndi-independent-output-design.md`

## Global Constraints

- Destination ids are exactly `ndi` and `ndi-graphics` (`StudioStreamOutputValidation.NdiGraphicsKeyDestination`).
- Preferences schema bumps from 15 to **16**; `NdiOutputEnabled` defaults to `false`; the v15 `StreamNdiEnabled` value must **not** turn NDI on.
- Existing preference keys `StreamNdiProgramName`, `StreamNdiGroupName`, `StreamNdiGraphicsKeyEnabled`, `StreamNdiGraphicsName` keep their names and values.
- NDI format is the stream output profile (resolution, fps); no new profile.
- Control actions: `transport.ndi.set` (`on`: bool, required), `transport.ndi.toggle` (no args), `transport.ndi.graphicsKey.set` (replaces `transport.stream.ndi.graphicsKey.set`, same params). Every new id goes in `StudioControlSurface.SupportedActionIds` (the coverage test fails otherwise).
- Control state: `NdiOutputOn` (bool), `NdiOutputStatus` (string). OSC feedback address `/cvp/state/ndi` (0/1).
- NDI status label strings, exactly: `Off`, `Starting`, `Live · {name}`, `Warning · {reason}`, `Unavailable · {reason}`; append ` + key` when the `ndi-graphics` sender is `live`.
- No input injection, no launching the app from tests; native tests run with `COREVIDEO_VCAM_SHM_DIR` set and an exact `--gtest_filter=Suite.Name` from `native/build-dev`.
- Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **Stop a stream while NDI is on** — the `ndi` sender must not restart (receiver keeps the picture). Pinned by Task 2's command-builder test and Task 6's native test.
2. **Record only + NDI on, then NDI off** — the recording generation must not restart (#466) and `stop-encoder-session` must not be sent while recording. Pinned in Task 2.
3. **Upgrade from v15 with the old NDI checkbox ticked** — NDI must stay off at first launch. Pinned in Task 1.
4. **Invalid NDI name typed while NDI and a stream are both running** — only NDI turns off; stream untouched. Pinned in Task 3.
5. **Core without `ndi-output` (stub / Mac without libNDI)** — the NDI toggle refuses with the reason and nothing else changes; stream start is unaffected. Pinned in Task 3 and Task 5.

---

### Task 1: NDI flag, persistence v16, NDI destination builders split from stream

**Files:**
- Modify: `native-shell/CoreVideoPro.WinUI/Services/ProductionOutputPreferencesStore.cs` (`CurrentVersion` 68, `StreamNdiEnabled` 83, migration ~432)
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.StreamDestinations.cs` (8-51, 53-163)
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs` (`_streamNdiEnabled` 518, save ~11959-11969, load ~12173-12183, change handler 3493)
- Create: `native-shell/CoreVideoPro.MediaCore/Models/OutputDestinationKind.cs`
- Test: `native-shell/CoreVideoPro.WinUI.Tests/ProductionOutputPreferencesStoreTests.cs`, `native-shell/CoreVideoPro.MediaCore.Tests/OutputDestinationKindTests.cs`, `native-shell/CoreVideoPro.WinUI.Tests/NdiOutputDestinationTests.cs` (new)

**Interfaces:**
- Produces: `public static class OutputDestinationKind { public static bool IsNdi(string? destinationId); public static bool IsStream(string? destinationId); }` in `CoreVideoPro.MediaCore.Models` — `IsNdi` is true for `ndi` and `ndi-graphics`; `IsStream` is true for any non-empty id that is neither NDI nor `recording`.
- Produces: `ProductionOutputPreferences.NdiOutputEnabled` (bool); `StudioViewModel.NdiOutputEnabled` (`[ObservableProperty]`, persisted, restored via backing field like `_virtualCameraEnabled`).
- Produces: `private IReadOnlyList<string> BuildNdiDestinations(bool validatedOnly = false)` and `private IReadOnlyList<MediaCoreStreamDestinationWire> BuildNdiDestinationSettings()` on `StudioViewModel` (moved bodies of the NDI branches; gated on `NdiOutputEnabled`).
- `StreamNdiEnabled` is deleted from the view model; the preferences class keeps a `[Obsolete]` read-only-on-load `StreamNdiEnabled` only for migration.

- [ ] **Step 1: Write failing tests**
  - `OutputDestinationKindTests`: `IsNdi("ndi")`, `IsNdi("ndi-graphics")` true; `IsNdi("rtmp")`, `IsNdi(null)` false; `IsStream("rtmp")`, `IsStream("srt")`, `IsStream("hls")` true; `IsStream("ndi")`, `IsStream("recording")`, `IsStream("")` false.
  - `ProductionOutputPreferencesStoreTests.V15WithStreamNdiTickedLoadsWithNdiOutputOff`: a v15 JSON with `"StreamNdiEnabled": true, "StreamNdiProgramName": "Studio A"` loads as `Version == 16`, `NdiOutputEnabled == false`, `StreamNdiProgramName == "Studio A"`.
  - `ProductionOutputPreferencesStoreTests.NdiOutputEnabledRoundTrips`: save `true`, load `true`.
  - `NdiOutputDestinationTests` (pure, through the extracted static `StudioStreamOutputValidation.SelectNdiDestinations(bool ndiOn, string? programName, bool keyIncluded, bool validatedOnly)` returning `IReadOnlyList<string>`): off → empty; on with name → `["ndi"]`; on with key → `["ndi","ndi-graphics"]`; on with blank name and `validatedOnly: true` → empty; on with blank name and `validatedOnly: false` → `["ndi"]`.
- [ ] **Step 2: Run them, confirm they fail** — `dotnet test native-shell/CoreVideoPro.WinUI.Tests --filter "FullyQualifiedName~ProductionOutputPreferencesStoreTests|FullyQualifiedName~NdiOutputDestinationTests"` and the MediaCore filter `OutputDestinationKindTests`. Expected: compile errors / FAIL.
- [ ] **Step 3: Implement** `OutputDestinationKind`, preferences v16 (`NdiOutputEnabled`; migration reads and discards `StreamNdiEnabled`), `StudioStreamOutputValidation.SelectNdiDestinations`, the VM flag and the two NDI builders; remove the NDI branches from `BuildSelectedStreamDestinations` / `BuildStreamDestinationSettings`. Fix every compile reference to `StreamNdiEnabled` by switching it to `NdiOutputEnabled` (the transport and telemetry semantics change in Tasks 3–4; here only keep it compiling).
- [ ] **Step 4: Run the tests, confirm PASS**, then the full WinUI and MediaCore suites: `dotnet test native-shell/CoreVideoPro.WinUI.Tests` and `.../CoreVideoPro.MediaCore.Tests`. Expected: only the stream-as-NDI tests listed in Tasks 2–4 may fail; note them, do not fix here.
- [ ] **Step 5: Commit** `NDI: own on/off flag (prefs v16), NDI destinations split from stream builders`.

### Task 2: Production sync carries NDI independently of Stream and Record

**Files:**
- Modify: `native-shell/CoreVideoPro.MediaCore/Models/MediaCoreProductionSyncContext.cs` (~324)
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/MediaCoreCommandBuilder.cs` (`BuildOutputCommand` ~693-750, encoder-session block ~97-122)
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs` (sync context ~9244-9247)
- Test: `native-shell/CoreVideoPro.MediaCore.Tests/MediaCoreCommandBuilderTests.cs`

**Interfaces:**
- Consumes: `BuildNdiDestinations(validatedOnly: true)`, `BuildNdiDestinationSettings()` (Task 1).
- Produces: `MediaCoreProductionSyncContext.NdiOutput` (bool) and `.NdiDestinations` (`IReadOnlyList<string>`, default empty). `StreamDestinationSettings` carries stream **and** NDI settings (VM concatenates).

- [ ] **Step 1: Write failing tests** in `MediaCoreCommandBuilderTests`:
  - `NdiOnAloneStartsProgramOutputWithOnlyNdi`: Recording false, Streaming false, NdiOutput true, NdiDestinations `["ndi","ndi-graphics"]` → one `start-program-output` with `destinations == ["ndi","ndi-graphics"]`, a `start-encoder-session`, no `stop-encoder-session`.
  - `StoppingTheStreamKeepsNdiInTheDestinationList`: Streaming false, NdiOutput true, StreamDestinations `["rtmp"]` → destinations `["ndi"]` (no `rtmp`).
  - `StreamOnNdiOffSendsNoNdi`: Streaming true, NdiOutput false, StreamDestinations `["rtmp"]`, NdiDestinations `["ndi"]` → destinations `["rtmp"]`.
  - `RecordingPlusNdiThenNdiOffNeverStopsTheEncoderSession`: Recording true + NdiOutput true → `["recording","ndi"]`; then Recording true + NdiOutput false → `["recording"]` and no `stop-encoder-session` in either batch.
  - `NothingOnStopsTheEncoderSession`: all false → `stop-encoder-session` present (existing `StopsEncoderAndRecordingWhenOutputsAreIdle` keeps passing).
  - Update `ArmsRecordingAndStreamingOutputsWhenEnabled` (:358, :437, :446) and `BuildsFullTakeSyncBatchWithSceneRoutesRosterAndOutputs` (:544, :557) to pass NDI via `NdiOutput`/`NdiDestinations`; expected order `recording`, stream ids, then NDI ids.
- [ ] **Step 2: Run** `dotnet test native-shell/CoreVideoPro.MediaCore.Tests --filter FullyQualifiedName~MediaCoreCommandBuilderTests` — FAIL.
- [ ] **Step 3: Implement**: `BuildOutputCommand` returns null only when `!Recording && !Streaming && !NdiOutput`; destinations = `recording` (if Recording) + `StreamDestinations` (if Streaming) + `NdiDestinations` (if NdiOutput), `Distinct` keeping that order. VM sets `NdiOutput = NdiOutputEnabled`, `NdiDestinations = BuildNdiDestinations(validatedOnly: true)`, `StreamDestinationSettings = BuildStreamDestinationSettings().Concat(BuildNdiDestinationSettings()).ToArray()`.
- [ ] **Step 4: Run** the filter, then the full MediaCore suite — PASS.
- [ ] **Step 5: Commit** `NDI: production sync carries NDI with or without Stream/Record`.

### Task 3: Stream transport ignores NDI; NDI toggle validates and fails alone

**Files:**
- Modify: `native-shell/CoreVideoPro.WinUI/Services/StudioStreamOutputValidation.cs` (`ValidateSelectedDestinations` 35-79, `NdiGraphicsKeyRefusal` 305-318)
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/Transport/TransportStatusFormatter.cs` (:23-39, :41-81, :147-179, :333-347, :554-561, :709-711)
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/Transport/TransportCoordinator.cs` (:590-593, :785), `ITransportHost.cs` (:145, :163), `StudioViewModel.Transport.cs` (:342-343, :363)
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs` (`OnStreamOutputOptionChanged` 10067-10120, `SyncOutputProfileChangeAsync` ~10035, `ValidateStreamDestinations` 9755-9789, NDI change handlers 3493-3525, `SetNdiGraphicsKey` 1917-1932, `IncludeNdiGraphicsKeyDestination` 1912-1914)
- Test: `native-shell/CoreVideoPro.WinUI.Tests/OutputLifecycleTransportTests.cs`, `StudioViewModelAudioStatusTests.cs`, `NdiGraphicsKeyOutputTests.cs`, `TransportCoordinatorTests.cs` (fake host :1485), new `NdiOutputValidationTests.cs`

**Interfaces:**
- Consumes: `OutputDestinationKind.IsNdi` (Task 1).
- Produces: `public static string? StudioStreamOutputValidation.ValidateNdiOutput(string? programName, string? graphicsName, bool graphicsKeyOn, bool? coreHasNdiOutput, bool? coreHasGraphicsKey)` → `null` when valid, else the operator-facing reason (reuse today's NDI name / capability texts; "NDI output is not available in this core build (missing ndi-output)." for the capability).
- Produces: `public string? NdiOutputStatusReason` on `StudioViewModel` (`[ObservableProperty]`, not persisted): the last reason NDI was refused or turned off; cleared when NDI turns on successfully. Task 4 passes it to `NdiStatus` as `refusalReason`.
- Produces: `internal string? SetNdiOutput(bool on)` on `StudioViewModel` — validates first (returns reason, changes nothing on refusal), else sets `NdiOutputEnabled`, saves, and runs one `SyncActiveSceneAsync("ndi-on"|"ndi-off")`.
- `ValidateSelectedDestinations` drops its `ndiEnabled`/`ndiProgramName` parameters; its "Select at least one stream destination." check covers RTMP, SRT, HLS only.

- [ ] **Step 1: Write failing tests**
  - `NdiOutputValidationTests`: valid name → null; blank name → the NDI name reason; key name equal to program name → the collision reason; `coreHasNdiOutput == false` → the missing-capability reason; `coreHasNdiOutput == null` (core not up) → null.
  - `OutputLifecycleTransportTests.StreamStartProofIgnoresNdiSenders`: requested `["rtmp"]`, senders rtmp `live` + ndi `failed` → proven.
  - Update `PartialFailureDoesNotStopHealthyDestinationOrProveAllLive` (:31) and `EveryRequestedDestinationMustBeLive` (:40) to use `rtmp` + `srt` instead of `ndi`.
  - `StudioViewModelAudioStatusTests`: rewrite the four `ndi`-based stream start failure tests (:715, :741, :1042, :1074) onto `srt`; add `TryFormatStreamingStartNoSenderFailure_IgnoresNdi` (requested `rtmp`, only an idle `ndi` sender → reports `rtmp` missing, never mentions NDI).
  - `NdiGraphicsKeyOutputTests.ActionRefusesMissingCapabilityOrNdiOffAndAppliesNothing` (:90): the "NDI off" case now reads `NdiOutputEnabled == false`; `KeyThatCannotBeSentIsDroppedAloneAndNeverFailsTheStream` (:72) keeps passing.
- [ ] **Step 2: Run** the four test classes — FAIL.
- [ ] **Step 3: Implement**:
  - Stream: `IsStreamingStartProven`, the no-sender and health formatters, `ValidateStreamDestinationCapabilities` and the stream status texts consider only ids where `OutputDestinationKind.IsStream` is true; drop NDI from `StreamDestinationSummary`, `BuildConfiguredStreamDestinationLabels` and the "Enable RTMP, NDI, SRT, or HLS" text (now "Enable RTMP, SRT, or HLS before streaming.").
  - `OnStreamOutputOptionChanged` / `SyncOutputProfileChangeAsync`: a stream validation failure stops the stream only (`StreamingRequested = false`); then, if `NdiOutputEnabled`, run `ValidateNdiOutput` and, on a reason, set `NdiOutputEnabled = false` and `NdiOutputStatusReason = reason` without touching `StreamingRequested`.
  - NDI property handlers (name, group, key, key name): save; if `NdiOutputEnabled`, validate NDI (turn NDI off on failure, as above), else one sync; never call stream validation.
  - `SetNdiGraphicsKey` / `NdiGraphicsKeyRefusal`: "NDI off" means `!NdiOutputEnabled`; refusal text becomes "Turn on NDI output first; the graphics key is a second NDI source that rides with it."
  - `ITransportHost.StreamNdiEnabled` is removed (logging reads `NdiOutputEnabled` from the VM directly).
- [ ] **Step 4: Run** the full WinUI suite — PASS.
- [ ] **Step 5: Commit** `NDI: stream validation, proof and failures ignore NDI; NDI validates and fails alone`.

### Task 4: Telemetry, NDI status label, close guard, support bundle

**Files:**
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/StudioOutputTelemetry.cs` (`Streaming` 24-52)
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/LiveProductionSync.cs` (`IsStreamingLive` 270-287, `SummarizeStreamStat` 361-393, `ResolveStreamHealthStatus` 420-440, `SummarizeOutputSession` 462-499)
- Modify: `native-shell/CoreVideoPro.WinUI/Services/CloseGuardPolicy.cs` (~171-194)
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs` (support bundle row 10405-10421; new status label)
- Test: `CoreVideoPro.MediaCore.Tests/StudioOutputTelemetryTests.cs`, `LiveProductionSyncTests.cs` (:478), `NativeMediaCoreStateMapperTests.cs` (:226), `CoreVideoPro.WinUI.Tests/CloseGuardPolicyTests.cs` (:182), new `CoreVideoPro.MediaCore.Tests/NdiOutputTelemetryTests.cs`

**Interfaces:**
- Consumes: `OutputDestinationKind` (Task 1), `NdiOutputEnabled` (Task 1).
- Produces: `public static string StudioOutputTelemetry.NdiStatus(bool enabled, IReadOnlyList<NativeMediaCoreOutputSender> senders, string? programName, string? refusalReason)` returning exactly the strings in Global Constraints (`refusalReason` non-null and `!enabled` → `Unavailable · {reason}`; `enabled` and no `ndi` sender yet → `Starting`).
- Produces: `StudioViewModel.NdiOutputStatusLabel` (string, raised on snapshot apply and on `NdiOutputEnabled` change).

- [ ] **Step 1: Write failing tests**
  - `NdiOutputTelemetryTests`: off → `Off`; on, no sender → `Starting`; on, ndi `live` → `Live · Studio A`; on, ndi `live` + ndi-graphics `live` → `Live · Studio A + key`; on, ndi `warning` with message `runtime missing` → `Warning · runtime missing`; off with reason → `Unavailable · {reason}`.
  - `StudioOutputTelemetryTests.StreamingIgnoresNdiSenders`: senders rtmp `live` 100 frames + ndi `live` 900 frames → Progress counts 100, Health not "Partial"; only ndi `live` → the idle stream readout.
  - `LiveProductionSyncTests`: `IsStreamingLive` false with only ndi senders live; `SummarizeStreamStat` with rtmp+ndi live lists only RTMP. Update `IsStreamingLiveRejectsUnavailableSenderWarning` (:478) onto `srt`.
  - `CloseGuardPolicyTests` (:182): a live `ndi` sender alone → `CloseGuardDecision.Nothing`; rtmp + ndi live → "Streaming to 1 destination".
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement** the filters with `OutputDestinationKind.IsStream`, `NdiStatus`, `NdiOutputStatusLabel`, the close-guard filter, and the support bundle row `Active = NdiOutputEnabled && <ndi sender live>`.
- [ ] **Step 4: Run** the MediaCore and WinUI suites — PASS.
- [ ] **Step 5: Commit** `NDI: own status label; stream telemetry and close guard exclude NDI`.

### Task 5: Control API, OSC, and the UI (footer tile, settings, stream flyout)

**Files:**
- Modify: `native-shell/CoreVideoPro.Control/ControlActionRegistry.cs` (:61-69), `ControlState.cs` (:136-141), `Osc/OscFeedback.cs` (:17-18)
- Modify: `native-shell/CoreVideoPro.WinUI/Services/StudioControlSurface.cs` (:45, :62-63, :255-265, :1014-1015, `SupportedActionIds`)
- Modify: `native-shell/CoreVideoPro.WinUI/Views/StudioWorkspace.xaml` (remove NDI checkbox 1337-1338; add NDI tile beside Virtual Cam ~1366), `native-shell/CoreVideoPro.WinUI/Views/ProductionSettingsWindow.xaml` (remove 314-315; add "NDI output" switch at the top of 458-482)
- Test: `CoreVideoPro.Control.Tests/ControlActionRegistryTests.cs`, `CoreVideoPro.WinUI.Tests/StudioControlSurfaceCoverageTests.cs`, `StreamDestinationControlTests.cs` (:54, :122), new `CoreVideoPro.WinUI.Tests/NdiOutputActionTests.cs`; layout probe `scripts/test-studio-layout.ps1`

**Interfaces:**
- Consumes: `SetNdiOutput(bool)` (Task 3), `NdiOutputStatusLabel` (Task 4), `SetNdiGraphicsKey` (existing).
- Produces: registry entries `transport.ndi.set` (`on`, bool, required), `transport.ndi.toggle`, `transport.ndi.graphicsKey.set` (same params as the old id; old id removed); `ControlState.NdiOutputOn`, `ControlState.NdiOutputStatus`; OSC `/cvp/state/ndi`.
- Static helper for testability: `public static ControlInvokeResult StudioControlSurface.NdiSetResult(string? refusal)` → `Fail(refusal)` or `Success`.

- [ ] **Step 1: Write failing tests**
  - `ControlActionRegistryTests.NdiActionsAreRegisteredAndTheOldKeyIdIsGone`: the three ids exist with the stated params; `transport.stream.ndi.graphicsKey.set` does not.
  - `NdiOutputActionTests`: `NdiSetResult("reason")` is a failure carrying "reason"; `NdiSetResult(null)` succeeds.
  - `StudioControlSurfaceCoverageTests.Adapter_HandlesEveryRegisteredAction` (existing) covers the new ids.
  - `StreamDestinationControlTests` (:54, :122): drop NDI from the stream-destination retention cases.
- [ ] **Step 2: Run** the Control and WinUI filters — FAIL.
- [ ] **Step 3: Implement** the registry, surface cases (`transport.ndi.set` → `SetNdiOutput(on)`; `toggle` → `SetNdiOutput(!NdiOutputEnabled)`; key action renamed), state fill and publish triggers (`NdiOutputEnabled`, `NdiOutputStatusLabel`), OSC feedback. XAML: an **NDI** `ToggleButton` tile (width 160, same template as Virtual Cam: glyph, "NDI" title, `NdiOutputStatusLabel` subtitle; `IsChecked` bound one-way, `Click` calls `SetNdiOutput(!NdiOutputEnabled)` so refusals are shown, not swallowed) with a flyout holding program name, group, graphics key switch and key name; Settings › NDI gets an "NDI output" `ToggleSwitch` at the top (same `SetNdiOutput` path); remove both NDI stream-destination checkboxes.
- [ ] **Step 4: Run** the Control and WinUI suites; build the shell Release x64; run `scripts/test-studio-layout.ps1` (with no CoreVideo app running) — all PASS, footer fits at 1360x768.
- [ ] **Step 5: Commit** `NDI: footer tile, settings switch, transport.ndi.* actions, OSC state`.

### Task 6: Native pin, docs, CLAUDE.md, BACKLOG

**Files:**
- Test: `native/tests/AsyncOutputSenderTest.cpp` (next to `TogglingTheGraphicsKeyNeverInterruptsProgramNdi`, :381)
- Modify: `docs/reference/stream-destination-control.md` (:32-39), `docs/reference/ndi-graphics-key.md` (:46, :88-117), `docs/reference/output-supervisor.md` (:9, :52-59), `CLAUDE.md` (:222 row), `docs/BACKLOG.md` (new row)

**Interfaces:**
- Consumes: nothing new (pins existing core behaviour).

- [ ] **Step 1: Write the test** `AsyncOutputSender.RemovingTheStreamDestinationNeverInterruptsNdi`: sync `["rtmp","ndi"]`, send frames, sync `["ndi"]`; the `ndi` sender's start count stays 1 and its frame count keeps rising; same for adding `rtmp` back.
- [ ] **Step 2: Run** from `native/build-dev` with `COREVIDEO_VCAM_SHM_DIR` set: `corevideo-native-tests.exe --gtest_filter=AsyncOutputSender.RemovingTheStreamDestinationNeverInterruptsNdi` — expected PASS without core changes (if it fails, stop and report: the spec's "core unchanged" premise is wrong).
- [ ] **Step 3: Docs** — `stream-destination-control.md`: NDI is not a stream destination; new "NDI output" section (on/off, persistence, validation, status strings, actions). `ndi-graphics-key.md`: "rides with NDI output" (no streaming), action renamed. `output-supervisor.md`: NDI senders are driven by the NDI flag. `CLAUDE.md` row for `ndi-graphics-key.md` names `transport.ndi.graphicsKey.set`; add a row or sentence for NDI output. `docs/BACKLOG.md`: row "NDI output independent of streaming (2026-11-01)" with the spec link and the open question (separate NDI format).
- [ ] **Step 4: Run** full shell suites one last time (Control, MediaCore, WinUI) — PASS.
- [ ] **Step 5: Commit** `NDI: native pin that stream edits never restart NDI; docs`.
