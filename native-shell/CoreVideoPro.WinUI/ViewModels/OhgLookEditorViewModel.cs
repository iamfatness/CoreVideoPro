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
    partial void OnBoxesChanged(int value)
    {
        Edit.Boxes = value;
        OnPropertyChanged(nameof(BoxesValue));
    }

    /// <summary>The box count as a <see cref="double"/>, because <c>NumberBox.Value</c> is a double
    /// and x:Bind will not narrow one to an int on the way back (that conversion is explicit in
    /// C#, so the generated TwoWay setter would not compile). NaN - what an emptied NumberBox
    /// reports - is read as 0 rather than throwing out of a bound setter.</summary>
    public double BoxesValue
    {
        get => Boxes;
        set => Boxes = double.IsNaN(value) ? 0 : (int)Math.Round(value);
    }
    partial void OnIncludesHostChanged(bool value) => Edit.IncludesHost = value;
    partial void OnIncludesReaderChanged(bool value) => Edit.IncludesReader = value;
    partial void OnPlateToneChanged(string value) => Edit.PlateTone = value;
    partial void OnTallySourceChanged(string value) => Edit.TallySource = value;
    partial void OnBoxFillChanged(string value) => Edit.BoxFill = value;
}
