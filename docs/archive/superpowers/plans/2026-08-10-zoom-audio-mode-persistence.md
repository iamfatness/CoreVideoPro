# Zoom Audio Mode Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist the operator's Zoom→program audio topology (`ProgramMix` / `PerGuestIso`) across launches, so a show that runs in per-guest ISO comes back up that way.

**Architecture:** A pure static parse/format helper converts between the `ZoomAudioMode` enum and its persisted string form. `ProductionOutputPreferences` gains a nullable string field at schema v9. `StudioViewModel` captures the mode when saving prefs and restores it by writing the **backing field** (never the setter, which would sync a core that is not up yet). No core, protocol, or snapshot change — this is shell-side end to end.

**Tech Stack:** C# / .NET 9, WinUI 3, xUnit, `System.Text.Json`.

**Spec:** `docs/superpowers/specs/2026-08-10-zoom-audio-mode-persistence-design.md`

## Global Constraints

- Persisted values are exactly `"programMix"` and `"perGuestIso"`. Any unrecognized, empty, or null value parses to `ZoomAudioMode.ProgramMix` — never throws.
- `ProductionOutputPreferences.CurrentVersion` goes **8 → 9**. No explicit migration branch is added; the existing `Version < CurrentVersion` bump covers it and an absent field deserializes to `null` → `ProgramMix`.
- The `Version < 3` local-audio-capture migration stays pinned at `< 3`. Do not touch it.
- Restore writes the backing field `_zoomAudioMode` directly. **Never** call `SetZoomAudioMode` from `ApplyProductionOutputPreferences` — it calls `TrySyncMediaCoreAsync()` against a core that is not running at restore time.
- `StudioViewModel` is **not constructible in tests** (field-init `DispatcherQueue.GetForCurrentThread()`, ctor hard-`new()`s ~10 services, launches the core). All new tests must target pure statics or the serializer. Do not write a test that constructs the view model.
- The field is not secret-bearing: no DPAPI treatment, no support-bundle redaction test.
- Run the full WinUI suite before the final commit of each task: it must stay at **678 pass / 0 fail** plus whatever this plan adds.

## Correction to the spec's test section

The spec listed three tests under `StudioViewModelAudioStatusTests` that require a constructed `StudioViewModel` ("restore writes the backing field and does not trigger a core sync"; "restore raises PropertyChanged and sets CommandStatus"; "SetZoomAudioMode persists"). **Those are not achievable** — that test class only exercises static methods, for the reason in Global Constraints.

This plan covers the same risk differently: the parse/format decision (the part with real branching, including the safe fallback) is extracted into a pure helper and tested directly, which is the established house pattern for logic trapped in the view model (`IsoSourceSelectionResolver`, `NativeUvcCapturePolicy`, `TransportStatusFormatter`). The remaining view-model wiring is three assignments with no branching, verified by the compiler and by the existing prefs round-trip. Task 3 says so out loud rather than implying coverage that does not exist.

## File Structure

| File | Responsibility |
|---|---|
| `native-shell/CoreVideoPro.WinUI/Services/ZoomAudioModePreference.cs` | **New.** Pure parse/format between `ZoomAudioMode` and its persisted string. The only place the wire strings are spelled. |
| `native-shell/CoreVideoPro.WinUI/Services/ProductionOutputPreferencesStore.cs` | **Modify.** Add `ZoomAudioMode` string field; bump `CurrentVersion` 8 → 9; add the v9 doc comment. |
| `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs` | **Modify.** Capture on save, restore on load, persist on toggle. |
| `native-shell/CoreVideoPro.WinUI.Tests/ZoomAudioModePreferenceTests.cs` | **New.** Parse/format tests incl. the fallback. |
| `native-shell/CoreVideoPro.WinUI.Tests/ProductionOutputPreferencesStoreTests.cs` | **Modify.** Round-trip + v8→v9 migration tests; fix two existing `CurrentVersion == 8` assertions. |
| `CLAUDE.md` | **Modify.** One line in the v9 prefs lineage. |

---

### Task 1: The pure parse/format helper

**Files:**
- Create: `native-shell/CoreVideoPro.WinUI/Services/ZoomAudioModePreference.cs`
- Test: `native-shell/CoreVideoPro.WinUI.Tests/ZoomAudioModePreferenceTests.cs`

**Interfaces:**
- Consumes: `ZoomAudioMode` enum (`CoreVideoPro.WinUI.Models`, `Models/ProductionModels.cs:139`).
- Produces: `ZoomAudioModePreference.Parse(string?) -> ZoomAudioMode`, `ZoomAudioModePreference.Format(ZoomAudioMode) -> string`, and the constants `ZoomAudioModePreference.ProgramMixValue` / `PerGuestIsoValue` (both `string`). Tasks 2 and 3 use these names exactly.

- [ ] **Step 1: Write the failing tests**

Create `native-shell/CoreVideoPro.WinUI.Tests/ZoomAudioModePreferenceTests.cs`:

```csharp
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ZoomAudioModePreferenceTests
{
    [Fact]
    public void Parse_ReadsBothPersistedModes()
    {
        Assert.Equal(ZoomAudioMode.ProgramMix, ZoomAudioModePreference.Parse("programMix"));
        Assert.Equal(ZoomAudioMode.PerGuestIso, ZoomAudioModePreference.Parse("perGuestIso"));
    }

    [Fact]
    public void Parse_IsCaseInsensitiveAndTrims()
    {
        // Hand-edited prefs files are a supported reality; casing is not a contract.
        Assert.Equal(ZoomAudioMode.PerGuestIso, ZoomAudioModePreference.Parse("perguestiso"));
        Assert.Equal(ZoomAudioMode.PerGuestIso, ZoomAudioModePreference.Parse("PERGUESTISO"));
        Assert.Equal(ZoomAudioMode.PerGuestIso, ZoomAudioModePreference.Parse("  perGuestIso  "));
    }

    [Fact]
    public void Parse_FallsBackToProgramMixForAnythingUnrecognized()
    {
        // THE safety property: an absent (v8 file), empty, or corrupted value must
        // land on the long-standing Z1 topology, never throw and never guess ISO.
        Assert.Equal(ZoomAudioMode.ProgramMix, ZoomAudioModePreference.Parse(null));
        Assert.Equal(ZoomAudioMode.ProgramMix, ZoomAudioModePreference.Parse(""));
        Assert.Equal(ZoomAudioMode.ProgramMix, ZoomAudioModePreference.Parse("   "));
        Assert.Equal(ZoomAudioMode.ProgramMix, ZoomAudioModePreference.Parse("chaos"));
    }

    [Fact]
    public void Format_EmitsTheDocumentedWireStrings()
    {
        Assert.Equal("programMix", ZoomAudioModePreference.Format(ZoomAudioMode.ProgramMix));
        Assert.Equal("perGuestIso", ZoomAudioModePreference.Format(ZoomAudioMode.PerGuestIso));
    }

    [Fact]
    public void FormatThenParse_RoundTripsEveryMode()
    {
        foreach (var mode in new[] { ZoomAudioMode.ProgramMix, ZoomAudioMode.PerGuestIso })
        {
            Assert.Equal(mode, ZoomAudioModePreference.Parse(ZoomAudioModePreference.Format(mode)));
        }
    }
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run:
```powershell
dotnet test native-shell\CoreVideoPro.WinUI.Tests\CoreVideoPro.WinUI.Tests.csproj --filter "FullyQualifiedName~ZoomAudioModePreferenceTests"
```
Expected: FAIL — build error, `ZoomAudioModePreference` does not exist in `CoreVideoPro.WinUI.Services`.

- [ ] **Step 3: Write the implementation**

Create `native-shell/CoreVideoPro.WinUI/Services/ZoomAudioModePreference.cs`:

```csharp
using System;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Parse/format for the PERSISTED Zoom→program audio topology (prefs v9,
/// docs/superpowers/specs/2026-08-10-zoom-audio-mode-persistence-design.md).
///
/// Stored as a string rather than a bool because <see cref="ZoomAudioMode"/> is
/// already an enum, a third topology is plausible (mix + selected stems), and a
/// string can express "unrecognized" — which is the safety property here:
/// anything this cannot read becomes <see cref="ZoomAudioMode.ProgramMix"/>, the
/// long-standing Z1 topology, rather than silently putting a show on air with
/// zoom-mix stripped off the program buses.
/// </summary>
public static class ZoomAudioModePreference
{
    public const string ProgramMixValue = "programMix";
    public const string PerGuestIsoValue = "perGuestIso";

    public static ZoomAudioMode Parse(string? persisted) =>
        string.Equals(persisted?.Trim(), PerGuestIsoValue, StringComparison.OrdinalIgnoreCase)
            ? ZoomAudioMode.PerGuestIso
            : ZoomAudioMode.ProgramMix;

    public static string Format(ZoomAudioMode mode) =>
        mode == ZoomAudioMode.PerGuestIso ? PerGuestIsoValue : ProgramMixValue;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:
```powershell
dotnet test native-shell\CoreVideoPro.WinUI.Tests\CoreVideoPro.WinUI.Tests.csproj --filter "FullyQualifiedName~ZoomAudioModePreferenceTests"
```
Expected: PASS — 5 passed, 0 failed.

- [ ] **Step 5: Commit**

```bash
git add native-shell/CoreVideoPro.WinUI/Services/ZoomAudioModePreference.cs native-shell/CoreVideoPro.WinUI.Tests/ZoomAudioModePreferenceTests.cs
git commit -m "feat(audio): pure parse/format for the persisted Zoom audio mode

Unrecognized/absent/corrupted values fall back to ProgramMix - a persisted
topology must never put a show on air with zoom-mix stripped off the program
buses because a string did not match."
```

---

### Task 2: The prefs field and the v9 schema bump

**Files:**
- Modify: `native-shell/CoreVideoPro.WinUI/Services/ProductionOutputPreferencesStore.cs:27-33` (doc comment + `CurrentVersion`), and the field list near `:119-126`
- Modify: `native-shell/CoreVideoPro.WinUI.Tests/ProductionOutputPreferencesStoreTests.cs:197` and `:237` (existing assertions that hard-code 8)
- Test: `native-shell/CoreVideoPro.WinUI.Tests/ProductionOutputPreferencesStoreTests.cs` (two new tests)

**Interfaces:**
- Consumes: `ZoomAudioModePreference.ProgramMixValue` / `PerGuestIsoValue` from Task 1.
- Produces: `ProductionOutputPreferences.ZoomAudioMode` (type `string?`, default `null`) and `ProductionOutputPreferences.CurrentVersion == 9`. Task 3 reads and writes this property.

- [ ] **Step 1: Write the failing tests**

Add to `native-shell/CoreVideoPro.WinUI.Tests/ProductionOutputPreferencesStoreTests.cs` (append inside the class, after `Serializer_MigratesV7FileToV8WithProgramOnlyIsoDefaults`):

```csharp
    [Fact]
    public void Serializer_RoundTripsZoomAudioMode()
    {
        // v9: the Zoom→program topology persists so a show that runs in per-guest
        // ISO comes back up that way.
        var preferences = new ProductionOutputPreferences
        {
            ZoomAudioMode = ZoomAudioModePreference.PerGuestIsoValue
        };

        var roundTripped = ProductionOutputPreferencesSerializer.Deserialize(
            ProductionOutputPreferencesSerializer.Serialize(preferences));

        Assert.NotNull(roundTripped);
        Assert.Equal(ZoomAudioModePreference.PerGuestIsoValue, roundTripped.ZoomAudioMode);
        Assert.Equal(ZoomAudioMode.PerGuestIso, ZoomAudioModePreference.Parse(roundTripped.ZoomAudioMode));
    }

    [Fact]
    public void Serializer_MigratesV8FileToV9WithProgramMixDefault()
    {
        // A v8 file has no ZoomAudioMode field: it migrates to v9 reading as the
        // long-standing Z1 program-mix topology — behaviour identical to today for
        // every existing profile.
        const string json = """
            {
              "Version": 8,
              "VirtualCameraEnabled": true
            }
            """;

        var migrated = ProductionOutputPreferencesSerializer.Deserialize(json, out var wasMigrated);

        Assert.NotNull(migrated);
        Assert.True(wasMigrated);
        Assert.Equal(9, ProductionOutputPreferences.CurrentVersion);
        Assert.Equal(ProductionOutputPreferences.CurrentVersion, migrated.Version);
        Assert.Null(migrated.ZoomAudioMode);
        Assert.Equal(ZoomAudioMode.ProgramMix, ZoomAudioModePreference.Parse(migrated.ZoomAudioMode));
        Assert.True(migrated.VirtualCameraEnabled);  // untouched fields survive
    }
```

The test file's existing usings are `CoreVideoPro.WinUI.Services` and `Xunit`. `ZoomAudioMode` lives in `CoreVideoPro.WinUI.Models`, so add that using at the top of the file:

```csharp
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;
```

- [ ] **Step 2: Run the tests to verify they fail**

Run:
```powershell
dotnet test native-shell\CoreVideoPro.WinUI.Tests\CoreVideoPro.WinUI.Tests.csproj --filter "FullyQualifiedName~ProductionOutputPreferencesStoreTests"
```
Expected: FAIL — build error, `ProductionOutputPreferences` has no `ZoomAudioMode` property.

- [ ] **Step 3: Add the field and bump the version**

In `native-shell/CoreVideoPro.WinUI/Services/ProductionOutputPreferencesStore.cs`, add this doc-comment block immediately after the existing v8 block (which ends `// secret-bearing (source ids are per-meeting handles, not credentials).`) and change the version constant on the following line:

```csharp
    // v9 (2026-08-10, follow-up to PR #397): the Zoom→program audio topology
    // persists — "programMix" (Zoom's own echo-cancelled mix rides the program
    // buses; the long-standing Z1 default) or "perGuestIso" (each guest's stem
    // routes through their own strip and zoom-mix leaves the program buses).
    // Older files migrate with the field absent = programMix, so every existing
    // profile behaves exactly as it did before the upgrade. Stored as a string,
    // not a bool: an unrecognized value falls back to programMix (see
    // ZoomAudioModePreference). Not secret-bearing.
    public const int CurrentVersion = 9;
```

Then add the property itself, immediately after `IsoRecordingSourceIds` (currently line 126):

```csharp
    // v9: the operator's Zoom→program audio topology. Null/absent/unrecognized =
    // "programMix". Read and written ONLY through ZoomAudioModePreference so the
    // wire strings are spelled in exactly one place.
    public string? ZoomAudioMode { get; set; }
```

- [ ] **Step 4: Fix the two existing tests that pin the old version**

Two existing tests assert the version literal and will now fail. In `ProductionOutputPreferencesStoreTests.cs`:

- Line ~197, in `Serializer_MigratesOlderSchemaToV6WithEmptyVstStates`: change `Assert.Equal(8, ProductionOutputPreferences.CurrentVersion);` to `Assert.Equal(9, ProductionOutputPreferences.CurrentVersion);`
- Line ~237, in `Serializer_MigratesV7FileToV8WithProgramOnlyIsoDefaults`: change `Assert.Equal(8, ProductionOutputPreferences.CurrentVersion);` to `Assert.Equal(9, ProductionOutputPreferences.CurrentVersion);`

Leave both test *names* alone — they describe which schema bump the fixture exercises (a v5 file, a v7 file), not the current version number, and renaming them loses that history.

- [ ] **Step 5: Run the full suite to verify everything passes**

Run:
```powershell
dotnet test native-shell\CoreVideoPro.WinUI.Tests\CoreVideoPro.WinUI.Tests.csproj
```
Expected: PASS — 685 passed, 0 failed (678 baseline + 5 from Task 1 + 2 here).

- [ ] **Step 6: Commit**

```bash
git add native-shell/CoreVideoPro.WinUI/Services/ProductionOutputPreferencesStore.cs native-shell/CoreVideoPro.WinUI.Tests/ProductionOutputPreferencesStoreTests.cs
git commit -m "feat(prefs): persist the Zoom audio mode - schema v8 -> v9

No explicit migration branch: the existing Version < CurrentVersion bump covers
it and an absent field reads as programMix, so every existing profile behaves
exactly as it did before. Also updates the two tests that pinned the literal 8."
```

---

### Task 3: Wire capture, restore, and save into StudioViewModel

**Files:**
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs:795-810` (`SetZoomAudioMode` — add the save), `:11150-11153` (capture into the prefs object), `:11299` (restore, inserted after `RefreshIsoReadouts();`)
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: `ZoomAudioModePreference.Parse` / `.Format` (Task 1); `ProductionOutputPreferences.ZoomAudioMode` (Task 2).
- Produces: no new public surface. `_zoomAudioMode` is restored before the first full sync, so the existing `EnsureDefaultZoomAudioRoutingSends` seeder picks it up with no further change.

**Test note — read before starting.** There is no unit test in this task, and that is deliberate, not an omission. `StudioViewModel` cannot be constructed in tests (see Global Constraints), so a test here would require a DI-seam refactor far outside this plan's scope. The branching logic is already covered by Task 1; what remains is three assignments. Verify by build + full suite, and confirm behaviour on the rig later. Do **not** invent a test that constructs the view model — it will not work.

- [ ] **Step 1: Persist on toggle**

In `SetZoomAudioMode` (line ~795), add the save call after the status assignment and before the sync. The method already early-returns when the mode is unchanged, so a no-op toggle will not rewrite the file. The result:

```csharp
    public void SetZoomAudioMode(bool perGuestIso)
    {
        var mode = perGuestIso ? ZoomAudioMode.PerGuestIso : ZoomAudioMode.ProgramMix;
        if (mode == _zoomAudioMode)
        {
            return;
        }
        _zoomAudioMode = mode;
        OnPropertyChanged(nameof(ZoomAudioMode));
        OnPropertyChanged(nameof(IsPerGuestIsoAudio));
        CommandStatus = perGuestIso
            ? "Per-guest ISO audio: each guest routes to program through their own fader (Zoom's combined mix is off program)."
            : "Zoom program mix: Zoom's combined, echo-cancelled mix routes to program; guest strips meter only.";
        // v9: the topology is operator intent — it survives the launch.
        SaveProductionOutputPreferences();
        // The seeded sends change shape — push the new routing to the core now.
        _ = TrySyncMediaCoreAsync();
    }
```

- [ ] **Step 2: Capture the mode when prefs are built**

In the preferences object initializer (line ~11150), immediately after the `IsoRecordingSourceIds` entry and before `CustomScenes`, add:

```csharp
            // v9: the Zoom→program audio topology persists across launches.
            ZoomAudioMode = ZoomAudioModePreference.Format(_zoomAudioMode),
```

Note this reads the backing field `_zoomAudioMode` directly rather than the `ZoomAudioMode` property, because the property name now collides with the prefs property name inside the initializer. Reading the field is unambiguous and avoids the shadowing question entirely.

- [ ] **Step 3: Restore the mode via the backing field**

In `ApplyProductionOutputPreferences`, insert immediately after `RefreshIsoReadouts();` (line ~11299) and before `RestoreMasteringFromPreferences(preferences);`:

```csharp
        // v9: restore the Zoom→program audio topology via the BACKING field (same
        // reason as vcam/ISO above — the setter calls TrySyncMediaCoreAsync and the
        // core isn't up yet). No respawn re-arm is needed: the seeder
        // (EnsureDefaultZoomAudioRoutingSends) synthesizes its sends on EVERY
        // sync-context build, so the mode is continuously re-asserted rather than
        // sent once — a respawned core picks it up on the next sync.
        _zoomAudioMode = ZoomAudioModePreference.Parse(preferences.ZoomAudioMode);
        OnPropertyChanged(nameof(ZoomAudioMode));
        OnPropertyChanged(nameof(IsPerGuestIsoAudio));
        if (_zoomAudioMode == ZoomAudioMode.PerGuestIso)
        {
            // Loud, not silent: tell the operator which topology they came up in
            // rather than making them notice a switch position. ProgramMix says
            // nothing — it's the default, and a line every launch is noise.
            CommandStatus = "Per-guest ISO audio: each guest routes to program through their own fader (Zoom's combined mix is off program).";
        }
```

- [ ] **Step 4: Build and run the full suite**

Run:
```powershell
dotnet build native-shell\CoreVideoPro.WinUI\CoreVideoPro.WinUI.csproj -c Debug
dotnet test native-shell\CoreVideoPro.WinUI.Tests\CoreVideoPro.WinUI.Tests.csproj
```
Expected: build succeeds; tests PASS at 685 passed, 0 failed.

If the build reports `error WMC` lines, read them — the standard build-error filter `': error '` misses XamlCompiler `error WMC0011` lines. Nothing in this task touches XAML, so a WMC error means something unrelated drifted.

- [ ] **Step 5: Update CLAUDE.md**

In the ISO-4 section, the persistence paragraph currently ends with the v7/v8 lineage note ("v7→v8 migrates to program-only defaults. (v7 was the true current version…)"). Append one sentence to that same parenthetical lineage:

```
v9 (2026-08-10) persists the Zoom→program audio topology (ZoomAudioMode:
"programMix"/"perGuestIso"); absent = programMix, and an unrecognized value falls
back to programMix rather than guessing ISO.
```

- [ ] **Step 6: Commit**

```bash
git add native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs CLAUDE.md
git commit -m "feat(audio): the Zoom audio mode survives a restart

Restore writes the BACKING field (the vcam O1 / ISO-4 pattern - the setter syncs
a core that isn't up yet). Restoring per-guest ISO announces itself on the status
line; restoring program-mix stays quiet, since it's the default.

No core-respawn re-arm: the seeder synthesizes its sends on every sync-context
build, so the mode is continuously re-asserted rather than sent once."
```

---

## Self-Review

**Spec coverage.** Global scope — Task 2, a single unkeyed field. String not bool — Tasks 1 and 2. v8→v9 with no explicit migration — Task 2, Steps 3 and 4. Backing-field restore — Task 3, Step 3. No respawn re-arm — Task 3, Step 3, recorded as a comment so nobody adds it later. Save on toggle — Task 3, Step 1. Honesty at launch — Task 3, Step 3. Safety analysis — no task; it is an argument about existing code, not work to do. Testing — Tasks 1 and 2, with the spec's three impossible view-model tests replaced and the substitution declared in "Correction to the spec's test section."

**Placeholder scan.** No TBD/TODO, no "add error handling", no "similar to Task N". Every code step carries its actual content.

**Type consistency.** `ZoomAudioModePreference.Parse` / `.Format` / `.ProgramMixValue` / `.PerGuestIsoValue` are declared in Task 1 and used under exactly those names in Tasks 2 and 3. `ProductionOutputPreferences.ZoomAudioMode` is `string?` where declared (Task 2) and consumed as `string?` by `Parse` (Task 3). The prefs property and the view-model property share the name `ZoomAudioMode`; Task 3 Step 2 calls that out and reads the backing field to sidestep it.

**Two things found and fixed during review**, both from reading the real code rather than the spec:

1. Two existing tests hard-code `Assert.Equal(8, ProductionOutputPreferences.CurrentVersion)` (lines 197 and 237) and break on the bump. Task 2 Step 4 fixes them. Nothing in the spec hinted at this.
2. The spec's three view-model tests are impossible, because `StudioViewModel` is not constructible in tests. Declared and substituted above rather than left for the implementer to discover mid-task.
