using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.ShowEngine;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>Settings-page VM for the OHG show config editor (Plan 7b Task 9). Owns an
/// <see cref="OhgConfigEditModel"/> (loaded from <paramref name="store"/> if a config already
/// exists, else <see cref="OhgConfigEditModel.Default"/>) and validates/saves it through the
/// same <see cref="ShowConfigStore"/>/<see cref="ShowConfigValidator"/> the rest of Plan 7
/// uses.</summary>
public sealed partial class OhgSettingsViewModel : ObservableObject
{
    private readonly ShowConfigStore _store;
    private readonly Func<IReadOnlySet<string>> _sceneIds;
    private readonly Func<IReadOnlyList<(string Id, string Name)>> _scenes;
    private readonly Func<ShowConfig, Task<string?>> _apply;
    private readonly bool _engineStartedAtLaunch;

    public OhgSettingsViewModel(
        ShowConfigStore store,
        Func<IReadOnlySet<string>> sceneIds,
        Func<IReadOnlyList<(string Id, string Name)>> scenes,
        Func<ShowConfig, Task<string?>> apply,
        bool engineStartedAtLaunch)
    {
        _store = store;
        _sceneIds = sceneIds;
        _scenes = scenes;
        _apply = apply;
        _engineStartedAtLaunch = engineStartedAtLaunch;

        ShowConfig? loaded = null;
        string? loadError = null;
        if (store.Exists)
        {
            loaded = store.Load(out loadError);
        }

        if (loaded is not null)
        {
            Model = OhgConfigEditModel.FromConfig(loaded, out _);
            validationMessage = "";
        }
        else
        {
            Model = OhgConfigEditModel.Default();
            if (loadError is not null)
            {
                // The saved config exists but couldn't be parsed (corrupt file, bad JSON,
                // unsupported version, etc.) — silently falling back to Default() would let an
                // operator overwrite a broken-but-recoverable file without ever knowing it was
                // broken. Surface the error and flag it so the UI can warn loudly.
                loadedFromDefaultsBecauseOfError = true;
                validationMessage = $"Existing config could not be loaded: {loadError}. Saving will overwrite it.";
            }
            else
            {
                validationMessage = "";
            }
        }

        Looks = new ObservableCollection<OhgLookEditorViewModel>(Model.Looks.Select(look => new OhgLookEditorViewModel(look)));
        SceneChoices = _scenes();

        saveStatus = "";
    }

    /// <summary>The bound edit model. Its scalar fields are bound directly by the settings page;
    /// <see cref="Looks"/> mirrors <see cref="OhgConfigEditModel.Looks"/> for the repeater.</summary>
    public OhgConfigEditModel Model { get; }

    public ObservableCollection<OhgLookEditorViewModel> Looks { get; }

    public IReadOnlyList<(string Id, string Name)> SceneChoices { get; private set; }

    [ObservableProperty] private string validationMessage;
    [ObservableProperty] private string saveStatus;
    [ObservableProperty] private bool needsAppRestart;
    [ObservableProperty] private bool loadedFromDefaultsBecauseOfError;

    [RelayCommand]
    private void RefreshScenes()
    {
        SceneChoices = _scenes();
        OnPropertyChanged(nameof(SceneChoices));
    }

    [RelayCommand]
    private void AddLook()
    {
        var edit = new OhgLookEdit { Id = "", Label = "" };
        Model.Looks.Add(edit);
        Looks.Add(new OhgLookEditorViewModel(edit));
    }

    [RelayCommand]
    private void RemoveLook(OhgLookEditorViewModel look)
    {
        Model.Looks.Remove(look.Edit);
        Looks.Remove(look);
    }

    [RelayCommand]
    private async Task SaveAsync()
    {
        var problem = Validate();
        if (problem is not null)
        {
            ValidationMessage = problem;
            return;
        }

        ValidationMessage = "";

        var config = Model.ToConfig();
        _store.Save(config);

        var error = await _apply(config);

        SaveStatus = error ?? (_engineStartedAtLaunch
            ? "Saved and applied"
            : "Saved. Restart CoreVideo Pro to start the show engine.");
        NeedsAppRestart = !_engineStartedAtLaunch;
    }

    /// <summary>Stub for Plan 7b Task 11, which replaces this with the real legacy-Isadora
    /// import.</summary>
    [RelayCommand]
    private Task ImportLegacyAsync()
    {
        SaveStatus = "Import is not wired yet (Task 11)";
        return Task.CompletedTask;
    }

    /// <summary>Every <c>engine</c> look's box count must fit the engine's fixed layout
    /// (<c>MAX_LOOK_BOXES</c> in <c>show-engine/src/config.ts</c>) — the engine parser would
    /// otherwise reject the config outright (exit 78, terminal, no respawn).</summary>
    private const int MaxLookBoxes = 4;

    /// <summary>Local checks first (look ids non-empty and unique; boxes in 0..4; every look has
    /// a scene preset and all four shell presets are set WHEN <see cref="OhgConfigEditModel.DriveHost"/>
    /// is on — shadow mode can be saved half-configured; Mukana's required fields whenever any
    /// integration flag is on — <c>ToConfig()</c> would otherwise emit an empty <c>baseUrl</c>/
    /// <c>event</c> or a non-positive interval, which <c>parseShowEngineConfig</c> refuses at
    /// startup with a TERMINAL exit code, never restarted), then the shared
    /// <see cref="ShowConfigValidator"/> over the built <see cref="ShowConfig"/>.</summary>
    public string? Validate()
    {
        var seenIds = new HashSet<string>(StringComparer.Ordinal);
        foreach (var look in Model.Looks)
        {
            if (string.IsNullOrEmpty(look.Id))
            {
                return "Every look needs an id";
            }
            if (!seenIds.Add(look.Id))
            {
                return $"Duplicate look id '{look.Id}'";
            }
            if (look.Boxes < 0 || look.Boxes > MaxLookBoxes)
            {
                return $"look '{look.Id}': boxes must be 0..4";
            }
        }

        if (Model.DriveHost)
        {
            foreach (var look in Model.Looks)
            {
                if (string.IsNullOrEmpty(look.ScenePreset))
                {
                    return $"Look '{look.Id}' needs a scene preset before Drive Host can be enabled";
                }
            }

            if (string.IsNullOrEmpty(Model.PresetSolo) ||
                string.IsNullOrEmpty(Model.PresetActiveSpeaker) ||
                string.IsNullOrEmpty(Model.PresetBlack) ||
                string.IsNullOrEmpty(Model.PresetGallery))
            {
                return "All four presets (solo, active speaker, black, gallery) must be set before Drive Host can be enabled";
            }
        }

        if (Model.RegistryEnabled || Model.HandsQueueEnabled || Model.QuestionFeedEnabled)
        {
            if (string.IsNullOrWhiteSpace(Model.MukanaBaseUrl) ||
                !(Model.MukanaBaseUrl.StartsWith("http://", StringComparison.OrdinalIgnoreCase) ||
                  Model.MukanaBaseUrl.StartsWith("https://", StringComparison.OrdinalIgnoreCase)) ||
                string.IsNullOrWhiteSpace(Model.MukanaEvent))
            {
                return "Mukana base URL and event are required when an integration is enabled";
            }

            if (Model.PanelistsIntervalMs <= 0 ||
                Model.HandsIntervalMs <= 0 ||
                Model.QuestionIntervalMs <= 0 ||
                Model.MaxBackoffMs <= 0)
            {
                return "Mukana interval settings must be positive when an integration is enabled";
            }
        }

        return ShowConfigValidator.Validate(Model.ToConfig(), _sceneIds());
    }
}
