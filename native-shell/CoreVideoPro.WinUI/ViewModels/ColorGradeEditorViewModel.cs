using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// View-model backing the per-source color grade pop-out window. Edits a working
/// copy of a <see cref="ColorGrade"/> and raises <see cref="GradeSaved"/> on Save so
/// the host (StudioViewModel) can persist the grade for the source it was opened for.
/// </summary>
public sealed partial class ColorGradeEditorViewModel : ObservableObject
{
    /// <summary>LUT presets mirrored from the renderer (src/engine/colorGrade.ts).</summary>
    public static IReadOnlyList<string> LutOptions { get; } =
        ["none", "neutral", "warm-film", "cool-broadcast", "punch"];

    public ColorGradeEditorViewModel(string sourceId, string sourceName, ColorGrade grade)
    {
        SourceId = sourceId;
        SourceName = sourceName;
        _lut = grade.Lut;
        _exposure = grade.Exposure;
        _contrast = grade.Contrast;
        _saturation = grade.Saturation;
        _temperature = grade.Temperature;
    }

    /// <summary>Participant/source id this editor is grading.</summary>
    public string SourceId { get; }

    /// <summary>Display name shown in the editor header.</summary>
    public string SourceName { get; }

    public string HeaderTitle => $"Color grade — {SourceName}";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Summary))]
    private string _lut;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Summary))]
    private int _exposure;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Summary))]
    private int _contrast;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Summary))]
    private int _saturation;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Summary))]
    private int _temperature;

    /// <summary>Live readout of the working grade, identical shape to ColorGrade.Summary.</summary>
    public string Summary => BuildGrade().Summary;

    /// <summary>Raised when the operator commits the grade. The window listens and closes.</summary>
    public event EventHandler<ColorGrade>? GradeSaved;

    /// <summary>Raised when the operator dismisses without saving. The window listens and closes.</summary>
    public event EventHandler? Closed;

    [RelayCommand]
    private void Save()
    {
        GradeSaved?.Invoke(this, BuildGrade());
    }

    [RelayCommand]
    private void Close()
    {
        Closed?.Invoke(this, EventArgs.Empty);
    }

    private ColorGrade BuildGrade() => new()
    {
        Lut = Lut,
        Exposure = Exposure,
        Contrast = Contrast,
        Saturation = Saturation,
        Temperature = Temperature
    };
}
