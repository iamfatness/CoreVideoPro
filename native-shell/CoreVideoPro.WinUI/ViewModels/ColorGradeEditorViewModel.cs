using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using System.Globalization;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>Grade controls and native observations only; never processes pixels.</summary>
public sealed partial class ColorGradeEditorViewModel : ObservableObject
{
    public static IReadOnlyList<string> LutOptions { get; } =
        ["none", "neutral", "warm-film", "cool-broadcast", "punch"];

    public ColorGradeEditorViewModel(string sourceId, string sourceName, ColorGrade grade)
    {
        SourceId = sourceId; SourceName = sourceName;
        _lut = grade.Lut; _exposure = grade.Exposure; _contrast = grade.Contrast;
        _saturation = grade.Saturation; _temperature = grade.Temperature;
        _nativeSurface = WaitingSurface();
    }
    public string InstanceId { get; } = Guid.NewGuid().ToString("N");
    public string SourceId { get; }
    public string SourceName { get; }
    public string HeaderTitle => $"Color grade - {SourceName}";
    public long Revision { get; private set; }
    [ObservableProperty, NotifyPropertyChangedFor(nameof(Summary))] private string _lut;
    [ObservableProperty, NotifyPropertyChangedFor(nameof(Summary))] private int _exposure;
    [ObservableProperty, NotifyPropertyChangedFor(nameof(Summary))] private int _contrast;
    [ObservableProperty, NotifyPropertyChangedFor(nameof(Summary))] private int _saturation;
    [ObservableProperty, NotifyPropertyChangedFor(nameof(Summary))] private int _temperature;
    [ObservableProperty] private bool _liveEditing = true;
    [ObservableProperty] private bool _compareOriginal;
    [ObservableProperty, NotifyPropertyChangedFor(nameof(HasPreview))] private VideoSurfaceState _nativeSurface;
    [ObservableProperty] private string _previewStatus = "Waiting for native grading preview.";
    public string Summary => CurrentGrade.Summary;
    public bool HasPreview => NativeSurface.PendingSharedHandle is { IsValid: true };
    public ColorGrade CurrentGrade => new() { Lut = Lut, Exposure = Exposure, Contrast = Contrast,
        Saturation = Saturation, Temperature = Temperature };
    public ColorGrade PreviewGrade => CompareOriginal ? new() { Lut = "none" } : CurrentGrade;
    public event EventHandler? PreviewRequested;
    public event EventHandler<ColorGrade>? GradeChanged;
    public event EventHandler<ColorGrade>? GradeSaved;
    public event EventHandler? Closed;
    partial void OnLutChanged(string value) => OnGradeEdited();
    partial void OnExposureChanged(int value) => OnGradeEdited();
    partial void OnContrastChanged(int value) => OnGradeEdited();
    partial void OnSaturationChanged(int value) => OnGradeEdited();
    partial void OnTemperatureChanged(int value) => OnGradeEdited();
    partial void OnCompareOriginalChanged(bool value) => RequestPreview();
    partial void OnLiveEditingChanged(bool value) { if (value) GradeChanged?.Invoke(this, CurrentGrade); }
    private void OnGradeEdited()
    {
        RequestPreview();
        if (LiveEditing) GradeChanged?.Invoke(this, CurrentGrade);
    }
    private void RequestPreview()
    {
        ++Revision;
        NativeSurface = WaitingSurface();
        PreviewStatus = CompareOriginal ? "Preparing original source comparison (monitor only)." : "Applying draft to native preview.";
        PreviewRequested?.Invoke(this, EventArgs.Empty);
    }
    public void SetNativeUnavailable(string reason)
    {
        NativeSurface = WaitingSurface() with { StatusLine = reason };
        PreviewStatus = reason;
    }
    public void ObserveNativePreview(GradePreviewObservation observation)
    {
        if (observation.InstanceId != InstanceId || observation.SourceId != SourceId || observation.Revision != Revision) return;
        var label = observation.Status switch {
            "ready" => CompareOriginal ? "Original source — monitor comparison only" : "Native graded source",
            "stale" => "Source is not advancing — last native image held",
            "held" => "Source unavailable — last native image held",
            "preparing" => "Preparing native grading preview",
            _ => $"Native preview unavailable: {observation.Reason}"
        };
        PreviewStatus = label;
        if (observation.Status is not ("ready" or "held" or "stale") || observation.Texture is not { } texture ||
            string.IsNullOrEmpty(texture.SharedHandleHex) ||
            !ulong.TryParse(texture.SharedHandleHex.Replace("0x", "", StringComparison.OrdinalIgnoreCase),
                NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var handle) || handle == 0)
        {
            NativeSurface = WaitingSurface() with { StatusLine = label }; return;
        }
        NativeSurface = WaitingSurface() with { StatusLine = label,
            PendingSharedHandle = new SharedTextureHandle { NtHandle = handle, Width = texture.Width,
                Height = texture.Height, Format = texture.Format, FrameNumber = texture.FrameNumber } };
    }
    private VideoSurfaceState WaitingSurface() => VideoSurfaceState.Waiting(VideoSurfaceKind.Participant,
        $"grade:{InstanceId}", SourceName);
    [RelayCommand] private void Save() => GradeSaved?.Invoke(this, CurrentGrade);
    [RelayCommand] private void Close() => Closed?.Invoke(this, EventArgs.Empty);
}
