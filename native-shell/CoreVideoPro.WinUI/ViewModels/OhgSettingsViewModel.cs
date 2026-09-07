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

        // Seed the bindable mirrors from the loaded model. See the mirrors' remarks below for why
        // the settings page binds THESE and not Model.* directly.
        registryEnabled = Model.RegistryEnabled;
        handsQueueEnabled = Model.HandsQueueEnabled;
        questionFeedEnabled = Model.QuestionFeedEnabled;
        mukanaBaseUrl = Model.MukanaBaseUrl ?? "";
        mukanaEvent = Model.MukanaEvent ?? "";
        panelistsIntervalMs = Model.PanelistsIntervalMs;
        handsIntervalMs = Model.HandsIntervalMs;
        questionIntervalMs = Model.QuestionIntervalMs;
        maxBackoffMs = Model.MaxBackoffMs;
        driveHost = Model.DriveHost;
        presetSolo = Model.PresetSolo;
        presetActiveSpeaker = Model.PresetActiveSpeaker;
        presetBlack = Model.PresetBlack;
        presetGallery = Model.PresetGallery;
        defaultTransition = Model.DefaultTransition;
        tallyUrl = Model.TallyUrl ?? "";
    }

    // ── bindable mirrors of the scalar edit-model fields (Plan 7b Task 10) ─────────────
    //
    // OhgConfigEditModel is a plain mutable class with NO INotifyPropertyChanged (deliberately —
    // it is the serialization shape, and Task 9's tests drive it directly). Binding
    // `OhgSettings.Model.X` TwoWay from XAML would therefore write once and never notify, so the
    // page could not reflect a value the VM changed. These mirrors are the bound surface; each
    // writes STRAIGHT THROUGH to Model on change, so Validate()/SaveAsync() — which read Model —
    // never need a separate commit pass, and Task 9's Model-first tests stay exactly as they were.
    // The looks list needs none of this: OhgLookEditorViewModel already does the same job per row.

    [ObservableProperty] private bool registryEnabled;
    [ObservableProperty] private bool handsQueueEnabled;
    [ObservableProperty] private bool questionFeedEnabled;
    [ObservableProperty] private string mukanaBaseUrl = "";
    [ObservableProperty] private string mukanaEvent = "";
    // The four intervals are DOUBLES because NumberBox.Value is a double and x:Bind will not
    // narrow one back to an int (that conversion is explicit in C#, so the generated TwoWay setter
    // would not compile). They round into the model's ints; NaN - what an emptied NumberBox
    // reports - keeps the model's previous value rather than writing a 0 the validator refuses.
    [ObservableProperty] private double panelistsIntervalMs;
    [ObservableProperty] private double handsIntervalMs;
    [ObservableProperty] private double questionIntervalMs;
    [ObservableProperty] private double maxBackoffMs;
    [ObservableProperty] private bool driveHost;
    [ObservableProperty] private string? presetSolo;
    [ObservableProperty] private string? presetActiveSpeaker;
    [ObservableProperty] private string? presetBlack;
    [ObservableProperty] private string? presetGallery;
    [ObservableProperty] private string defaultTransition = "cut";
    [ObservableProperty] private string tallyUrl = "";

    partial void OnRegistryEnabledChanged(bool value) => Model.RegistryEnabled = value;
    partial void OnHandsQueueEnabledChanged(bool value) => Model.HandsQueueEnabled = value;
    partial void OnQuestionFeedEnabledChanged(bool value) => Model.QuestionFeedEnabled = value;
    partial void OnMukanaBaseUrlChanged(string value) => Model.MukanaBaseUrl = Blank(value);
    partial void OnMukanaEventChanged(string value) => Model.MukanaEvent = Blank(value);
    partial void OnPanelistsIntervalMsChanged(double value) => Model.PanelistsIntervalMs = Whole(value, Model.PanelistsIntervalMs);
    partial void OnHandsIntervalMsChanged(double value) => Model.HandsIntervalMs = Whole(value, Model.HandsIntervalMs);
    partial void OnQuestionIntervalMsChanged(double value) => Model.QuestionIntervalMs = Whole(value, Model.QuestionIntervalMs);
    partial void OnMaxBackoffMsChanged(double value) => Model.MaxBackoffMs = Whole(value, Model.MaxBackoffMs);
    partial void OnDriveHostChanged(bool value) => Model.DriveHost = value;
    partial void OnPresetSoloChanged(string? value) => Model.PresetSolo = Blank(value);
    partial void OnPresetActiveSpeakerChanged(string? value) => Model.PresetActiveSpeaker = Blank(value);
    partial void OnPresetBlackChanged(string? value) => Model.PresetBlack = Blank(value);
    partial void OnPresetGalleryChanged(string? value) => Model.PresetGallery = Blank(value);
    partial void OnDefaultTransitionChanged(string value) => Model.DefaultTransition = value;
    partial void OnTallyUrlChanged(string value) => Model.TallyUrl = Blank(value);

    /// <summary>An empty/whitespace text box means "not set", i.e. null — never an empty string,
    /// which <c>ShowConfigValidator</c> and the engine parser both read as a real (bad) value.
    /// The "(none)" entry in each preset ComboBox arrives here as null already.</summary>
    private static string? Blank(string? value) => string.IsNullOrWhiteSpace(value) ? null : value;

    /// <summary>A NumberBox value as a whole number, keeping <paramref name="fallback"/> for the
    /// NaN an emptied box reports - writing 0 there would fail validation for a field the operator
    /// is only halfway through retyping.</summary>
    private static int Whole(double value, int fallback)
        => double.IsNaN(value) || double.IsInfinity(value) ? fallback : (int)Math.Round(value);

    /// <summary>Scene id for the picked NAME (the combos list names, the config stores ids), or
    /// null for the "(none)" entry / an unknown name. Pure so the code-behind's guarded handler
    /// carries no logic of its own.</summary>
    public string? SceneIdForName(string? name)
    {
        if (string.IsNullOrWhiteSpace(name))
        {
            return null;
        }

        foreach (var (id, sceneName) in SceneChoices)
        {
            if (string.Equals(sceneName, name, StringComparison.Ordinal))
            {
                return id;
            }
        }

        return null;
    }

    /// <summary>Display name for a stored scene id, or the id itself when the scene has since been
    /// renamed or deleted — showing the raw id is honest; showing nothing hides a broken config.</summary>
    public string? SceneNameForId(string? id)
    {
        if (string.IsNullOrEmpty(id))
        {
            return null;
        }

        foreach (var (sceneId, sceneName) in SceneChoices)
        {
            if (string.Equals(sceneId, id, StringComparison.Ordinal))
            {
                return sceneName;
            }
        }

        return id;
    }

    /// <summary>The "(none)" entry every preset/scene ComboBox carries, so an operator can UNSET a
    /// preset (the config's null) instead of only ever swapping it for another scene.</summary>
    public const string NoneChoice = "(none)";

    /// <summary>The four transitions <c>ShowConfigValidator</c> accepts.</summary>
    public static IReadOnlyList<string> TransitionChoices { get; } = ["cut", "fade", "dip", "wipe"];

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
