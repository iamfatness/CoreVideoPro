using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Services;

public static class CaptionStyleHelper
{
    public static IReadOnlyList<string> FontSizeOptions { get; } = ["small", "medium", "large"];

    public static IReadOnlyList<string> LowerThirdPositionOptions { get; } = ["lower-left", "upper-left"];

    public static IReadOnlyList<string> CaptionPositionOptions { get; } = ["bottom", "top"];

    public static int FontSizeToPixels(string fontSize) => fontSize switch
    {
        "small" => 14,
        "large" => 24,
        _ => 18
    };

    public static int ClampOpacity(int value) => Math.Clamp(value, 0, 100);

    public static string FormatCaptionText(string text, bool uppercase) =>
        uppercase ? text.ToUpperInvariant() : text;

    public static string Summarize(CaptionStyleState style)
    {
        var caseLabel = style.Uppercase ? "uppercase" : "sentence case";
        return $"{style.FontSize} - {style.BackgroundOpacity}% backdrop - {caseLabel}";
    }

    public static SolidColorBrush CaptionTextBrush(string textColor)
    {
        var normalized = NormalizeHexColor(textColor, "#F7FBF8");
        return BrushFromHex(normalized);
    }

    public static SolidColorBrush CaptionBackgroundBrush(int opacityPercent)
    {
        var alpha = (byte)Math.Round(ClampOpacity(opacityPercent) / 100.0 * 255);
        return new SolidColorBrush(Windows.UI.Color.FromArgb(alpha, 12, 17, 24));
    }

    public static (HorizontalAlignment Horizontal, VerticalAlignment Vertical) LowerThirdAlignment(string position) =>
        position == "upper-left"
            ? (HorizontalAlignment.Left, VerticalAlignment.Top)
            : (HorizontalAlignment.Left, VerticalAlignment.Bottom);

    public static bool IsCaptionStripBelowProgram(string captionPosition) =>
        captionPosition != "top";

    private static string NormalizeHexColor(string value, string fallback)
    {
        var trimmed = value.Trim();
        if (!trimmed.StartsWith('#'))
        {
            trimmed = "#" + trimmed;
        }

        return trimmed.Length is 7 or 9 ? trimmed : fallback;
    }

    private static SolidColorBrush BrushFromHex(string hex)
    {
        hex = hex.TrimStart('#');
        if (hex.Length == 6)
        {
            hex = "FF" + hex;
        }

        var value = Convert.ToUInt32(hex, 16);
        return new SolidColorBrush(Windows.UI.Color.FromArgb(
            (byte)((value >> 24) & 0xFF),
            (byte)((value >> 16) & 0xFF),
            (byte)((value >> 8) & 0xFF),
            (byte)(value & 0xFF)));
    }
}