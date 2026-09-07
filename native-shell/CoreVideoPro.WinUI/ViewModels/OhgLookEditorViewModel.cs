using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>Bindable mirror of one <see cref="OhgLookEdit"/> row in the settings editor
/// (Plan 7b Task 9). <see cref="OhgSettingsViewModel"/> keeps this in sync with the underlying
/// <see cref="OhgLookEdit"/> live — every setter here writes straight through — so
/// <c>Model.Looks</c> never needs a separate "commit" pass before <c>Validate()</c>/<c>SaveAsync</c>
/// read it.</summary>
public sealed partial class OhgLookEditorViewModel : ObservableObject
{
    public OhgLookEditorViewModel(OhgLookEdit edit)
    {
        Edit = edit;
        id = edit.Id;
        label = edit.Label;
        scenePreset = edit.ScenePreset;
        boxes = edit.Boxes;
        includesHost = edit.IncludesHost;
        includesReader = edit.IncludesReader;
        plateTone = edit.PlateTone;
        tallySource = edit.TallySource;
        boxFill = edit.BoxFill;
    }

    /// <summary>The backing model row this view model writes through to.</summary>
    public OhgLookEdit Edit { get; }

    [ObservableProperty] private string id;
    [ObservableProperty] private string label;
    [ObservableProperty] private string? scenePreset;
    [ObservableProperty] private int boxes;
    [ObservableProperty] private bool includesHost;
    [ObservableProperty] private bool includesReader;
    [ObservableProperty] private string plateTone;
    [ObservableProperty] private string tallySource;
    [ObservableProperty] private string boxFill;

    partial void OnIdChanged(string value) => Edit.Id = value;
    partial void OnLabelChanged(string value) => Edit.Label = value;
    partial void OnScenePresetChanged(string? value) => Edit.ScenePreset = value;
    partial void OnBoxesChanged(int value) => Edit.Boxes = value;
    partial void OnIncludesHostChanged(bool value) => Edit.IncludesHost = value;
    partial void OnIncludesReaderChanged(bool value) => Edit.IncludesReader = value;
    partial void OnPlateToneChanged(string value) => Edit.PlateTone = value;
    partial void OnTallySourceChanged(string value) => Edit.TallySource = value;
    partial void OnBoxFillChanged(string value) => Edit.BoxFill = value;
}
