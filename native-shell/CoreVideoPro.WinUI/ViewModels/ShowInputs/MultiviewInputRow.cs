using CommunityToolkit.Mvvm.ComponentModel;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;
using Windows.UI;
using CoreVideoPro.MediaCore.Contracts;

namespace CoreVideoPro.WinUI.ViewModels.ShowInputs;

/// <summary>One fixed Multiview slot's observed cells. No media payload lives here.</summary>
public sealed partial class MultiviewInputRow : ObservableObject
{
    public MultiviewInputRow(int slotNumber) => SlotNumber = slotNumber;

    public int SlotNumber { get; }
    public SourceInstanceIdentity? LastAppliedSourceInstance { get; internal set; }
    public string? RosterEpoch { get; internal set; }
    public long HighestObservationRevision { get; internal set; }
    public int HighestFrameId { get; internal set; }
    public double LastFrameAtMs { get; internal set; } = -1;
    public int ObservedWidth { get; internal set; }
    public int ObservedHeight { get; internal set; }
    public int ObservedFps { get; internal set; }

    [ObservableProperty] private string? _sourceId;
    [ObservableProperty] private string _statusLabel = "IDLE";
    [ObservableProperty] private string _formatLabel = "—";
    [ObservableProperty] private string _configuredCapLabel = "";
    [ObservableProperty] private bool _capDiffers;
    [ObservableProperty] private bool _formatStale;
    [ObservableProperty] private double _frameAgeMs = -1;
    [ObservableProperty] private string _recLabel = "OFF";
    [ObservableProperty] private bool _hasPreviewTile;
    [ObservableProperty] private Rect _previewCropRect = new(0, 0, 1, 1);
    [ObservableProperty] private string _previewTally = "none";
    [ObservableProperty] private double _meterWidth;

    public Brush PreviewBorderBrush => new SolidColorBrush(PreviewTally switch
    {
        "pgm" or "hold" => Color.FromArgb(255, 255, 159, 10),
        "pvw" => Color.FromArgb(255, 48, 209, 88),
        "talking" => Color.FromArgb(255, 242, 242, 247),
        _ => Color.FromArgb(255, 58, 58, 60)
    });

    public string FormatDetailLabel => FormatStale ? $"age {FrameAgeMs / 1000:0.0}s stale" : ConfiguredCapLabel;
    public bool HasFormatDetail => FormatStale || CapDiffers;
    public Brush FormatBrush => new SolidColorBrush(FormatStale
        ? Color.FromArgb(255, 255, 159, 10) : Color.FromArgb(255, 242, 242, 247));
    public string StatusChipLabel => StatusLabel == "LIVE" && PreviewTally == "pgm" ? "PGM" : StatusLabel;
    public string RecChipLabel => RecLabel == "RECORDING" ? "REC" : RecLabel;
    public Brush StatusBackgroundBrush => new SolidColorBrush(StatusChipLabel switch
    {
        "LIVE" => Color.FromArgb(255, 14, 59, 30),
        "PGM" => Color.FromArgb(255, 92, 26, 18),
        "TALKING" or "STALLED" => Color.FromArgb(255, 58, 42, 0),
        _ => Color.FromArgb(255, 58, 58, 60)
    });
    public Brush RecBackgroundBrush => new SolidColorBrush(RecLabel switch
    {
        "RECORDING" => Color.FromArgb(255, 255, 69, 58),
        "ERROR" => Color.FromArgb(255, 92, 26, 18),
        "ARMED" => Color.FromArgb(255, 58, 42, 0),
        _ => Color.FromArgb(255, 58, 58, 60)
    });

    partial void OnPreviewTallyChanged(string value)
    {
        OnPropertyChanged(nameof(PreviewBorderBrush));
        OnPropertyChanged(nameof(StatusChipLabel));
        OnPropertyChanged(nameof(StatusBackgroundBrush));
    }
    partial void OnFormatStaleChanged(bool value) => RaiseFormatChrome();
    partial void OnCapDiffersChanged(bool value) => RaiseFormatChrome();
    partial void OnConfiguredCapLabelChanged(string value) => RaiseFormatChrome();
    partial void OnFrameAgeMsChanged(double value) => RaiseFormatChrome();
    partial void OnStatusLabelChanged(string value)
    {
        OnPropertyChanged(nameof(StatusChipLabel));
        OnPropertyChanged(nameof(StatusBackgroundBrush));
    }
    partial void OnRecLabelChanged(string value)
    {
        OnPropertyChanged(nameof(RecChipLabel));
        OnPropertyChanged(nameof(RecBackgroundBrush));
    }

    private void RaiseFormatChrome()
    {
        OnPropertyChanged(nameof(FormatDetailLabel));
        OnPropertyChanged(nameof(HasFormatDetail));
        OnPropertyChanged(nameof(FormatBrush));
    }
}
