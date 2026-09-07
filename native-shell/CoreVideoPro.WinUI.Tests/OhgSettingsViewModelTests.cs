using CoreVideoPro.ShowEngine;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>Plan 7b Task 9 — <see cref="OhgSettingsViewModel"/> over a REAL
/// <see cref="ShowConfigStore"/> on a temp folder (cheap, and it pins the JSON round trip
/// end-to-end rather than mocking the one piece that matters). Constructed with plain data, no
/// <see cref="Microsoft.UI.Dispatching.DispatcherQueue"/>, same convention as every other VM test
/// in this assembly.</summary>
public sealed class OhgSettingsViewModelTests : IDisposable
{
    private readonly string _dir;

    public OhgSettingsViewModelTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cvp-ohg-settings-vm-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    private static readonly IReadOnlySet<string> Scenes = new HashSet<string>(StringComparer.Ordinal)
    {
        "scene-a", "scene-b", "scene-c", "scene-d"
    };

    private static readonly IReadOnlyList<(string Id, string Name)> SceneList =
    [
        ("scene-a", "Scene A"),
        ("scene-b", "Scene B"),
        ("scene-c", "Scene C"),
        ("scene-d", "Scene D")
    ];

    private OhgSettingsViewModel MakeViewModel(
        out List<ShowConfig> applied,
        bool engineStartedAtLaunch = true,
        Func<ShowConfig, Task<string?>>? apply = null,
        ShowConfigStore? store = null)
    {
        var appliedConfigs = new List<ShowConfig>();
        applied = appliedConfigs;

        Func<ShowConfig, Task<string?>> applyFn = apply ?? (cfg =>
        {
            appliedConfigs.Add(cfg);
            return Task.FromResult<string?>(null);
        });

        return new OhgSettingsViewModel(
            store ?? new ShowConfigStore(_dir),
            () => Scenes,
            () => SceneList,
            applyFn,
            engineStartedAtLaunch);
    }

    private static void AssignAllPresetsAndLookScenes(OhgSettingsViewModel vm)
    {
        vm.Model.PresetSolo = "scene-a";
        vm.Model.PresetActiveSpeaker = "scene-b";
        vm.Model.PresetBlack = "scene-c";
        vm.Model.PresetGallery = "scene-d";
        foreach (var look in vm.Model.Looks)
        {
            look.ScenePreset = "scene-a";
        }
    }

    [Fact]
    public void Default_ValidatesOnlyWhenPresetsAreSetAndDriveHostFalse()
    {
        var vm = MakeViewModel(out _);

        // Fresh Default(): every look scenePreset is empty -> the engine-level look check fails.
        Assert.NotNull(vm.Validate());

        AssignAllPresetsAndLookScenes(vm);
        Assert.False(vm.Model.DriveHost);

        Assert.Null(vm.Validate());
    }

    [Fact]
    public void Validate_RequiresEveryLookPresetAndAllFourShellPresetsOnlyWhenDriveHostIsOn()
    {
        var vm = MakeViewModel(out _);
        // Give the engine-level look check something valid to pass, but leave the shell
        // presets unset - DriveHost=false should tolerate that.
        foreach (var look in vm.Model.Looks)
        {
            look.ScenePreset = "scene-a";
        }
        Assert.Null(vm.Validate());

        vm.Model.DriveHost = true;
        var problem = vm.Validate();

        Assert.NotNull(problem);
        Assert.Contains("preset", problem, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Validate_CatchesDuplicateLookIds()
    {
        var vm = MakeViewModel(out _);
        AssignAllPresetsAndLookScenes(vm);
        vm.Model.Looks[1].Id = vm.Model.Looks[0].Id;

        var problem = vm.Validate();

        Assert.NotNull(problem);
        Assert.Contains("Duplicate look id", problem);
    }

    [Fact]
    public void Validate_CatchesAPresetNamingAMissingScene()
    {
        var vm = MakeViewModel(out _);
        AssignAllPresetsAndLookScenes(vm);
        vm.Model.PresetBlack = "scene-does-not-exist";

        var problem = vm.Validate();

        Assert.NotNull(problem);
        Assert.Contains("scene-does-not-exist", problem);
    }

    [Fact]
    public void Validate_CatchesCapacityMismatch()
    {
        var vm = MakeViewModel(out _);
        AssignAllPresetsAndLookScenes(vm);
        vm.Model.Capacity = 4;

        var problem = vm.Validate();

        Assert.NotNull(problem);
        Assert.Contains("capacity", problem);
    }

    [Fact]
    public void Validate_CatchesALookWithMoreThanFourBoxes()
    {
        var vm = MakeViewModel(out _);
        AssignAllPresetsAndLookScenes(vm);
        vm.Model.Looks[0].Boxes = 5;

        var problem = vm.Validate();

        Assert.Equal($"look '{vm.Model.Looks[0].Id}': boxes must be 0..4", problem);
    }

    [Fact]
    public async Task SaveAsync_RefusesALookWithMoreThanFourBoxesAndNeverSaves()
    {
        var vm = MakeViewModel(out var applied);
        AssignAllPresetsAndLookScenes(vm);
        vm.Model.Looks[0].Boxes = 5;

        await vm.SaveCommand.ExecuteAsync(null);

        Assert.Empty(applied);
        Assert.NotEmpty(vm.ValidationMessage);
        Assert.False(new ShowConfigStore(_dir).Exists);
    }

    [Fact]
    public void Validate_RequiresMukanaBaseUrlAndEventWhenAnyIntegrationIsEnabled()
    {
        var vm = MakeViewModel(out _);
        AssignAllPresetsAndLookScenes(vm);
        vm.Model.HandsQueueEnabled = true;
        // MukanaBaseUrl/MukanaEvent left unset — this is exactly what ToConfig() would otherwise
        // silently turn into baseUrl:"" / event:"", which the engine's parseShowEngineConfig
        // rejects at startup (terminal exit 78).

        var problem = vm.Validate();

        Assert.Equal("Mukana base URL and event are required when an integration is enabled", problem);
    }

    [Fact]
    public void Validate_RejectsAMukanaBaseUrlWithoutAnHttpScheme()
    {
        var vm = MakeViewModel(out _);
        AssignAllPresetsAndLookScenes(vm);
        vm.Model.RegistryEnabled = true;
        vm.Model.MukanaBaseUrl = "not-a-url";
        vm.Model.MukanaEvent = "officehours";

        var problem = vm.Validate();

        Assert.Equal("Mukana base URL and event are required when an integration is enabled", problem);
    }

    [Fact]
    public void Validate_RequiresPositiveMukanaIntervalsWhenAnyIntegrationIsEnabled()
    {
        var vm = MakeViewModel(out _);
        AssignAllPresetsAndLookScenes(vm);
        vm.Model.QuestionFeedEnabled = true;
        vm.Model.MukanaBaseUrl = "https://host/php-panel-rest.php";
        vm.Model.MukanaEvent = "officehours";
        vm.Model.MaxBackoffMs = 0;

        var problem = vm.Validate();

        Assert.Equal("Mukana interval settings must be positive when an integration is enabled", problem);
    }

    [Fact]
    public async Task SaveAsync_ARefusedMukanaConfigIsNeverSaved()
    {
        var vm = MakeViewModel(out var applied);
        AssignAllPresetsAndLookScenes(vm);
        vm.Model.RegistryEnabled = true;

        await vm.SaveCommand.ExecuteAsync(null);

        Assert.Empty(applied);
        Assert.NotEmpty(vm.ValidationMessage);
        Assert.False(new ShowConfigStore(_dir).Exists);
    }

    [Fact]
    public async Task SaveAsync_AValidEnabledIntegrationConfigSaves()
    {
        var vm = MakeViewModel(out var applied);
        AssignAllPresetsAndLookScenes(vm);
        vm.Model.RegistryEnabled = true;
        vm.Model.MukanaBaseUrl = "https://host/php-panel-rest.php";
        vm.Model.MukanaEvent = "officehours";

        await vm.SaveCommand.ExecuteAsync(null);

        Assert.Empty(vm.ValidationMessage);
        Assert.Single(applied);
        Assert.True(new ShowConfigStore(_dir).Exists);
    }

    [Fact]
    public void Constructor_ACorruptExistingConfigIsReportedNotSilentlyReplaced()
    {
        var store = new ShowConfigStore(_dir);
        File.WriteAllText(store.FilePath, "{ this is not valid json");

        var vm = MakeViewModel(out _, store: store);

        Assert.True(vm.LoadedFromDefaultsBecauseOfError);
        Assert.StartsWith("Existing config could not be loaded:", vm.ValidationMessage);
        Assert.Contains("Saving will overwrite it.", vm.ValidationMessage);
        // Falls back to Default() so the VM is still usable.
        Assert.Equal(10, vm.Model.Capacity);
        Assert.Equal(4, vm.Model.Looks.Count);
    }

    [Fact]
    public void Constructor_ALoadableExistingConfigDoesNotSetTheLoadErrorFlag()
    {
        var store = new ShowConfigStore(_dir);
        store.Save(OhgConfigEditModel.Default().ToConfig());

        var vm = MakeViewModel(out _, store: store);

        Assert.False(vm.LoadedFromDefaultsBecauseOfError);
        Assert.Empty(vm.ValidationMessage);
    }

    [Fact]
    public async Task SaveAsync_DoesNotCallApplyOnAValidationError()
    {
        var vm = MakeViewModel(out var applied);
        // Fresh Default() is invalid (empty look scene presets).

        await vm.SaveCommand.ExecuteAsync(null);

        Assert.Empty(applied);
        Assert.NotEmpty(vm.ValidationMessage);
        var store = new ShowConfigStore(_dir);
        Assert.False(store.Exists);
    }

    [Fact]
    public async Task SaveAsync_FirstTimeSaveSetsNeedsAppRestart()
    {
        var vm = MakeViewModel(out var applied, engineStartedAtLaunch: false);
        AssignAllPresetsAndLookScenes(vm);

        await vm.SaveCommand.ExecuteAsync(null);

        Assert.Empty(vm.ValidationMessage);
        Assert.Single(applied);
        Assert.True(vm.NeedsAppRestart);
        Assert.Equal("Saved. Restart CoreVideo Pro to start the show engine.", vm.SaveStatus);

        var store = new ShowConfigStore(_dir);
        Assert.True(store.Exists);
    }

    [Fact]
    public async Task SaveAsync_WhenEngineAlreadyRunningReportsSavedAndApplied()
    {
        var vm = MakeViewModel(out var applied, engineStartedAtLaunch: true);
        AssignAllPresetsAndLookScenes(vm);

        await vm.SaveCommand.ExecuteAsync(null);

        Assert.Single(applied);
        Assert.False(vm.NeedsAppRestart);
        Assert.Equal("Saved and applied", vm.SaveStatus);
    }

    [Fact]
    public async Task SaveAsync_SurfacesAnApplyErrorInSaveStatus()
    {
        var vm = MakeViewModel(out _, apply: _ => Task.FromResult<string?>("engine refused: bad look"));
        AssignAllPresetsAndLookScenes(vm);

        await vm.SaveCommand.ExecuteAsync(null);

        Assert.Equal("engine refused: bad look", vm.SaveStatus);
    }

    [Fact]
    public async Task SaveAsync_CallsApplyOnlyAfterStoreSave()
    {
        var store = new ShowConfigStore(_dir);
        var vm = MakeViewModel(
            out _,
            store: store,
            apply: _ =>
            {
                Assert.True(store.Exists);
                return Task.FromResult<string?>(null);
            });
        AssignAllPresetsAndLookScenes(vm);

        await vm.SaveCommand.ExecuteAsync(null);

        Assert.True(store.Exists);
    }

    [Fact]
    public void AddLook_AddsToBothTheCollectionAndTheModel()
    {
        var vm = MakeViewModel(out _);
        var before = vm.Model.Looks.Count;

        vm.AddLookCommand.Execute(null);

        Assert.Equal(before + 1, vm.Model.Looks.Count);
        Assert.Equal(before + 1, vm.Looks.Count);
        Assert.Same(vm.Model.Looks[^1], vm.Looks[^1].Edit);
    }

    [Fact]
    public void RemoveLook_RemovesFromBothTheCollectionAndTheModel()
    {
        var vm = MakeViewModel(out _);
        var before = vm.Model.Looks.Count;
        var target = vm.Looks[0];
        var targetEdit = target.Edit;

        vm.RemoveLookCommand.Execute(target);

        Assert.Equal(before - 1, vm.Model.Looks.Count);
        Assert.Equal(before - 1, vm.Looks.Count);
        Assert.DoesNotContain(targetEdit, vm.Model.Looks);
    }

    [Fact]
    public void LookEditorViewModel_WritesThroughToTheModel()
    {
        var vm = MakeViewModel(out _);
        var row = vm.Looks[0];

        row.ScenePreset = "scene-c";
        row.Boxes = 7;

        Assert.Equal("scene-c", vm.Model.Looks[0].ScenePreset);
        Assert.Equal(7, vm.Model.Looks[0].Boxes);
    }

    [Fact]
    public async Task ImportLegacyAsync_IsAStubForTask11()
    {
        var vm = MakeViewModel(out _);

        await vm.ImportLegacyCommand.ExecuteAsync(null);

        Assert.Equal("Import is not wired yet (Task 11)", vm.SaveStatus);
    }

    [Fact]
    public void Constructor_LoadsExistingConfigWhenPresent()
    {
        var store = new ShowConfigStore(_dir);
        var seed = OhgConfigEditModel.Default();
        seed.GalleryCells = 42;
        store.Save(seed.ToConfig());

        var vm = MakeViewModel(out _, store: store);

        Assert.Equal(42, vm.Model.GalleryCells);
    }

    [Fact]
    public void Constructor_UsesDefaultWhenNoConfigExists()
    {
        var vm = MakeViewModel(out _);

        Assert.Equal(10, vm.Model.Capacity);
        Assert.Equal(4, vm.Model.Looks.Count);
    }

    [Fact]
    public void SceneChoices_ComesFromScenesAtConstructionAndRefreshCommand()
    {
        var callCount = 0;
        IReadOnlyList<(string Id, string Name)> ScenesList()
        {
            callCount++;
            return callCount == 1 ? SceneList : [("scene-e", "Scene E")];
        }

        var vm = new OhgSettingsViewModel(
            new ShowConfigStore(_dir),
            () => Scenes,
            ScenesList,
            _ => Task.FromResult<string?>(null),
            true);

        Assert.Equal(SceneList, vm.SceneChoices);

        vm.RefreshScenesCommand.Execute(null);

        Assert.Equal([("scene-e", "Scene E")], vm.SceneChoices);
    }

    // -- Task 10 fix round 1: an emptied NumberBox must not silently change the look --

    [Fact]
    public void BoxesValue_KeepsThePreviousCountWhenTheNumberBoxIsEmptied()
    {
        var edit = new OhgLookEdit { Id = "banter", Label = "Banter", Boxes = 2 };
        var look = new OhgLookEditorViewModel(edit);

        look.BoxesValue = double.NaN;

        Assert.Equal(2, look.Boxes);
        Assert.Equal(2, edit.Boxes);
    }

    [Fact]
    public void BoxesValue_RoundsAndWritesThroughToTheEditModel()
    {
        var edit = new OhgLookEdit { Id = "banter", Label = "Banter", Boxes = 2 };
        var look = new OhgLookEditorViewModel(edit);

        look.BoxesValue = 3.0;

        Assert.Equal(3, edit.Boxes);
        Assert.Equal(3.0, look.BoxesValue);
    }
}
