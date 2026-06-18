using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class OverlaysViewModel : ObservableObject
{
    private readonly StudioViewModel _studio;

    [ObservableProperty]
    private string _captionFontSize = ProductionCatalog.CaptionStyle.FontSize;

    [ObservableProperty]
    private string _captionTextColor = ProductionCatalog.CaptionStyle.TextColor;

    [ObservableProperty]
    private int _captionBackgroundOpacity = ProductionCatalog.CaptionStyle.BackgroundOpacity;

    [ObservableProperty]
    private bool _captionUppercase = ProductionCatalog.CaptionStyle.Uppercase;

    [ObservableProperty]
    private string _lowerThirdPosition = "lower-left";

    [ObservableProperty]
    private string _captionPosition = "bottom";

    public OverlaysViewModel(StudioViewModel studio)
    {
        _studio = studio;
    }

    public ObservableCollection<GraphicOverlay> Graphics => _studio.Graphics;

    public BrandKit BrandKit => _studio.BrandKit;

    public IReadOnlyList<CaptionTranscriptEntry> CaptionTranscript => _studio.CaptionTranscript;

    public IReadOnlyList<string> CaptionFontSizeOptions => CaptionStyleHelper.FontSizeOptions;

    public IReadOnlyList<string> LowerThirdPositionOptions => CaptionStyleHelper.LowerThirdPositionOptions;

    public IReadOnlyList<string> CaptionPositionOptions => CaptionStyleHelper.CaptionPositionOptions;

    public string CaptionStyleSummary => CaptionStyleHelper.Summarize(new CaptionStyleState
    {
        FontSize = CaptionFontSize,
        TextColor = CaptionTextColor,
        BackgroundOpacity = CaptionBackgroundOpacity,
        Uppercase = CaptionUppercase
    });

    public string CaptionQualitySummary => _studio.CaptionQualitySummary;

    public string LowerThirdPositionLabel => LowerThirdPosition.Replace('-', ' ');

    public string CaptionPositionLabel => CaptionPosition;

    public double CaptionFontSizePx => CaptionStyleHelper.FontSizeToPixels(CaptionFontSize);

    public string CaptionDisplayText =>
        CaptionStyleHelper.FormatCaptionText(_studio.CaptionText, CaptionUppercase);

    public SolidColorBrush CaptionTextBrush => CaptionStyleHelper.CaptionTextBrush(CaptionTextColor);

    public SolidColorBrush CaptionBackgroundBrush =>
        CaptionStyleHelper.CaptionBackgroundBrush(CaptionBackgroundOpacity);

    public HorizontalAlignment LowerThirdHorizontalAlignment =>
        CaptionStyleHelper.LowerThirdAlignment(LowerThirdPosition).Horizontal;

    public VerticalAlignment LowerThirdVerticalAlignment =>
        CaptionStyleHelper.LowerThirdAlignment(LowerThirdPosition).Vertical;

    public bool ShowCaptionStripBelowProgram =>
        CaptionStyleHelper.IsCaptionStripBelowProgram(CaptionPosition);

    public bool ShowCaptionStripInProgram => !ShowCaptionStripBelowProgram;

    public Thickness CaptionStripMargin =>
        CaptionPosition == "top" ? new Thickness(16, 16, 16, 0) : new Thickness(0);

    partial void OnCaptionFontSizeChanged(string value) => NotifyCaptionPresentationChanged();

    partial void OnCaptionTextColorChanged(string value) => NotifyCaptionPresentationChanged();

    partial void OnCaptionBackgroundOpacityChanged(int value) => NotifyCaptionPresentationChanged();

    partial void OnCaptionUppercaseChanged(bool value) => NotifyCaptionPresentationChanged();

    partial void OnLowerThirdPositionChanged(string value)
    {
        OnPropertyChanged(nameof(LowerThirdPositionLabel));
        OnPropertyChanged(nameof(LowerThirdHorizontalAlignment));
        OnPropertyChanged(nameof(LowerThirdVerticalAlignment));
    }

    partial void OnCaptionPositionChanged(string value)
    {
        OnPropertyChanged(nameof(CaptionPositionLabel));
        OnPropertyChanged(nameof(ShowCaptionStripBelowProgram));
        OnPropertyChanged(nameof(ShowCaptionStripInProgram));
        OnPropertyChanged(nameof(CaptionStripMargin));
    }

    public void NotifyCaptionContentChanged()
    {
        OnPropertyChanged(nameof(CaptionDisplayText));
    }

    [RelayCommand]
    private void ToggleGraphic(string graphicId) => _studio.ToggleGraphicCommand.Execute(graphicId);

    private void NotifyCaptionPresentationChanged()
    {
        OnPropertyChanged(nameof(CaptionStyleSummary));
        OnPropertyChanged(nameof(CaptionFontSizePx));
        OnPropertyChanged(nameof(CaptionDisplayText));
        OnPropertyChanged(nameof(CaptionTextBrush));
        OnPropertyChanged(nameof(CaptionBackgroundBrush));
    }
}