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
        if (store.Exists)
        {
            loaded = store.Load(out _);
        }

        Model = loaded is not null
            ? OhgConfigEditModel.FromConfig(loaded, out _)
            : OhgConfigEditModel.Default();

        Looks = new ObservableCollection<OhgLookEditorViewModel>(Model.Looks.Select(look => new OhgLookEditorViewModel(look)));
        SceneChoices = _scenes();

        validationMessage = "";
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

    /// <summary>Local checks first (look ids non-empty and unique; boxes &gt;= 0; every look has
    /// a scene preset and all four shell presets are set WHEN <see cref="OhgConfigEditModel.DriveHost"/>
    /// is on — shadow mode can be saved half-configured), then the shared
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
            if (look.Boxes < 0)
            {
                return $"Look '{look.Id}' has a negative box count";
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

        return ShowConfigValidator.Validate(Model.ToConfig(), _sceneIds());
    }
}
