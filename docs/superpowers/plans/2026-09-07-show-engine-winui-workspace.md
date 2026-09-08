# Show Engine WinUI Workspace (Plan 7b) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give operators a native "OHG Show" tab in the Windows shell — four thin panels rendered from the engine snapshot, an always-visible status strip, and a settings section that edits the show config and hot-restarts the engine — plus a one-shot importer from the legacy Isadora config files.

**Architecture:** `OhgShowViewModel` holds exactly two inputs (the latest `ShowEngineSnapshot` and the `ShowEngineHealth`) and projects them, through a pure `OhgSnapshotProjection`, into diff-updated observable collections and scalar properties; every button is an `ohg.*` invoke through an `IOhgActionInvoker` seam. No show logic lives in the shell. Settings edits go through `OhgSettingsViewModel` → `ShowConfigStore` → `ShowConfigValidator` → `MainWindow.ApplyShowConfigAsync`, which swaps the adapter and restarts the engine over the same spawn request.

**Tech Stack:** C# .NET 9 / WinUI 3 (CommunityToolkit.Mvvm `[ObservableProperty]`/`[RelayCommand]`), `System.Text.Json`, xUnit 2.9. No new NuGet packages.

**Spec:** `docs/superpowers/specs/2026-09-07-show-engine-host-bridge-design.md` §10 (workspace), §9 (config editor + importer), §11 row "workspace". Parent: `docs/superpowers/specs/2026-08-04-ohg-show-engine-design.md` §4.4. Read `docs/superpowers/plan-authoring-rules.md` (rules 1–10, and the candidate rule 11 in the Plan 7a outcomes) before Task 1. Plan 7a's outcomes: `docs/superpowers/plans/2026-09-07-show-engine-host-bridge-outcomes.md`.

## Global Constraints

- Branch `plan/show-engine-winui-workspace`, stacked on `plan/show-engine-host-bridge` (PR #412). If #412 has merged, rebase onto `main` first.
- **0xc000027b rules (CLAUDE.md):** never replace a bound collection at frame rate — diff-update in place under a signature; every queued UI callback goes through `UiDispatch` (never a raw `TryEnqueue`); `Selector.SelectedValue` is never driven by x:Bind inside an ItemsRepeater template; give every new interactive control an `AutomationProperties.Name`.
- **Bridge events fire on the engine's reader thread.** The ViewModel receives them through an injected `Action<Action> marshal` (production: `UiDispatch.Run` on the window's `DispatcherQueue`; tests: synchronous).
- **The panels are thin renderers.** Every displayed value comes from the snapshot projection; every behaviour is an `ohg.*` invoke. A ViewModel member that computes show state (who is on air, which box is filled, queue order) is a defect — read it from the snapshot.
- **Snapshot wire shape** is `show-engine/src/showSnapshot.ts` (`ShowSnapshot`) with nested types in `contracts.ts`, `programBus.ts`, `lookDirector.ts`, `tallyPublisher.ts`, `overlayDirector.ts`, `mukanaClient.ts`; the `ProgramSource` wire strings for `ohg.program.preview`/`directCut` are `black | gallery | activeSpeaker | look:<id> | slot:<n>`; PINs and participant ids are STRINGS.
- Test conventions: page-level ViewModels are constructed with plain data and no `DispatcherQueue`; anything UI-affine is extracted as a pure static and tested directly. XAML content tests read the `.xaml` text and assert binding paths and automation names (`OperatorUiTelemetryPlacementTests` pattern).
- Design tokens: `Themes/StudioTheme.xaml` brushes/fonts and `Views/OperatorTabResources.xaml` styles only (`OperatorSectionBorder`, `OperatorSectionTitle`, `OperatorSummary`, `OperatorGhostButton`, `OperatorReadoutBorder`); tally colours `StudioAirBrush` (on air), `StudioProgramBrush` (preview/warning), `StudioLiveBrush` (ok).
- **Do NOT use `git stash`.**
- Every task carries a **Mutations to run** block; a task is not done until each named mutation has reddened the named test for the named reason and been reverted. Report results.
- Commit after every task with the message given.

## Model guidance per task (advisory — the executing controller decides)

Calibrated on Plan 7a: every Critical finding there came from a more capable reviewer on a threading or cross-task seam; well-specified mechanical tasks landed with at most one fix round on a mid-tier implementer.

| Task | Implementer | Task reviewer | Why |
|---|---|---|---|
| 1 Snapshot projection | mid (sonnet) | mid | pure JSON → records; the brief carries the field list and tests |
| 2 Invoker seam + arg builders | mid | mid | 28 mechanical builders against a literal id list |
| 3 ViewModel core | **most capable (opus)** | **most capable** | marshaling, diff-update under the 0xc000027b rules, signature gate — threading judgment |
| 4 Panelist commands | mid | mid | dispatch-only partial |
| 5 Program commands + labels | mid | mid | dispatch + pure labels; adds the diff-updated `Boxes` collection (follow Task 3's pattern) |
| 6 Gallery + GFX commands | mid | mid | dispatch-only partial |
| 7 Tab plumbing + page + panelist board XAML | **most capable** | **most capable** | touches `StudioViewModel` tab machinery and `StudioWorkspace.xaml`; XAML compile + automation names |
| 8 Remaining panel XAML | **most capable** | mid | XAML volume; selection-in-code-behind rule |
| 9 Config edit model + settings VM | mid | mid | pure round-trip + validation; well specified |
| 10 Settings section, adapter hot-swap, restart on save | **most capable** | **most capable** | `MainWindow`/`StudioControlSurface` lifecycle; ordering seam |
| 11 Isadora importer | mid | mid | pure text extraction with synthetic fixtures |
| 12 Conformance case, parked minors, docs, outcomes | **most capable** | — (final whole-branch review follows) | cross-language conformance edit + synthesis from all reports |
| Scoped re-reviews of small fix diffs | — | cheap/mid (haiku/sonnet) | verify named findings only |
| Final whole-branch review | — | **most capable** | seams the per-task reviews cannot see |

Fix-loop escalation (rounds 4–5) goes one tier above the stuck implementer, per the SDD skill.

## Decisions taken before this plan (controller rulings, 2026-09-07)

- **D1 The tab is always visible.** The spec's "tab visible only when the config file exists" is replaced by: the tab is always present; with no config the page renders a single "Set up OHG" surface whose button opens the settings section. (Hiding a tab on a file's existence is a discoverability trap; the setup surface is the honest state.)
- **D2 One bridge and one supervisor for the app lifetime.** A settings save does NOT rebuild the control catalog: `MainWindow.ApplyShowConfigAsync` validates, re-materializes the effective config at the same path, swaps the `OhgHostAdapter` on the control surface (`StudioControlSurface.ReplaceOhgAdapter`), and calls `bridge.RestartAsync()` — the spawn request's config path is unchanged. **First-time setup (no config at launch) requires an app restart** and the page says so after save; the launch path (`StartShowEngine`) is the only place the catalog is built.
- **D3 `OhgShowViewModel` is attached by `MainWindow`**, not constructed by `StudioViewModel` (the bridge lives in the window). `StudioViewModel` gains an `[ObservableProperty] OhgShowViewModel? OhgShow` that the page binds `OneWay`; null ⇒ setup surface.
- **D4 The importer reads the two legacy files as TEXT** with tolerant extraction (the files are JS object literals, not JSON; no samples exist in the repo — shapes come from `docs/superpowers/specs/2026-08-04-ohg-isadora-actor-reference.md` §0 C/D/H and lines 397–404): Mukana base URL + `event` from the `php-panel-rest.php?event=<x>` URL; capacity from the first `videoPins`; looks are NOT in the legacy files, so the importer writes the spec's four default looks (`hr-q`, `banter`, `teatime`, `panel-checks`) with EMPTY `scenePreset` for the operator to pick. Everything it could not find is reported, never guessed.
- **D5 Overrides editor is a form, not a grid:** PIN, name, location, role → `ohg.mukana.override.set`; a "Delete override" needs only the PIN. Existing overrides are shown by reading `panelists[]` rows whose `role` differs from Mukana's — the snapshot does not expose the override table, so the panel shows the effective role and offers set/delete; this is recorded as a carry (expose `overrides` on the snapshot) rather than derived in the shell.

## Carried obligations discharged here (authoring rule 6)

| Carried from | Obligation | Discharged in |
|---|---|---|
| Plan 7a outcomes | render `restoreWarnings`, `pagingRefused`, adapter refusals (the "loud, never silent" channel) | Task 3 (status strip), Task 7 |
| Plan 7a outcomes | config-change ⇒ restart; editor; importer | Tasks 9, 10, 11 |
| Plan 7a outcomes | a conformance case with an unseated chair | Task 12 |
| Plan 7a final review (parked) | CLAUDE.md's unreachable 30 s rung; supervisor disposal behind the bridge-null return | Task 12 |

NOT discharged here (remain carries; do not attempt): Tiles gallery ordering (core API), per-input lower-third title, the core positional route fallback (empty box not guaranteed blank), engine `setPreview`-before-`applyLook` double cue, tally URL posting, raise-hand.

## File Structure

**Create (WinUI project `native-shell/CoreVideoPro.WinUI/`):**
- `Services/OhgSnapshotProjection.cs` — pure: `JsonElement` snapshot → `OhgSnapshotView` records.
- `Services/OhgSnapshotView.cs` — the view records (`OhgPanelistRow`, `OhgSlotRow`, `OhgGalleryCellRow`, `OhgProgramView`, `OhgQueueView`, `OhgLookView`, `OhgOverlayView`, `OhgHealthView`, `OhgCapabilityView`, `OhgSnapshotView`).
- `Services/IOhgActionInvoker.cs` + `Services/BridgeOhgActionInvoker.cs` — the invoke seam and its bridge implementation.
- `Services/OhgActionArgs.cs` — pure static builders for the 28 actions' positional args (typed).
- `ViewModels/OhgShowViewModel.cs` — the page VM (partial class split across `OhgShowViewModel.Panelists.cs`, `.Program.cs`, `.Gallery.cs`, `.Gfx.cs`).
- `ViewModels/OhgPanelistRowViewModel.cs`, `OhgSlotRowViewModel.cs`, `OhgGalleryCellViewModel.cs` — per-row observable items (diff-updated in place).
- `ViewModels/OhgSettingsViewModel.cs` + `ViewModels/OhgLookEditorViewModel.cs` — the config editor model.
- `Services/OhgConfigEditModel.cs` — pure: `ShowConfig` ⇄ editable model ⇄ `ShowConfig` (JSON round-trip of the `engine` block).
- `Services/IsadoraConfigImporter.cs` — pure text importer.
- `Views/OhgShowPage.xaml(.cs)` — the tab page (UserControl with a `ViewModel` DP of type `StudioViewModel`, like `AutomationPage`).
- Tests (`CoreVideoPro.WinUI.Tests/`): `OhgSnapshotProjectionTests.cs`, `OhgActionArgsTests.cs`, `OhgShowViewModelTests.cs` (+ `.Panelists/.Program/.Gallery/.Gfx` partials), `OhgConfigEditModelTests.cs`, `OhgSettingsViewModelTests.cs`, `IsadoraConfigImporterTests.cs`, `OhgShowPageContentTests.cs`, `FakeOhgActionInvoker.cs`.

**Modify:** `Models/ProductionModels.cs` (`StudioTab.OhgShow`), `ViewModels/StudioViewModel.cs` (tab plumbing + `OhgShow` property), `Views/StudioWorkspace.xaml` (nav button + host), `Views/ProductionSettingsWindow.xaml(.cs)` (OHG section), `Services/StudioControlSurface.cs` (`ReplaceOhgAdapter`), `MainWindow.xaml.cs` (attach the VM; `ApplyShowConfigAsync`), `show-engine/src/conformance.ts` (unseated-chair case) + `OhgConformanceGoldens.cs`, `CLAUDE.md`.

---

### Task 1: Snapshot projection (pure)

**Files:** Create `Services/OhgSnapshotView.cs`, `Services/OhgSnapshotProjection.cs`; test `OhgSnapshotProjectionTests.cs`.

**Interfaces — Produces:**

```csharp
namespace CoreVideoPro.WinUI.Services;

public sealed record OhgPanelistRow(string ParticipantId, string DisplayName, string Location, string? Pin,
    bool HasMukana, string Role, bool Online, bool VideoOn, bool AudioOn, bool HandRaised, int? Slot /* seated slot or null */);
public sealed record OhgSlotRow(int Slot, OhgPanelistRow? Panelist, bool OnAir);
public sealed record OhgGalleryCellRow(int Cell, int Slot /* 0 = blank */, string? DisplayName);
public sealed record OhgProgramView(string Program, string Preview, bool ActiveSpeakerFollow, string? ActiveSpeakerId);
   // Program/Preview are the WIRE strings ("black", "gallery", "activeSpeaker", "look:<id>", "slot:<n>")
public sealed record OhgQueueView(IReadOnlyList<string> Previous, string? Current, IReadOnlyList<string> Upcoming);
public sealed record OhgBoxView(int Box, int? Slot, string? DisplayName);
public sealed record OhgLookView(string LookId, string ScenePreset, int? HostSlot, int? ReaderSlot,
    IReadOnlyList<OhgBoxView> Boxes, int Page, int PageCount, string BoxFill);
public sealed record OhgOverlayView(string? QuestionText, string? QuestionAsker, string? HeadlineName, string? HeadlineLocation, bool HeadlineVisible);
public sealed record OhgCapabilityView(string State, string? Detail);
public sealed record OhgHealthView(string Panelists, string Hands, string Question, string Worst);
public sealed record OhgSnapshotView(
    long Revision, IReadOnlyList<OhgPanelistRow> Panelists, IReadOnlyList<OhgSlotRow> Slots,
    IReadOnlyList<OhgGalleryCellRow> Gallery, OhgQueueView Queue, OhgProgramView Program, OhgLookView? Look,
    IReadOnlyDictionary<int, int> ManualBoxes, IReadOnlyList<int> OnAirSlots, OhgOverlayView Overlays,
    OhgCapabilityView Registry, OhgCapabilityView HandsQueue, OhgCapabilityView QuestionFeed,
    OhgHealthView Health, bool SmartGallery, IReadOnlyList<OhgPanelistRow> Unseated,
    string? PagingRefused, IReadOnlyList<string> RestoreWarnings);

public sealed record OhgLookOption(string Id, string Label);   // a configured look for the picker (Task 5) — built from config, not from the snapshot

public static class OhgSnapshotProjection
{
    /// Total: never throws. Missing/malformed nodes project to empty/neutral values and are listed in `warnings`.
    public static OhgSnapshotView Project(JsonElement snapshot, out IReadOnlyList<string> warnings);
    /// The wire string for a ProgramSource object: {kind:"look",lookId} → "look:<id>", {kind:"slot",slot} → "slot:<n>", else kind.
    public static string FormatProgramSource(JsonElement source);
    /// Pure: `engine.looks[]` → options (id, label ?? id); malformed entries skipped. Used by MainWindow (Task 10) to build the picker list.
    public static IReadOnlyList<OhgLookOption> LookOptionsFromEngine(JsonElement engine);
}
```

Add to the tests: `[Fact] LookOptionsFromEngine_ReadsIdAndLabel_SkipsMalformed()` (an entry without `id` is skipped; a missing `label` falls back to the id).

**Behavior:** read every field by its TS name (`participantId`, `rawName`, `displayName`, `location`, `pin`, `hasMukana`, `role`, `online`, `videoOn`, `audioOn`, `handRaised`; `slots[].slot/panelist`; `gallery[].cell/slot`; `queue.previous/current/upcoming`; `program.program/preview/activeSpeakerFollow/activeSpeakerId`; `look.lookId/scenePreset/hostSlot/readerSlot/boxes[].box/slot/page/pageCount/boxFill`; `manualBoxes` (object keyed by box number); `tally.onAirSlots`; `overlays.question.text/askerName`, `overlays.headline.name/location`, `overlays.headlineVisible`; `capabilities.registry|handsQueue|questionFeed.state/detail`; `health.panelists|hands|question.state`; `smartGallery`; `unseated[]`; `pagingRefused`; `restoreWarnings[]`). `OhgSlotRow.OnAir` = slot ∈ `tally.onAirSlots`. `OhgPanelistRow.Slot` = the slot whose panelist has this `participantId`. `OhgGalleryCellRow.DisplayName` = the seated panelist's name for that slot (null when blank/unseated). `Health.Worst` = worst of the three by rank failing > dormant > ok (mirror `controlState.ts`).

**Tests** (rule 1: state fixture invariants — `slots.length == capacity`, gallery cells 1..16, look boxes reference seated slots):

```csharp
[Fact] public void ProjectsAFullSnapshot()        // one literal JSON with every node populated; assert every record field (one Assert per field group)
[Fact] public void FormatsEveryProgramSourceKind() // 5 kinds incl. leading-zero-free slot 7 and look id with a dot
[Fact] public void AnEmptySeatIsAHole_AndOnAirComesFromTally()
[Fact] public void GalleryCellNamesFollowTheSeatedPanelist_BlankIsNull()
[Fact] public void WorstMukanaHealthWins()        // ok/dormant/failing → "failing"; ok/ok/dormant → "dormant"
[Fact] public void MissingNodesProjectNeutral_AndAreListedAsWarnings()  // snapshot {} → empty lists, Program "black"/"black", warnings names each missing node
[Fact] public void NeverThrowsOnGarbage()         // slots: "x", gallery: 5, look: [] → returns, warnings non-empty
```

**Mutations to run:**
- Read `onAir` from `slot.panelist != null` instead of `tally.onAirSlots` → the hole/on-air test reds.
- Format `{kind:"slot",slot:7}` as `"slot7"` → the kinds test reds.
- Throw on a missing `gallery` node → the missing-nodes test reds.

**Steps:** tests red → implement → `dotnet test native-shell/CoreVideoPro.WinUI.Tests --filter "FullyQualifiedName~OhgSnapshotProjection"` green → mutations → commit `feat(winui): pure projection of the show-engine snapshot into view records (Plan 7b Task 1)`.

---

### Task 2: Action invoker seam and typed arg builders

**Files:** Create `Services/IOhgActionInvoker.cs`, `Services/BridgeOhgActionInvoker.cs`, `Services/OhgActionArgs.cs`; tests `OhgActionArgsTests.cs`, `FakeOhgActionInvoker.cs`.

**Interfaces — Produces:**

```csharp
public interface IOhgActionInvoker
{
    /// Never throws. Returns the bridge's ControlInvokeResult (Ok, or Fail(reason/message)).
    Task<ControlInvokeResult> InvokeAsync(string actionId, IReadOnlyList<object?> args, CancellationToken ct = default);
    Task<ControlInvokeResult> RestartEngineAsync(CancellationToken ct = default);
}
public sealed class BridgeOhgActionInvoker : IOhgActionInvoker   // wraps ShowEngineBridge.InvokeAsync / RestartAsync; RestartAsync's InvalidOperationException ⇒ Fail(message)

public static class OhgActionArgs   // one method per action; returns (string ActionId, object?[] Args)
{
    public static (string, object?[]) PanelistAdd(string participantId, int? slot = null);   // slot null ⇒ args ["<pid>"] (omitted — the bridge trims trailing nulls, but do not rely on it)
    public static (string, object?[]) PanelistRemove(int slot);
    public static (string, object?[]) PanelistReplace(int slot, string participantId);
    public static (string, object?[]) PanelistRoleSet(string pin, string role);
    public static (string, object?[]) PanelistSyncAll();
    public static (string, object?[]) ProgramPreview(string source);   // wire string
    public static (string, object?[]) ProgramCut(); ProgramAuto(); ProgramDirectCut(string source); ProgramAsFollowSet(bool on);
    public static (string, object?[]) LookSet(string lookId); LookNextGuest(); LookPrevGuest(); LookBoxAssign(int box, int slot); LookBoxClear(int box);
    public static (string, object?[]) GalleryResetFromSlots(); GalleryReplace(int cell, int slot); GalleryRemove(int cell); GalleryEmpty(); GallerySmartSet(bool on);
    public static (string, object?[]) HeadlineIn(); HeadlineOut(); HeadlineChange(string name, string location); QuestionIn(); QuestionOut();
    public static (string, object?[]) MukanaSync(); OverrideSet(string pin, string name, string location, string role); OverrideDelete(string pin);
    public static string SourceForSlot(int slot) => $"slot:{slot}"; SourceForLook(string id) => $"look:{id}";
}
```

`FakeOhgActionInvoker`: records `(ActionId, Args)` in order; `Handler` func to script results; `RestartCalls` counter.

**Tests:** a `[Theory]` over all 28 builders asserting the action id is one of `OHG_ACTIONS` (copy the 28 ids as literals — the contract) and the arg count/types match the action's params (int for slot/box/cell, string for pin/ids/source, bool for on); `PanelistAdd` with `slot: null` yields exactly one arg; PIN `"0042"` stays a string.

**Mutations to run:** make `PanelistAdd` emit `[pid, null]` → one-arg test reds; type `slot` as string in `PanelistRemove` → theory reds; drop `ohg.gallery.smart.set` from the builder set → the 28-count theory reds.

**Steps:** tests red → implement → filter test green → mutations → commit `feat(winui): the ohg action invoker seam and typed arg builders (Plan 7b Task 2)`.

---

### Task 3: `OhgShowViewModel` core — ingestion, diff-update, status strip

**Files:** Create `ViewModels/OhgShowViewModel.cs`, `OhgPanelistRowViewModel.cs`, `OhgSlotRowViewModel.cs`, `OhgGalleryCellViewModel.cs`; test `OhgShowViewModelTests.cs`.

**Interfaces — Produces:**

```csharp
public sealed partial class OhgShowViewModel : ObservableObject
{
    public OhgShowViewModel(IOhgActionInvoker invoker, Action<Action> marshal, Func<ShowEngineSnapshot?> latest, Func<ShowEngineHealth> health,
        IReadOnlyList<OhgLookOption> looks /* from config: id + label */, bool driveHost);
    /// Subscribe to bridge events (call from the UI thread); Dispose unsubscribes.
    public void Attach(ShowEngineBridge bridge);   // production only; tests drive OnSnapshot/OnHealth directly
    internal void OnSnapshot(ShowEngineSnapshot snapshot);   // may be called from any thread — marshals
    internal void OnHealth(ShowEngineHealth health);         // same
    internal void OnLog(ShowEngineLogLine line);             // same; feeds the refusal/log strip

    // Status strip
    [ObservableProperty] string engineState;        // "stopped"/"starting"/"running"/"recovering"/"failed"
    [ObservableProperty] string engineDetail;       // LastError or "gen N, N restarts"
    [ObservableProperty] bool isShadowMode;         // = !driveHost
    [ObservableProperty] string shadowLastCommand;
    [ObservableProperty] string pagingRefused;      // "" when null
    public ObservableCollection<string> RestoreWarnings { get; }   // diff-updated
    public ObservableCollection<string> RecentRefusals { get; }    // last 10 log lines at level warn/error, newest first
    [RelayCommand] Task RestartEngineAsync();

    // Collections (diff-updated in place, keyed)
    public ObservableCollection<OhgPanelistRowViewModel> Panelists { get; }   // keyed by ParticipantId
    public ObservableCollection<OhgSlotRowViewModel> Slots { get; }           // keyed by Slot, fixed length = capacity
    public ObservableCollection<OhgGalleryCellViewModel> Gallery { get; }     // keyed by Cell, fixed 16
    public ObservableCollection<OhgPanelistRowViewModel> Unseated { get; }
    [ObservableProperty] long revision;
    public OhgSnapshotView? Current { get; }        // last projected view (for tests and the other partials)
}
```

**Behavior:** `OnSnapshot` ⇒ `marshal(() => Apply(OhgSnapshotProjection.Project(...)))`. `Apply` computes a **signature** (revision) and returns early when unchanged; then updates each keyed collection IN PLACE: existing rows get their properties set (each row VM is `ObservableObject` with `[ObservableProperty]` fields), new keys are inserted at their sorted position, missing keys removed — never `Clear()+Add` (0xc000027b rule). Slots/Gallery are fixed-size and only mutate row content. Selection state on the page (selected panelist / selected slot) lives on the VM as `[ObservableProperty] string? SelectedParticipantId` / `int? SelectedSlot` and survives an `Apply`. `OnHealth` ⇒ `EngineState` + `EngineDetail`. `OnLog` ⇒ prepend to `RecentRefusals` when level ∈ {warn, error}, trim to 10.

**Tests** (VM constructed with `FakeOhgActionInvoker`, `marshal = a => a()`):
```csharp
[Fact] public void ApplyingASnapshot_FillsEveryCollection_WithoutReplacingThem()   // capture collection references + CollectionChanged counts; assert rows updated, references same, no Reset action raised
[Fact] public void ASecondSnapshotWithTheSameRevision_IsANoOp()
[Fact] public void RowsAreUpdatedInPlace_NewRowsInsertedSorted_DepartedRemoved()
[Fact] public void SelectionSurvivesASnapshot()
[Fact] public void HealthProjectsToStateAndDetail()   // Failed + LastError; Running gen 3 restarts 1
[Fact] public void WarnAndErrorLogsFeedTheRefusalStrip_NewestFirst_CappedAtTen()
[Fact] public void MarshalIsUsedForEveryIngestPath()  // marshal that records calls; OnSnapshot/OnHealth/OnLog each go through it
[Fact] public async Task RestartEngine_InvokesTheInvoker()
```

**Mutations to run:** replace the in-place update with `Clear()+Add` → the no-replace test reds (Reset action observed / reference identity); drop the revision short-circuit → the no-op test reds; bypass marshal in `OnLog` → the marshal test reds.

**Steps:** tests red → implement → filter green → mutations → commit `feat(winui): OhgShowViewModel core — marshaled ingestion, diff-updated rows, status strip (Plan 7b Task 3)`.

---

### Task 4: Panelist board commands

**Files:** Create `ViewModels/OhgShowViewModel.Panelists.cs`; test `OhgShowViewModelTests.Panelists.cs`.

**Interfaces — Produces:**
```csharp
[RelayCommand] Task AssignSelectedToSlotAsync(int slot);        // selected panelist → ohg.panelist.add(pid, slot); occupied slot ⇒ ohg.panelist.replace(slot, pid)
[RelayCommand] Task AddSelectedToFirstEmptyAsync();             // ohg.panelist.add(pid) — one arg
[RelayCommand] Task RemoveSlotAsync(int slot);                  // ohg.panelist.remove(slot)
[RelayCommand] Task SetRoleAsync((string Pin, string Role) arg);// ohg.panelist.role.set; refused locally (status) when the row has no PIN
[RelayCommand] Task SyncAllAsync();
public IReadOnlyList<string> Roles { get; } = ["panelist","host","reader","aslpanelist","aslinterpreter"];
[ObservableProperty] string lastActionStatus;                   // the invoker's Fail text, or "" on Ok
```
**Behavior:** every command sets `LastActionStatus` from the result (`""` on Ok) and ALSO appends a Fail to `RecentRefusals`. No local state change — the next snapshot renders the outcome.

**Tests:** one fact per command asserting the exact `(ActionId, Args)` recorded by the fake (occupied vs empty slot picks add vs replace, from the projected `Slots`); the no-PIN role refusal never invokes; a scripted `Fail("manual box fill")` lands in `LastActionStatus` and `RecentRefusals`.

**Mutations to run:** always emit `add` even for an occupied slot → replace test reds; swallow the Fail text → refusal test reds.

**Steps:** red → implement → green → mutations → commit `feat(winui): OHG panelist board commands (Plan 7b Task 4)`.

---

### Task 5: Program panel commands

**Files:** Create `ViewModels/OhgShowViewModel.Program.cs`; test `OhgShowViewModelTests.Program.cs`.

**Interfaces — Produces:**
```csharp
public ObservableCollection<OhgLookOption> Looks { get; }        // from ctor (id, label); static for the VM's life
[ObservableProperty] string? selectedLookId;                     // set by SetLook; reflects Current.Look.LookId after a snapshot
[RelayCommand] Task PreviewAsync(string source); CutAsync(); AutoAsync(); DirectCutAsync(string source); SetAsFollowAsync(bool on);
[RelayCommand] Task SetLookAsync(string lookId); NextGuestAsync(); PrevGuestAsync(); AssignBoxAsync((int Box, int Slot) a); ClearBoxAsync(int box);
[RelayCommand] Task PreviewSlotAsync(int slot) => PreviewAsync(OhgActionArgs.SourceForSlot(slot));
public string ProgramLabel { get; } / PreviewLabel { get; }       // "look: Teatime (page 2/3)", "slot 4: Ada Lovelace", "black" — derived from Current only
public string CurrentSpeakerLabel { get; }                        // name of program.activeSpeakerId's panelist or "—"
public string QueueLabel { get; }                                 // "prev: 0042, 0017 · current: 0099 · next: 0003, 0120"
```
**Tests:** each command's recorded `(ActionId, Args)`; labels from a projected snapshot (rule 2: expected strings are fixture constants); `SelectedLookId` follows the snapshot's look after `Apply`.

**Mutations to run:** `PreviewSlotAsync` formats `"slot 4"` → reds; `ProgramLabel` ignores page → label test reds.

**Steps:** red → implement → green → mutations → commit `feat(winui): OHG program panel commands and labels (Plan 7b Task 5)`.

---

### Task 6: Gallery and GFX/data panel commands

**Files:** Create `ViewModels/OhgShowViewModel.Gallery.cs`, `OhgShowViewModel.Gfx.cs`; tests `.Gallery.cs`, `.Gfx.cs`.

**Interfaces — Produces:**
```csharp
// Gallery
[RelayCommand] Task ReplaceCellWithSelectedSlotAsync(int cell);  // requires SelectedSlot ⇒ ohg.gallery.replace(cell, slot); else status
[RelayCommand] Task RemoveCellAsync(int cell); ResetGalleryFromSlotsAsync(); EmptyGalleryAsync(); SetSmartGalleryAsync(bool on);
public string GalleryNote { get; } = "Cell order is not yet applied to Tiles (carried to a core change)";
// GFX & data
[ObservableProperty] string headlineName; headlineLocation;      // editor fields (seeded from the snapshot's headline when empty)
[RelayCommand] Task HeadlineInAsync(); HeadlineOutAsync(); HeadlineChangeAsync();  // change uses the two fields; both required
[RelayCommand] Task QuestionInAsync(); QuestionOutAsync(); MukanaSyncAsync();
[ObservableProperty] string overridePin; overrideName; overrideLocation; overrideRole = "panelist";
[RelayCommand] Task OverrideSetAsync(); OverrideDeleteAsync();   // set requires all four; delete requires pin
public string RegistryLamp { get; } / HandsLamp / QuestionLamp   // capability state strings for the three lamps
public string MukanaHealthLabel { get; }                          // Current.Health.Worst
```
**Tests:** recorded args per command; the required-field local refusals never invoke; lamps/labels from a projected snapshot with `registry: unavailable (detail "no registry configured")` (rule 3: quantify over the three states).

**Mutations to run:** `OverrideSetAsync` with an empty location still invokes → reds; lamp reads `health` instead of `capabilities` → lamp test reds.

**Steps:** red → implement → green → mutations → commit `feat(winui): OHG gallery and GFX/data panel commands (Plan 7b Task 6)`.

---

### Task 7: Tab plumbing and the page (status strip + panelist board)

**Files:** Modify `Models/ProductionModels.cs`, `ViewModels/StudioViewModel.cs`, `Views/StudioWorkspace.xaml`; create `Views/OhgShowPage.xaml(.cs)`; test `OhgShowPageContentTests.cs`.

**Interfaces — Produces:**
- `StudioTab.OhgShow`; `SelectTab("ohgshow")`; `IsOhgShowTab`; `OhgShowTabChrome`; both raised in `OnActiveTabChanged`.
- `StudioViewModel`: `[ObservableProperty] private OhgShowViewModel? _ohgShow;` (D3) + `[RelayCommand] void OpenOhgSettings()` (opens the settings window on the OHG section — Task 10 wires the section id; here it opens the window).
- `StudioWorkspace.xaml`: nav button "OHG Show" in the Produce group after Automation; host `Border` with `<views:OhgShowPage ViewModel="{x:Bind ViewModel, Mode=OneWay}" />`.
- `OhgShowPage.xaml`: root grid with (a) the **status strip** row (engine state readout with `StudioLiveBrush`/`StudioProgramBrush`/`StudioAirBrush` by state, Restart button, shadow badge + last command, `PagingRefused`, `RestoreWarnings` list, `RecentRefusals` list) always visible when `ViewModel.OhgShow` is non-null; (b) the **setup surface** when null ("OHG is not configured" + "Set up OHG" button → `OpenOhgSettingsCommand`); (c) a 2×2 panel grid; this task fills the **Panelist board** cell: master list (`ItemsRepeater` over `OhgShow.Panelists` with name, location, PIN badge, Mukana/video/online glyphs, role chip = ComboBox whose selection is applied on `SelectionChanged` in code-behind (never x:Bind `SelectedValue`), select-on-tap) beside the slot grid (`ItemsRepeater` over `OhgShow.Slots`: slot number, name or "EMPTY", on-air tally border, tap ⇒ `AssignSelectedToSlotCommand`, right-click/flyout ⇒ Remove). Every button/ComboBox/list item gets `AutomationProperties.Name` (`OHG slot 3`, `OHG panelist Ada Lovelace`, `OHG restart engine`, …).
- Page code-behind mirrors `AutomationPage.xaml.cs` (a `ViewModel` DP of `StudioViewModel`) plus the two `SelectionChanged` handlers.

**Tests** (`OhgShowPageContentTests`, XAML-text assertions per the `OperatorUiTelemetryPlacementTests` pattern): the page merges `OperatorTabResources.xaml`; binds `ViewModel.OhgShow.EngineState`, `.RestartEngineCommand`, `.Panelists`, `.Slots`, `.AssignSelectedToSlotCommand`; contains no `SelectedValue="{x:Bind`; every `<Button` and `<ComboBox` in the file has `AutomationProperties.Name`; `StudioWorkspace.xaml` contains `CommandParameter="ohgshow"` and `<views:OhgShowPage`. Plus a `StudioTab` test that `SelectTab("ohgshow")` is routable (pure: add `internal static StudioTab ParseTab(string)` extracted from `SelectTab` and test it).

**Mutations to run:** remove one `AutomationProperties.Name` → the names test reds; bind a ComboBox `SelectedValue` via x:Bind → reds; drop `OhgShow` from `OnActiveTabChanged` raises → add a pure test on the `TabChrome` selector (`OhgShowTabChrome` for `ActiveTab == OhgShow`) that reds.

**Verification:** `dotnet build native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj -c Release -p:Platform=x64` must pass (XAML compiles). Launching the app is NOT required in this task; if an app is already running on this box you may screenshot the tab, otherwise say so.

**Steps:** red → implement → build → tests → mutations → commit `feat(winui): the OHG Show tab — status strip, setup surface, panelist board (Plan 7b Task 7)`.

---

### Task 8: Program, Gallery, and GFX/data panel XAML

**Files:** Modify `Views/OhgShowPage.xaml(.cs)`; extend `OhgShowPageContentTests.cs`.

**Behavior:** fill the remaining three cells. **Program:** PGM/PVW readouts (`OperatorReadoutBorder`; PGM uses `StudioAirBrush` text), Preview/Cut/Auto/Direct-cut buttons (direct-cut takes the PVW source), AS-follow `ToggleSwitch` (applied on `Toggled` in code-behind → `SetAsFollowCommand`), current-speaker plate, look picker (`ComboBox` over `OhgShow.Looks`, selection applied on `SelectionChanged` → `SetLookCommand`), page readout + Prev/Next guest buttons, `PagingRefused` inline, box list (`ItemsRepeater` over `Current.Look.Boxes` exposed as an `ObservableCollection<OhgBoxViewModel>` diff-updated in Task 5 — add it there if not yet: keyed by Box), each box with Assign-selected-slot / Clear. **Gallery:** 4×4 `ItemsRepeater` over `Gallery` (cell number, name or blank), tap ⇒ `ReplaceCellWithSelectedSlotCommand`, flyout Remove, Smart `ToggleSwitch`, Reset-from-slots, Empty, and the `GalleryNote` line. **GFX & data:** question card (asker + text) with In/Out, headline editor (two `TextBox` + In/Out/Change), Mukana sync button + three capability lamps + `MukanaHealthLabel`, override form (PIN/name/location/role ComboBox + Set/Delete). All controls named.

**Tests:** extend the content test: binds `SetLookCommand`, `CutCommand`, `ReplaceCellWithSelectedSlotCommand`, `HeadlineChangeCommand`, `OverrideSetCommand`; the four panel titles present (`Panelist board`, `Program`, `Gallery`, `GFX & data`); names rule holds for every control.

**Mutations to run:** as Task 7 (name removal; a `SelectedValue` x:Bind).

**Steps:** implement → build → tests → mutations → commit `feat(winui): OHG program, gallery, and GFX panels (Plan 7b Task 8)`.

---

### Task 9: Config edit model and `OhgSettingsViewModel`

**Files:** Create `Services/OhgConfigEditModel.cs`, `ViewModels/OhgSettingsViewModel.cs`, `ViewModels/OhgLookEditorViewModel.cs`; tests `OhgConfigEditModelTests.cs`, `OhgSettingsViewModelTests.cs`.

**Interfaces — Produces:**

```csharp
public sealed class OhgLookEdit { string Id; string Label; string? ScenePreset; int Boxes; bool IncludesHost; bool IncludesReader; string PlateTone = "neutral"; string TallySource = "boxes"; string BoxFill = "queue"; }
public sealed class OhgConfigEditModel
{
    public bool RegistryEnabled, HandsQueueEnabled, QuestionFeedEnabled;
    public string? MukanaBaseUrl, MukanaEvent; public int PanelistsIntervalMs = 5000, HandsIntervalMs = 2000, QuestionIntervalMs = 2000, MaxBackoffMs = 60000;
    public int Capacity = 10 /* read-only in the UI */, UtilityPinBase = 9000, GalleryCells = 16;
    public List<string> SkipRoles = ["aslinterpreter"];
    public List<OhgLookEdit> Looks;
    public bool DriveHost; public string? PresetSolo, PresetActiveSpeaker, PresetBlack, PresetGallery; public string DefaultTransition = "cut"; public string? TallyUrl;
    public static OhgConfigEditModel FromConfig(ShowConfig config, out IReadOnlyList<string> warnings);   // unknown engine fields are PRESERVED in `Extra` (a JsonElement bag) and re-emitted
    public ShowConfig ToConfig();                                                                          // builds the engine JsonElement (mukana null when every integration is off, per config.ts)
    public static OhgConfigEditModel Default();   // the spec's four looks with empty presets
}
public sealed partial class OhgSettingsViewModel : ObservableObject
{
    public OhgSettingsViewModel(ShowConfigStore store, Func<IReadOnlySet<string>> sceneIds, Func<IReadOnlyList<(string Id, string Name)>> scenes,
        Func<ShowConfig, Task<string?>> apply /* MainWindow.ApplyShowConfigAsync: returns an error or null */, bool engineStartedAtLaunch);
    public OhgConfigEditModel Model { get; }                       // bound fields
    public ObservableCollection<OhgLookEditorViewModel> Looks { get; }
    public IReadOnlyList<(string Id, string Name)> SceneChoices { get; }   // for the pickers, id + name
    [ObservableProperty] string validationMessage; saveStatus; bool needsAppRestart;
    [RelayCommand] void AddLook(); RemoveLook(OhgLookEditorViewModel look); Task SaveAsync(); Task ImportLegacyAsync(); // Import = Task 11
    public string? Validate();   // ShowConfigValidator over Model.ToConfig() + local: look ids unique & non-empty, boxes ≥ 0, every look has a preset when DriveHost
}
```
**Behavior:** `SaveAsync` ⇒ `Validate()`; on error set `ValidationMessage`, do not save; else `store.Save(cfg)` then `await apply(cfg)`; `apply` error ⇒ `SaveStatus`; when `!engineStartedAtLaunch` set `NeedsAppRestart = true` and `SaveStatus = "Saved. Restart CoreVideo Pro to start the show engine."` (D2).

**Tests:** round-trip `FromConfig(ToConfig(model))` preserves every field and an unknown extra engine field; `Default()` validates only when presets are set and `DriveHost` false; `Validate` catches duplicate look ids, a preset naming a missing scene, capacity ≠ 10; `SaveAsync` does not call `apply` on a validation error; first-time save sets `NeedsAppRestart`; `Looks` add/remove reflect into `Model.Looks`.

**Mutations to run:** drop the `Extra` preservation → round-trip test reds; validate presets only when `!DriveHost` → the DriveHost-requires-presets test reds; call `apply` before `Save` → ordering test (fake store records order) reds.

**Steps:** red → implement → green → mutations → commit `feat(winui): OHG config edit model and settings view model (Plan 7b Task 9)`.

---

### Task 10: Settings section, `ReplaceOhgAdapter`, `ApplyShowConfigAsync`, VM attachment

**Files:** Modify `Views/ProductionSettingsWindow.xaml(.cs)`, `Services/StudioControlSurface.cs`, `MainWindow.xaml.cs`, `ViewModels/StudioViewModel.cs` (`OpenOhgSettings` targets the section); tests `StudioControlSurfaceOhgForwardingTests.cs` (+ replace-adapter pure test), `OhgSettingsSectionContentTests.cs`.

**Interfaces — Produces:**
- `StudioControlSurface.ReplaceOhgAdapter(OhgHostAdapter? adapter)` — swaps the adapter the host-command queue applies to; UI-thread only; the queue is drained naturally (commands already enqueued run against the old adapter — document).
- `MainWindow.ApplyShowConfigAsync(ShowConfig config) : Task<string?>` — validate against `ViewModel.Scenes` ids; write the effective config; build a new `OhgHostAdapter(new StudioViewModelOhgFacade(ViewModel, LaunchLog.Write), config.Shell, LaunchLog.Write, ShowConfigLooks.PresetsByLookId(config.Engine))`; `_controlSurface.ReplaceOhgAdapter(adapter)`; `await _showEngineBridge.RestartAsync(ct)`; returns null or the error text. When `_showEngineBridge is null` (no config at launch) it validates + writes only and returns null (the settings VM shows the restart-app message).
- `MainWindow` attaches the page VM after `StartShowEngine`: `ViewModel.OhgShow = new OhgShowViewModel(new BridgeOhgActionInvoker(bridge), a => UiDispatch.Run(_dispatcher, a, "ohg-show"), () => bridge.Latest, () => bridge.Health, looks, config.Shell.DriveHost)` + `Attach(bridge)`; detached/disposed in `StopShowEngineAsync`.
- Settings window: an "OHG" menu button + `OhgPanel` `ScrollViewer` with the editor: integrations toggles, Mukana URL/event/intervals, looks list (`ItemsRepeater` over `Looks` with id/label/boxes/host/reader/tone/tally/fill + a scene ComboBox applied on `SelectionChanged`), the four preset ComboBoxes, `DriveHost` toggle (labelled "Drive the show (off = shadow mode)"), default transition, tally URL, Save + Import buttons, `ValidationMessage`/`SaveStatus`. `ShowSection("ohg")`. The window is constructed with an `OhgSettingsViewModel` (add a ctor overload or a settable property; `StudioViewModel.OpenOhgSettings` passes it through).

**Tests:** content test for the section (bindings + names + no x:Bind SelectedValue); a pure test that `ReplaceOhgAdapter` swaps the reference used by the next enqueued command (extract the adapter lookup into a `Func<OhgHostAdapter?>` read at apply time and test that).

**Mutations to run:** `ApplyShowConfigAsync` restarts before writing the effective config → add a pure ordering seam (`OhgConfigApplySteps.Order(...)` returning the step list) and a test that reds; content-test name removal reds.

**Verification:** Release x64 build; all three .NET suites.

**Steps:** red → implement → build → tests → mutations → commit `feat(winui): OHG settings section, adapter hot-swap, config apply + engine restart (Plan 7b Task 10)`.

---

### Task 11: Legacy Isadora importer

**Files:** Create `Services/IsadoraConfigImporter.cs`; test `IsadoraConfigImporterTests.cs`; wire `OhgSettingsViewModel.ImportLegacyAsync` (file pickers in the settings window code-behind via `FileOpenPicker`, two files; the VM takes the two file TEXTS).

**Interfaces — Produces:**
```csharp
public sealed record IsadoraImportResult(OhgConfigEditModel Model, IReadOnlyList<string> Found, IReadOnlyList<string> NotFound);
public static class IsadoraConfigImporter
{
    /// Pure. `infrastructureJs` and `mukanaJs` are the raw file texts (either may be null/empty).
    public static IsadoraImportResult Import(string? infrastructureJs, string? mukanaJs, OhgConfigEditModel? existing = null);
}
```
**Behavior (D4):** from `mukanaJs`: the first `https?://[^"'\s]+php-panel-rest\.php\?event=([A-Za-z0-9_-]+)` ⇒ `MukanaBaseUrl` (URL without the query) + `MukanaEvent`, and `RegistryEnabled = HandsQueueEnabled = QuestionFeedEnabled = true`; from `infrastructureJs`: the first `"videoPins"\s*:\s*(\d+)` ⇒ reported as the legacy capacity (`Found` says "legacy videoPins = 8; capacity stays 10 (Show Inputs)") — capacity is NOT changed; tally URL: first `oh\.tally[^"'\s]*` or `"tallyUrl"` string if present. Looks: `Default()`'s four looks with empty presets unless `existing` already has looks (kept). Everything else untouched. `NotFound` lists each probe that found nothing, in plain words.

**Tests:** synthetic legacy texts (JS object literals with the `"oscRole:"` trailing-colon typo and single-quoted strings) → fields found; empty inputs → all `NotFound`; an existing model's looks preserved; capacity never changed.

**Mutations to run:** regex requires double quotes only → single-quote test reds; importer overwrites existing looks → preservation test reds.

**Steps:** red → implement → green → mutations → commit `feat(winui): one-shot importer from the legacy Isadora config files (Plan 7b Task 11)`.

---

### Task 12: Conformance unseated-chair case, parked 7a minors, CLAUDE.md, outcomes

**Files:** Modify `show-engine/src/conformance.ts` (+ its test), `native-shell/CoreVideoPro.WinUI.Tests/OhgConformanceGoldens.cs`, `native-shell/CoreVideoPro.ShowEngine/ShowEngineRestartPolicy.cs` (doc only) or `CLAUDE.md` (the 30 s rung sentence), `MainWindow.xaml.cs` (hoist supervisor disposal above the bridge-null return), `CLAUDE.md` (a "OHG Show tab + settings" paragraph in the OHG section), create `docs/superpowers/plans/2026-09-07-show-engine-winui-workspace-outcomes.md`.

**Behavior:**
- Add a conformance case "a look with no reader chair clears ohg-reader" (a look whose `includesReader` is false / `readerSlot` null) and pin its golden: the `CueSceneWithRoutes` line must carry `ohg-reader=` (null). Run the `AdapterConformance` integration test (spawns node) and paste `conformance: 7/7`.
- Fix the two parked 7a minors named in the carry table.
- CLAUDE.md: how to open the tab, that config saves hot-restart the engine except first-time setup, the importer's honesty rule, and that the panels are thin renderers (a `PropertyChanged` storm on the tab is a snapshot-rate bug, not a UI bug).
- Gates: all three .NET suites; Release x64 build; `npm run test:show-engine`.
- Outcomes doc in the Plan 7a outcomes format (shipped / corrections / decisions / carries / deferred minors / process note / mutations).

**Steps:** implement → gates → commit `test(show-engine): conformance case for an unseated chair; close two parked 7a minors (Plan 7b Task 12a)` then `docs: CLAUDE.md OHG workspace notes and Plan 7b outcomes`.

---

## Self-review (done at authoring)

- **Spec coverage:** §10 panels 1–4 → Tasks 4–8; status strip → Tasks 3, 7; setup surface → Task 7 (D1); config editor → Tasks 9–10; importer → Task 11; §11 "workspace: projection + dispatch tests for every panel control" → Tasks 1–6 (projection + dispatch) and 7–8 (content tests); carries → Task 12.
- **Type consistency:** `OhgSnapshotView` (T1) is what `OhgShowViewModel.Current` (T3) exposes and what T4–T6 labels read; `IOhgActionInvoker` (T2) is the VM's only outbound seam; `OhgActionArgs` builders (T2) are what T4–T6 commands call; `OhgConfigEditModel` (T9) is what the importer (T11) returns and the settings section (T10) binds; `ApplyShowConfigAsync` (T10) is the `apply` func T9's VM takes.
- **Known softness, stated:** the page XAML (T7/T8) and the settings section (T10) are verified by build + content tests + (optionally) a screenshot if an app happens to be running; the first live look at the tab is an owner action, like 7a's drill. The override editor (D5) cannot list existing overrides because the snapshot does not expose them — carried.
