using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Maps media-core snapshot fields into studio operator readouts (color, brand kit).
/// </summary>
public static class ProductionReadoutSync
{
    public sealed record ColorGradePatch(
        string Lut,
        int Exposure,
        int Contrast,
        int Saturation,
        int Temperature);

    public sealed record BrandKitPatch(
        string Name,
        string LogoText,
        string? LogoAssetId,
        string? LogoAssetName,
        string? LogoAssetPath,
        string BrandColor,
        string AccentColor,
        string BackgroundColor,
        string FontFamily,
        string LowerThirdStyle,
        string CaptionStyle,
        string DefaultOverlayBehavior);

    public static ColorGradePatch? ResolveColorGradePatch(
        NativeMediaCoreStateSnapshot snapshot,
        bool engineRunning)
    {
        if (!engineRunning)
        {
            return null;
        }

        var grade = snapshot.RenderPlan.ColorGrade;
        return new ColorGradePatch(
            string.IsNullOrWhiteSpace(grade.Lut) ? "none" : grade.Lut.Trim(),
            (int)Math.Round(grade.Exposure),
            (int)Math.Round(grade.Contrast),
            (int)Math.Round(grade.Saturation),
            (int)Math.Round(grade.Temperature));
    }

    public static BrandKitPatch? ResolveBrandKitPatch(
        NativeMediaCoreStateSnapshot snapshot,
        bool engineRunning)
    {
        if (!engineRunning)
        {
            return null;
        }

        var brandKit = snapshot.BrandKit;
        if (brandKit is null ||
            string.IsNullOrWhiteSpace(brandKit.Name) ||
            brandKit.Summary.Equals("Idle", StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }

        return new BrandKitPatch(
            brandKit.Name.Trim(),
            string.IsNullOrWhiteSpace(brandKit.LogoText) ? brandKit.Name.Trim() : brandKit.LogoText.Trim(),
            brandKit.LogoAssetId,
            brandKit.LogoAssetName,
            brandKit.LogoAssetPath,
            NormalizeColor(brandKit.BrandColor, "#44c1a1"),
            NormalizeColor(brandKit.AccentColor, "#f0a85c"),
            NormalizeColor(brandKit.BackgroundColor, "#0c1118"),
            string.IsNullOrWhiteSpace(brandKit.FontFamily) ? "Inter" : brandKit.FontFamily.Trim(),
            string.IsNullOrWhiteSpace(brandKit.LowerThirdStyle) ? "gradient" : brandKit.LowerThirdStyle.Trim(),
            string.IsNullOrWhiteSpace(brandKit.CaptionStyle) ? "medium sentence captions" : brandKit.CaptionStyle.Trim(),
            string.IsNullOrWhiteSpace(brandKit.DefaultOverlayBehavior) ? "all-off" : brandKit.DefaultOverlayBehavior.Trim());
    }

    private static string NormalizeColor(string? value, string fallback) =>
        string.IsNullOrWhiteSpace(value) ? fallback : value.Trim();
}
