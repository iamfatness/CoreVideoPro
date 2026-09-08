using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using WinRT.Interop;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class ProductionSettingsWindow : Window
{
    private const int WindowWidth = 1180;
    private const int WindowHeight = 780;

    public ProductionSettingsWindow(StudioViewModel viewModel)
    {
        ViewModel = viewModel ?? throw new ArgumentNullException(nameof(viewModel));

        // Built BEFORE InitializeComponent so the OHG panel's x:Binds have something to resolve on
        // their first pass. A failure here (an unreadable config folder, say) must not stop the
        // whole settings window opening - the section simply renders inert.
        try
        {
            OhgSettings = viewModel.CreateOhgSettingsViewModel();
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"ohg: settings section unavailable ({ex.GetType().Name}: {ex.Message})");
            OhgSettings = null;
        }

        InitializeComponent();
        Closed += OnWindowClosed;
        ApplyChromeAndSize();
        ShowSection("output");
    }

    public StudioViewModel ViewModel { get; }

    /// <summary>The OHG show-config editor's view model, or null when it could not be built. The
    /// section binds this directly; a null leaves the panel's controls inert rather than throwing
    /// during layout.</summary>
    public OhgSettingsViewModel? OhgSettings { get; }

    public event EventHandler? WindowClosed;

    public void ShowSection(string? section)
    {
        var normalized = string.IsNullOrWhiteSpace(section)
            ? "output"
            : section.Trim().ToLowerInvariant();

        ShowPanel(normalized switch
        {
            "stream" or "streaming" => StreamingPanel,
            "audio" => AudioPanel,
            "record" or "recording" => RecordingPanel,
            "multiviewer" or "multiview" => MultiviewerPanel,
            "license" or "plan" => LicensePanel,
            "ffmpeg" => FfmpegPanel,
            "ohg" => OhgPanel,
            _ => OutputPanel
        });
    }

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        Closed -= OnWindowClosed;
        WindowClosed?.Invoke(this, EventArgs.Empty);
    }

    private void OnOutputClicked(object sender, RoutedEventArgs args) => ShowSection("output");

    private void OnStreamingClicked(object sender, RoutedEventArgs args) => ShowSection("streaming");

    private void OnAudioClicked(object sender, RoutedEventArgs args) => ShowSection("audio");

    private void OnRecordingClicked(object sender, RoutedEventArgs args) => ShowSection("recording");

    private void OnMultiviewerClicked(object sender, RoutedEventArgs args) => ShowSection("multiviewer");

    private void OnLicenseClicked(object sender, RoutedEventArgs args) => ShowSection("license");

    private void OnFfmpegClicked(object sender, RoutedEventArgs args) => ShowSection("ffmpeg");

    private void OnOhgClicked(object sender, RoutedEventArgs args) => Guarded("ohg section", () => ShowSection("ohg"));

    /// <summary>x:Bind function binding for the section's two conditional lines. A pure static, not
    /// an IValueConverter: a converter is an untestable instance in a resource dictionary, this is
    /// an ordinary method.</summary>
    public static Visibility VisibleWhen(bool value) => value ? Visibility.Visible : Visibility.Collapsed;

    /// <summary>Removing a look is a Click, not a Command binding: inside an ItemsRepeater template
    /// the row is reached through <c>Tag</c> (the DataContext is null there), and an ElementName
    /// binding back out to the window's OhgSettings has nothing to name - a Window is not a
    /// FrameworkElement. The command itself still does the work.</summary>
    private void OnOhgRemoveLookClicked(object sender, RoutedEventArgs args)
        => Guarded("ohg remove look", () =>
        {
            if (sender is FrameworkElement element && LookFor(element) is { } look)
            {
                OhgSettings?.RemoveLookCommand.Execute(look);
            }
        });

    // ---- OHG show config section (Plan 7b Task 10) ----------------------------------
    //
    // Two house rules shape every handler below (both from CLAUDE.md):
    //
    //  * A throwing UI callback fail-fasts the process with NO managed log, so every one of them
    //    routes through Guarded(...) - a mis-set dropdown is cosmetic, a crash ends the show.
    //  * Selector selection is NEVER driven by x:Bind (a recycled ItemsRepeater container re-bound
    //    mid-resolve threw COMException 0x80004005 out of set_SelectedValue and killed the app), so
    //    every ComboBox here is populated and selected in code-behind on Loaded and reports back
    //    through SelectionChanged, with the re-entrancy guard below suppressing the echo.

    /// <summary>Suppresses the SelectionChanged a programmatic selection raises, so syncing a combo
    /// to the stored config is never mistaken for the operator editing it.</summary>
    private bool _applyingOhgSelection;

    private void Guarded(string what, Action body)
    {
        try
        {
            body();
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"ohg: {what} skipped ({ex.GetType().Name}: {ex.Message})");
        }
    }

    /// <summary>Fills a scene ComboBox with "(none)" + every scene NAME and selects the one the
    /// config's stored id resolves to. Names are what an operator recognises; the id is what the
    /// config stores, and <c>SceneIdForName</c> maps back on the way out.</summary>
    private void FillSceneCombo(ComboBox combo, string? selectedSceneId)
    {
        if (OhgSettings is not { } settings)
        {
            return;
        }

        _applyingOhgSelection = true;
        try
        {
            combo.Items.Clear();
            combo.Items.Add(OhgSettingsViewModel.NoneChoice);
            foreach (var (_, name) in settings.SceneChoices)
            {
                combo.Items.Add(name);
            }

            var selectedName = settings.SceneNameForId(selectedSceneId);
            combo.SelectedItem = selectedName is not null && combo.Items.Contains(selectedName)
                ? selectedName
                : OhgSettingsViewModel.NoneChoice;
        }
        finally
        {
            _applyingOhgSelection = false;
        }
    }

    private void FillChoiceCombo(ComboBox combo, IReadOnlyList<string> choices, string? selected)
    {
        _applyingOhgSelection = true;
        try
        {
            combo.Items.Clear();
            foreach (var choice in choices)
            {
                combo.Items.Add(choice);
            }

            // A stored value the engine does not know (a hand-edited config, or one written by the
            // build that offered "warm"/"cool" plate tones) is ADDED as an extra item rather than
            // silently swapped for choices[0]: the picker must show what the model actually holds,
            // and Save's enum rules are what refuse it — out loud, naming the look.
            if (selected is { Length: > 0 } && !combo.Items.Contains(selected))
            {
                combo.Items.Add(selected);
            }

            combo.SelectedItem = selected is not null && combo.Items.Contains(selected)
                ? selected
                : (choices.Count > 0 ? choices[0] : null);
        }
        finally
        {
            _applyingOhgSelection = false;
        }
    }

    /// <summary>The picked scene id, or null for "(none)" / a stale name. <paramref name="isOperatorEdit"/>
    /// is false when the change is the programmatic sync above, so the caller writes nothing.</summary>
    private string? PickedSceneId(object sender, out bool isOperatorEdit)
    {
        isOperatorEdit = !_applyingOhgSelection && OhgSettings is not null && sender is ComboBox;
        return isOperatorEdit ? OhgSettings!.SceneIdForName(((ComboBox)sender).SelectedItem as string) : null;
    }

    /// <summary>Re-reads the app's scene list and REDRAWS the four preset pickers. A Click, not a
    /// Command binding, because Button.Command runs after the Click handler - refilling from a
    /// separate handler would use the scene list from before the refresh.</summary>
    private void OnOhgRefreshScenesClicked(object sender, RoutedEventArgs args)
        => Guarded("ohg refresh scenes", () =>
        {
            if (OhgSettings is not { } settings) return;

            settings.RefreshScenesCommand.Execute(null);
            FillSceneCombo(OhgPresetSoloCombo, settings.PresetSolo);
            FillSceneCombo(OhgPresetActiveSpeakerCombo, settings.PresetActiveSpeaker);
            FillSceneCombo(OhgPresetBlackCombo, settings.PresetBlack);
            FillSceneCombo(OhgPresetGalleryCombo, settings.PresetGallery);
        });

    // -- the four preset scene pickers --

    private void OnOhgPresetSoloLoaded(object sender, RoutedEventArgs args)
        => Guarded("ohg preset solo sync", () => FillSceneCombo((ComboBox)sender, OhgSettings?.PresetSolo));

    private void OnOhgPresetSoloChanged(object sender, SelectionChangedEventArgs args)
        => Guarded("ohg preset solo change", () =>
        {
            var sceneId = PickedSceneId(sender, out var edit);
            if (edit) OhgSettings!.PresetSolo = sceneId;
        });

    private void OnOhgPresetActiveSpeakerLoaded(object sender, RoutedEventArgs args)
        => Guarded("ohg preset active speaker sync", () => FillSceneCombo((ComboBox)sender, OhgSettings?.PresetActiveSpeaker));

    private void OnOhgPresetActiveSpeakerChanged(object sender, SelectionChangedEventArgs args)
        => Guarded("ohg preset active speaker change", () =>
        {
            var sceneId = PickedSceneId(sender, out var edit);
            if (edit) OhgSettings!.PresetActiveSpeaker = sceneId;
        });

    private void OnOhgPresetBlackLoaded(object sender, RoutedEventArgs args)
        => Guarded("ohg preset black sync", () => FillSceneCombo((ComboBox)sender, OhgSettings?.PresetBlack));

    private void OnOhgPresetBlackChanged(object sender, SelectionChangedEventArgs args)
        => Guarded("ohg preset black change", () =>
        {
            var sceneId = PickedSceneId(sender, out var edit);
            if (edit) OhgSettings!.PresetBlack = sceneId;
        });

    private void OnOhgPresetGalleryLoaded(object sender, RoutedEventArgs args)
        => Guarded("ohg preset gallery sync", () => FillSceneCombo((ComboBox)sender, OhgSettings?.PresetGallery));

    private void OnOhgPresetGalleryChanged(object sender, SelectionChangedEventArgs args)
        => Guarded("ohg preset gallery change", () =>
        {
            var sceneId = PickedSceneId(sender, out var edit);
            if (edit) OhgSettings!.PresetGallery = sceneId;
        });

    // -- default transition --

    private void OnOhgTransitionLoaded(object sender, RoutedEventArgs args)
        => Guarded("ohg transition sync", () =>
            FillChoiceCombo((ComboBox)sender, OhgSettingsViewModel.TransitionChoices, OhgSettings?.DefaultTransition));

    private void OnOhgTransitionChanged(object sender, SelectionChangedEventArgs args)
        => Guarded("ohg transition change", () =>
        {
            if (_applyingOhgSelection || OhgSettings is null || sender is not ComboBox combo) return;
            if (combo.SelectedItem is string picked && picked.Length > 0) OhgSettings.DefaultTransition = picked;
        });

    // -- integration toggles + drive host (applied on Toggled, never TwoWay-bound) --

    private void OnOhgRegistryToggled(object sender, RoutedEventArgs args)
        => Guarded("ohg registry toggle", () =>
        {
            if (OhgSettings is not null && sender is ToggleSwitch toggle) OhgSettings.RegistryEnabled = toggle.IsOn;
        });

    private void OnOhgHandsQueueToggled(object sender, RoutedEventArgs args)
        => Guarded("ohg hands queue toggle", () =>
        {
            if (OhgSettings is not null && sender is ToggleSwitch toggle) OhgSettings.HandsQueueEnabled = toggle.IsOn;
        });

    private void OnOhgQuestionFeedToggled(object sender, RoutedEventArgs args)
        => Guarded("ohg question feed toggle", () =>
        {
            if (OhgSettings is not null && sender is ToggleSwitch toggle) OhgSettings.QuestionFeedEnabled = toggle.IsOn;
        });

    private void OnOhgDriveHostToggled(object sender, RoutedEventArgs args)
        => Guarded("ohg drive host toggle", () =>
        {
            if (OhgSettings is not null && sender is ToggleSwitch toggle) OhgSettings.DriveHost = toggle.IsOn;
        });

    // -- per-look rows (the ItemsRepeater template) --

    private void OnOhgLookHostToggled(object sender, RoutedEventArgs args)
        => Guarded("ohg look host toggle", () =>
        {
            if (sender is ToggleSwitch toggle && LookFor(toggle) is { } look) look.IncludesHost = toggle.IsOn;
        });

    private void OnOhgLookReaderToggled(object sender, RoutedEventArgs args)
        => Guarded("ohg look reader toggle", () =>
        {
            if (sender is ToggleSwitch toggle && LookFor(toggle) is { } look) look.IncludesReader = toggle.IsOn;
        });

    private void OnOhgLookSceneLoaded(object sender, RoutedEventArgs args)
        => Guarded("ohg look scene sync", () =>
        {
            if (sender is not ComboBox combo) return;
            FillSceneCombo(combo, LookFor(combo)?.ScenePreset);
        });

    private void OnOhgLookSceneChanged(object sender, SelectionChangedEventArgs args)
        => Guarded("ohg look scene change", () =>
        {
            if (_applyingOhgSelection || OhgSettings is null || sender is not ComboBox combo) return;
            if (LookFor(combo) is { } look) look.ScenePreset = OhgSettings.SceneIdForName(combo.SelectedItem as string);
        });

    private void OnOhgLookPlateToneLoaded(object sender, RoutedEventArgs args)
        => Guarded("ohg plate tone sync", () =>
        {
            if (sender is not ComboBox combo) return;
            FillChoiceCombo(combo, PlateTones, LookFor(combo)?.PlateTone);
        });

    private void OnOhgLookPlateToneChanged(object sender, SelectionChangedEventArgs args)
        => Guarded("ohg plate tone change", () =>
        {
            if (_applyingOhgSelection || sender is not ComboBox combo) return;
            if (combo.SelectedItem is string picked && LookFor(combo) is { } look) look.PlateTone = picked;
        });

    private void OnOhgLookTallySourceLoaded(object sender, RoutedEventArgs args)
        => Guarded("ohg tally source sync", () =>
        {
            if (sender is not ComboBox combo) return;
            FillChoiceCombo(combo, TallySources, LookFor(combo)?.TallySource);
        });

    private void OnOhgLookTallySourceChanged(object sender, SelectionChangedEventArgs args)
        => Guarded("ohg tally source change", () =>
        {
            if (_applyingOhgSelection || sender is not ComboBox combo) return;
            if (combo.SelectedItem is string picked && LookFor(combo) is { } look) look.TallySource = picked;
        });

    private void OnOhgLookBoxFillLoaded(object sender, RoutedEventArgs args)
        => Guarded("ohg box fill sync", () =>
        {
            if (sender is not ComboBox combo) return;
            FillChoiceCombo(combo, BoxFills, LookFor(combo)?.BoxFill);
        });

    private void OnOhgLookBoxFillChanged(object sender, SelectionChangedEventArgs args)
        => Guarded("ohg box fill change", () =>
        {
            if (_applyingOhgSelection || sender is not ComboBox combo) return;
            if (combo.SelectedItem is string picked && LookFor(combo) is { } look) look.BoxFill = picked;
        });

    /// <summary>The row a templated control belongs to. Inside an ItemsRepeater template the
    /// DataContext is null, so every template element carries <c>Tag="{x:Bind}"</c> and this reads
    /// the row from there - the same rule OhgShowPage follows.</summary>
    private static OhgLookEditorViewModel? LookFor(FrameworkElement element)
        => element.Tag as OhgLookEditorViewModel ?? element.DataContext as OhgLookEditorViewModel;

    /// <summary>The enumerations <c>show-engine/src/contracts.ts</c> defines for a look — the ONE
    /// copy lives in <see cref="OhgLookChoices"/>, pinned against the engine's arrays by
    /// <c>OhgSettingsChoicesTests</c>. These pickers previously offered "warm"/"cool" plate tones,
    /// which no engine build has ever accepted: saving one made <c>optionalPlateTone</c> throw at
    /// the next config parse, i.e. exit 78 — terminal, no respawn.</summary>
    internal static string[] PlateTones => OhgLookChoices.PlateTones;
    internal static string[] TallySources => OhgLookChoices.TallySources;
    internal static string[] BoxFills => OhgLookChoices.BoxFills;

    // Plan 7b Task 11 — one-shot import from the two legacy Isadora config files. The Click
    // handler itself stays SYNC and routed through Guarded (the house rule every OHG UI callback
    // here follows), dispatching the actual async picker/import work as a fire-and-forget task
    // that carries its own try/catch (an unobserved exception on a fire-and-forget task is as
    // fatal as a throwing TryEnqueue callback - CLAUDE.md's UiDispatch rule).
    private void OnOhgImportLegacyClicked(object sender, RoutedEventArgs args)
        => Guarded("ohg import legacy show", () => _ = RunOhgImportLegacyAsync());

    /// <summary>Two optional <see cref="Windows.Storage.Pickers.FileOpenPicker"/>s
    /// (infrastructure, then mukana; cancelling either just leaves that text null, per D4's
    /// "tolerant extraction" rule), then <see cref="OhgSettingsViewModel.ImportLegacyCommand"/>
    /// runs the pure importer and refreshes the section.</summary>
    private async Task RunOhgImportLegacyAsync()
    {
        try
        {
            if (OhgSettings is null)
            {
                return;
            }

            var infrastructureJs = await PickLegacyIsadoraTextAsync();
            var mukanaJs = await PickLegacyIsadoraTextAsync();
            await OhgSettings.ImportLegacyCommand.ExecuteAsync((infrastructureJs, mukanaJs));
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"ohg: import legacy show skipped ({ex.GetType().Name}: {ex.Message})");
        }
    }

    /// <summary>Opens a <see cref="Windows.Storage.Pickers.FileOpenPicker"/> for one legacy `.js`
    /// file and returns its text, or null on cancel/failure (an optional input, per D4).</summary>
    private async Task<string?> PickLegacyIsadoraTextAsync()
    {
        try
        {
            var picker = new Windows.Storage.Pickers.FileOpenPicker
            {
                SuggestedStartLocation = Windows.Storage.Pickers.PickerLocationId.Desktop,
            };
            picker.FileTypeFilter.Add(".js");
            InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(this));
            var file = await picker.PickSingleFileAsync();
            if (file is null)
            {
                return null;
            }
            return await Windows.Storage.FileIO.ReadTextAsync(file);
        }
        catch
        {
            // Picker can throw if the shell COM apartment is busy; treat as "not provided".
            return null;
        }
    }

    // Pick the folder where recordings are written. The whole path plumbing already
    // exists (RecordingTargetFolder -> targetFolder wire -> core resolveTargetDir); this
    // just gives the operator a real folder picker instead of hand-typing a path.
    private async void OnBrowseRecordingFolderClicked(object sender, RoutedEventArgs args)
    {
        try
        {
            var picker = new Windows.Storage.Pickers.FolderPicker
            {
                SuggestedStartLocation = Windows.Storage.Pickers.PickerLocationId.VideosLibrary,
            };
            picker.FileTypeFilter.Add("*"); // required or PickSingleFolderAsync throws
            InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(this));
            var folder = await picker.PickSingleFolderAsync();
            if (folder is not null && !string.IsNullOrWhiteSpace(folder.Path))
            {
                ViewModel.RecordingTargetFolder = folder.Path;
            }
        }
        catch
        {
            // Picker can throw if the shell COM apartment is busy; leave the field as-is.
        }
    }

    // Reveal the current recording folder in Explorer so the operator can confirm where
    // files land (creates it if it does not exist yet).
    private void OnOpenRecordingFolderClicked(object sender, RoutedEventArgs args)
    {
        var path = ViewModel.RecordingTargetFolder;
        if (string.IsNullOrWhiteSpace(path))
        {
            return;
        }

        try
        {
            System.IO.Directory.CreateDirectory(path);
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
            {
                FileName = path,
                UseShellExecute = true,
            });
        }
        catch
        {
            // Non-fatal: an invalid/inaccessible path just doesn't open.
        }
    }

    private void ShowPanel(FrameworkElement activePanel)
    {
        OutputPanel.Visibility = activePanel == OutputPanel ? Visibility.Visible : Visibility.Collapsed;
        StreamingPanel.Visibility = activePanel == StreamingPanel ? Visibility.Visible : Visibility.Collapsed;
        AudioPanel.Visibility = activePanel == AudioPanel ? Visibility.Visible : Visibility.Collapsed;
        RecordingPanel.Visibility = activePanel == RecordingPanel ? Visibility.Visible : Visibility.Collapsed;
        MultiviewerPanel.Visibility = activePanel == MultiviewerPanel ? Visibility.Visible : Visibility.Collapsed;
        LicensePanel.Visibility = activePanel == LicensePanel ? Visibility.Visible : Visibility.Collapsed;
        FfmpegPanel.Visibility = activePanel == FfmpegPanel ? Visibility.Visible : Visibility.Collapsed;
        OhgPanel.Visibility = activePanel == OhgPanel ? Visibility.Visible : Visibility.Collapsed;
    }

    private void ApplyChromeAndSize()
    {
        var hwnd = WindowNative.GetWindowHandle(this);
        var windowId = Win32Interop.GetWindowIdFromWindow(hwnd);
        var appWindow = AppWindow.GetFromWindowId(windowId);
        if (appWindow is null)
        {
            return;
        }

        appWindow.Resize(new Windows.Graphics.SizeInt32(WindowWidth, WindowHeight));
        WindowChromeService.Apply(this, appWindow);

        if (appWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.IsMaximizable = true;
            presenter.IsMinimizable = true;
            presenter.IsResizable = true;
        }
    }
}
