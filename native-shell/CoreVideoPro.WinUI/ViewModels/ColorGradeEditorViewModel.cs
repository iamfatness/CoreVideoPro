using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// View-model backing the per-source color grade pop-out window. Edits a working
/// copy of a <see cref="ColorGrade"/> and publishes live grade/preview updates.
/// </summary>
public sealed partial class ColorGradeEditorViewModel : ObservableObject
{
    private const int MaxPreviewWidth = 640;
    private byte[]? _sourcePreviewBgra;
    private int _sourcePreviewWidth;
    private int _sourcePreviewHeight;

    /// <summary>LUT presets mirrored from the renderer (src/engine/colorGrade.ts).</summary>
    public static IReadOnlyList<string> LutOptions { get; } =
        ["none", "neutral", "warm-film", "cool-broadcast", "punch"];

    public ColorGradeEditorViewModel(
        string sourceId,
        string sourceName,
        ColorGrade grade,
        VideoSurfaceState? previewSurface = null)
    {
        SourceId = sourceId;
        SourceName = sourceName;
        _lut = grade.Lut;
        _exposure = grade.Exposure;
        _contrast = grade.Contrast;
        _saturation = grade.Saturation;
        _temperature = grade.Temperature;
        SetPreviewSurface(previewSurface);
    }

    public string SourceId { get; }

    public string SourceName { get; }

    public string HeaderTitle => $"Color grade - {SourceName}";

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

    [ObservableProperty]
    private byte[]? _gradedPreviewBgra;

    [ObservableProperty]
    private int _gradedPreviewWidth;

    [ObservableProperty]
    private int _gradedPreviewHeight;

    [ObservableProperty]
    private string _previewStatus = "Waiting for source preview frames.";

    public string Summary => BuildGrade().Summary;

    public bool HasPreview => GradedPreviewBgra is { Length: > 0 } && GradedPreviewWidth > 0 && GradedPreviewHeight > 0;

    public event EventHandler<ColorGrade>? GradeChanged;

    public event EventHandler<ColorGrade>? GradeSaved;

    public event EventHandler? Closed;

    public void SetPreviewSurface(VideoSurfaceState? surface)
    {
        if (surface?.HasPreviewBitmap != true)
        {
            _sourcePreviewBgra = null;
            _sourcePreviewWidth = 0;
            _sourcePreviewHeight = 0;
            GradedPreviewBgra = null;
            GradedPreviewWidth = 0;
            GradedPreviewHeight = 0;
            PreviewStatus = surface is null
                ? "Waiting for this source to publish preview frames."
                : $"{surface.Title}: {surface.StatusLine}";
            OnPropertyChanged(nameof(HasPreview));
            return;
        }

        PreviewStatus = $"{surface.Title}: {surface.StatusLine}";
        _sourcePreviewBgra = surface.PreviewBgra;
        _sourcePreviewWidth = surface.PreviewWidth;
        _sourcePreviewHeight = surface.PreviewHeight;
        RebuildPreview(surface.PreviewBgra!, surface.PreviewWidth, surface.PreviewHeight);
    }

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

    partial void OnLutChanged(string value) => OnGradeEdited();

    partial void OnExposureChanged(int value) => OnGradeEdited();

    partial void OnContrastChanged(int value) => OnGradeEdited();

    partial void OnSaturationChanged(int value) => OnGradeEdited();

    partial void OnTemperatureChanged(int value) => OnGradeEdited();

    private void OnGradeEdited()
    {
        if (_sourcePreviewBgra is { Length: > 0 })
        {
            RebuildPreview(_sourcePreviewBgra, _sourcePreviewWidth, _sourcePreviewHeight);
        }

        GradeChanged?.Invoke(this, BuildGrade());
        OnPropertyChanged(nameof(HasPreview));
    }

    private ColorGrade BuildGrade() => new()
    {
        Lut = Lut,
        Exposure = Exposure,
        Contrast = Contrast,
        Saturation = Saturation,
        Temperature = Temperature
    };

    private void RebuildPreview(byte[] sourceBgra, int sourceWidth, int sourceHeight)
    {
        if (sourceBgra.Length != sourceWidth * sourceHeight * 4 || sourceWidth <= 0 || sourceHeight <= 0)
        {
            GradedPreviewBgra = null;
            GradedPreviewWidth = 0;
            GradedPreviewHeight = 0;
            OnPropertyChanged(nameof(HasPreview));
            return;
        }

        var targetWidth = sourceWidth;
        var targetHeight = sourceHeight;
        if (sourceWidth > MaxPreviewWidth)
        {
            var scale = MaxPreviewWidth / (double)sourceWidth;
            targetWidth = MaxPreviewWidth;
            targetHeight = Math.Max(1, (int)Math.Round(sourceHeight * scale));
        }

        var graded = new byte[targetWidth * targetHeight * 4];
        var grade = BuildGrade();
        for (var y = 0; y < targetHeight; y++)
        {
            var sourceY = Math.Min(sourceHeight - 1, (int)(y * (sourceHeight / (double)targetHeight)));
            for (var x = 0; x < targetWidth; x++)
            {
                var sourceX = Math.Min(sourceWidth - 1, (int)(x * (sourceWidth / (double)targetWidth)));
                var sourceOffset = ((sourceY * sourceWidth) + sourceX) * 4;
                var targetOffset = ((y * targetWidth) + x) * 4;
                ApplyGrade(
                    sourceBgra[sourceOffset + 2],
                    sourceBgra[sourceOffset + 1],
                    sourceBgra[sourceOffset],
                    grade,
                    out graded[targetOffset + 2],
                    out graded[targetOffset + 1],
                    out graded[targetOffset]);
                graded[targetOffset + 3] = sourceBgra[sourceOffset + 3];
            }
        }

        GradedPreviewBgra = graded;
        GradedPreviewWidth = targetWidth;
        GradedPreviewHeight = targetHeight;
        OnPropertyChanged(nameof(HasPreview));
    }

    private static void ApplyGrade(
        byte red,
        byte green,
        byte blue,
        ColorGrade grade,
        out byte gradedRed,
        out byte gradedGreen,
        out byte gradedBlue)
    {
        var exposure = grade.Exposure / 100.0;
        var contrast = 1.0 + grade.Contrast / 100.0;
        var saturation = 1.0 + grade.Saturation / 100.0;
        var temperature = grade.Temperature / 100.0;

        ApplyLutOffsets(grade.Lut, ref exposure, ref contrast, ref saturation, ref temperature);

        var r = Clamp01((red / 255.0) + exposure + temperature * 0.08);
        var g = Clamp01((green / 255.0) + exposure);
        var b = Clamp01((blue / 255.0) + exposure - temperature * 0.08);

        r = Clamp01(((r - 0.5) * contrast) + 0.5);
        g = Clamp01(((g - 0.5) * contrast) + 0.5);
        b = Clamp01(((b - 0.5) * contrast) + 0.5);

        var luminance = (r * 0.2126) + (g * 0.7152) + (b * 0.0722);
        r = Clamp01(luminance + ((r - luminance) * saturation));
        g = Clamp01(luminance + ((g - luminance) * saturation));
        b = Clamp01(luminance + ((b - luminance) * saturation));

        gradedRed = ToByte(r);
        gradedGreen = ToByte(g);
        gradedBlue = ToByte(b);
    }

    private static void ApplyLutOffsets(
        string? lut,
        ref double exposure,
        ref double contrast,
        ref double saturation,
        ref double temperature)
    {
        switch (lut)
        {
            case "warm-film":
                contrast += 0.05;
                saturation += 0.05;
                temperature += 0.16;
                break;
            case "cool-broadcast":
                contrast += 0.04;
                temperature -= 0.14;
                break;
            case "punch":
                contrast += 0.12;
                saturation += 0.16;
                break;
            case "neutral":
                contrast += 0.02;
                break;
        }
    }

    private static double Clamp01(double value) => Math.Clamp(value, 0, 1);

    private static byte ToByte(double value) => (byte)Math.Clamp((int)Math.Round(value * 255), 0, 255);
}
