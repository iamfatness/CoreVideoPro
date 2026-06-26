using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class OverlaysViewModel : ObservableObject
{
    private readonly StudioViewModel _studio;
    private bool _updatingBrandColors;

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
        BrandName = studio.BrandKit.Name;
        BrandLogoText = studio.BrandKit.LogoText;
        BrandPrimaryColor = studio.BrandKit.BrandColor;
        BrandAccentColor = studio.BrandKit.AccentColor;
        BrandBackgroundColor = studio.BrandKit.BackgroundColor;
        BrandPrimaryPickerColor = ParseHexColorOrDefault(BrandPrimaryColor, Color.FromArgb(255, 68, 193, 161));
        BrandAccentPickerColor = ParseHexColorOrDefault(BrandAccentColor, Color.FromArgb(255, 240, 168, 92));
        BrandBackgroundPickerColor = ParseHexColorOrDefault(BrandBackgroundColor, Color.FromArgb(255, 12, 17, 24));
        BrandLowerThirdStyle = studio.BrandKit.LowerThirdStyle;
        BrandDefaultOverlayBehavior = studio.BrandKit.DefaultOverlayBehavior;
    }

    [ObservableProperty]
    private string _brandName = string.Empty;

    [ObservableProperty]
    private string _brandLogoText = string.Empty;

    [ObservableProperty]
    private string _brandPrimaryColor = string.Empty;

    [ObservableProperty]
    private string _brandAccentColor = string.Empty;

    [ObservableProperty]
    private string _brandBackgroundColor = string.Empty;

    [ObservableProperty]
    private Color _brandPrimaryPickerColor;

    [ObservableProperty]
    private Color _brandAccentPickerColor;

    [ObservableProperty]
    private Color _brandBackgroundPickerColor;

    [ObservableProperty]
    private string _brandLowerThirdStyle = string.Empty;

    [ObservableProperty]
    private string _brandDefaultOverlayBehavior = string.Empty;

    public ObservableCollection<GraphicOverlay> Graphics => _studio.Graphics;

    public bool HasGraphics => Graphics.Count > 0;

    public BrandKit BrandKit => _studio.BrandKit;

    public IReadOnlyList<CaptionTranscriptEntry> CaptionTranscript => _studio.CaptionTranscript;

    public IReadOnlyList<string> CaptionFontSizeOptions => CaptionStyleHelper.FontSizeOptions;

    public IReadOnlyList<string> LowerThirdPositionOptions => CaptionStyleHelper.LowerThirdPositionOptions;

    public IReadOnlyList<string> CaptionPositionOptions => CaptionStyleHelper.CaptionPositionOptions;

    public IReadOnlyList<string> BrandLowerThirdStyleOptions { get; } = ["solid", "minimal", "gradient"];

    public IReadOnlyList<string> BrandDefaultOverlayBehaviorOptions { get; } = ["all-off", "manual"];

    public string CaptionStyleSummary => CaptionStyleHelper.Summarize(new CaptionStyleState
    {
        FontSize = CaptionFontSize,
        TextColor = CaptionTextColor,
        BackgroundOpacity = CaptionBackgroundOpacity,
        Uppercase = CaptionUppercase
    });

    public string BrandColorSummary => $"{BrandKit.BrandColor} / {BrandKit.AccentColor} / {BrandKit.BackgroundColor}";

    public SolidColorBrush BrandPrimarySwatch => new(BrandPrimaryPickerColor);

    public SolidColorBrush BrandAccentSwatch => new(BrandAccentPickerColor);

    public SolidColorBrush BrandBackgroundSwatch => new(BrandBackgroundPickerColor);

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

    partial void OnBrandNameChanged(string value) => UpdateBrandKit();

    partial void OnBrandLogoTextChanged(string value) => UpdateBrandKit();

    partial void OnBrandPrimaryColorChanged(string value)
    {
        if (TryParseHexColor(value, out var color))
        {
            SetPickerColorFromText(ref _updatingBrandColors, () => BrandPrimaryPickerColor = color);
        }

        UpdateBrandKit();
    }

    partial void OnBrandAccentColorChanged(string value)
    {
        if (TryParseHexColor(value, out var color))
        {
            SetPickerColorFromText(ref _updatingBrandColors, () => BrandAccentPickerColor = color);
        }

        UpdateBrandKit();
    }

    partial void OnBrandBackgroundColorChanged(string value)
    {
        if (TryParseHexColor(value, out var color))
        {
            SetPickerColorFromText(ref _updatingBrandColors, () => BrandBackgroundPickerColor = color);
        }

        UpdateBrandKit();
    }

    partial void OnBrandPrimaryPickerColorChanged(Color value)
    {
        OnPropertyChanged(nameof(BrandPrimarySwatch));
        if (_updatingBrandColors)
        {
            return;
        }

        BrandPrimaryColor = ToHex(value);
    }

    partial void OnBrandAccentPickerColorChanged(Color value)
    {
        OnPropertyChanged(nameof(BrandAccentSwatch));
        if (_updatingBrandColors)
        {
            return;
        }

        BrandAccentColor = ToHex(value);
    }

    partial void OnBrandBackgroundPickerColorChanged(Color value)
    {
        OnPropertyChanged(nameof(BrandBackgroundSwatch));
        if (_updatingBrandColors)
        {
            return;
        }

        BrandBackgroundColor = ToHex(value);
    }

    partial void OnBrandLowerThirdStyleChanged(string value) => UpdateBrandKit();

    partial void OnBrandDefaultOverlayBehaviorChanged(string value) => UpdateBrandKit();

    partial void OnLowerThirdPositionChanged(string value)
    {
        OnPropertyChanged(nameof(LowerThirdPositionLabel));
        OnPropertyChanged(nameof(LowerThirdHorizontalAlignment));
        OnPropertyChanged(nameof(LowerThirdVerticalAlignment));
        _studio.NotifyLowerThirdPresentationChanged();
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

    [RelayCommand]
    private void AddGraphic(string kind)
    {
        if (_studio.AddGraphicOverlay(kind))
        {
            OnPropertyChanged(nameof(HasGraphics));
        }
    }

    [RelayCommand]
    private void UseSelectedMediaAsLogo()
    {
        if (!_studio.ApplySelectedMediaAssetAsBrandLogo())
        {
            _studio.CommandStatus = "Select a media asset before assigning the brand logo";
        }
    }

    public void NotifyBrandKitChanged()
    {
        BrandName = BrandKit.Name;
        BrandLogoText = BrandKit.LogoText;
        BrandPrimaryColor = BrandKit.BrandColor;
        BrandAccentColor = BrandKit.AccentColor;
        BrandBackgroundColor = BrandKit.BackgroundColor;
        BrandLowerThirdStyle = BrandKit.LowerThirdStyle;
        BrandDefaultOverlayBehavior = BrandKit.DefaultOverlayBehavior;
        OnPropertyChanged(nameof(BrandKit));
        OnPropertyChanged(nameof(BrandColorSummary));
    }

    private void NotifyCaptionPresentationChanged()
    {
        OnPropertyChanged(nameof(CaptionStyleSummary));
        OnPropertyChanged(nameof(CaptionFontSizePx));
        OnPropertyChanged(nameof(CaptionDisplayText));
        OnPropertyChanged(nameof(CaptionTextBrush));
        OnPropertyChanged(nameof(CaptionBackgroundBrush));
        UpdateBrandKit();
    }

    private void UpdateBrandKit()
    {
        if (string.IsNullOrWhiteSpace(BrandName) ||
            string.IsNullOrWhiteSpace(BrandLogoText) ||
            string.IsNullOrWhiteSpace(BrandPrimaryColor) ||
            string.IsNullOrWhiteSpace(BrandAccentColor) ||
            string.IsNullOrWhiteSpace(BrandBackgroundColor) ||
            string.IsNullOrWhiteSpace(BrandLowerThirdStyle) ||
            string.IsNullOrWhiteSpace(BrandDefaultOverlayBehavior))
        {
            return;
        }

        if (!TryParseHexColor(BrandPrimaryColor, out _) ||
            !TryParseHexColor(BrandAccentColor, out _) ||
            !TryParseHexColor(BrandBackgroundColor, out _))
        {
            return;
        }

        _studio.SetBrandKit(new BrandKit
        {
            Name = BrandName.Trim(),
            LogoText = BrandLogoText.Trim(),
            LogoAssetId = BrandKit.LogoAssetId,
            LogoAssetName = BrandKit.LogoAssetName,
            LogoAssetPath = BrandKit.LogoAssetPath,
            BrandColor = BrandPrimaryColor.Trim(),
            AccentColor = BrandAccentColor.Trim(),
            BackgroundColor = BrandBackgroundColor.Trim(),
            FontFamily = BrandKit.FontFamily,
            LowerThirdStyle = BrandLowerThirdStyle,
            CaptionStyle = CaptionStyleSummary,
            DefaultOverlayBehavior = BrandDefaultOverlayBehavior
        });
    }

    private static void SetPickerColorFromText(ref bool updating, Action update)
    {
        updating = true;
        try
        {
            update();
        }
        finally
        {
            updating = false;
        }
    }

    private static Color ParseHexColorOrDefault(string value, Color fallback) =>
        TryParseHexColor(value, out var color) ? color : fallback;

    private static bool TryParseHexColor(string? value, out Color color)
    {
        color = default;
        if (string.IsNullOrWhiteSpace(value))
        {
            return false;
        }

        var hex = value.Trim();
        if (hex.StartsWith('#'))
        {
            hex = hex[1..];
        }

        if (hex.Length != 6 ||
            !byte.TryParse(hex[..2], System.Globalization.NumberStyles.HexNumber, null, out var r) ||
            !byte.TryParse(hex.Substring(2, 2), System.Globalization.NumberStyles.HexNumber, null, out var g) ||
            !byte.TryParse(hex.Substring(4, 2), System.Globalization.NumberStyles.HexNumber, null, out var b))
        {
            return false;
        }

        color = Color.FromArgb(255, r, g, b);
        return true;
    }

    private static string ToHex(Color color) => $"#{color.R:X2}{color.G:X2}{color.B:X2}";
}
