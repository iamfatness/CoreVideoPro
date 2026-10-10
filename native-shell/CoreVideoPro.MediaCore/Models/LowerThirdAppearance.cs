namespace CoreVideoPro.MediaCore.Models;

/// <summary>Shared look; text stays on its source. Pixel sizes reference a 1080-line canvas.</summary>
public sealed record LowerThirdAppearance
{
    public int Version { get; init; } = 1;
    public string Preset { get; init; } = "compact-solid";
    public string Anchor { get; init; } = "lower-left";
    public string FontFamily { get; init; } = "Segoe UI";
    public double NameSize { get; init; } = 42;
    public double TitleSize { get; init; } = 28;
    public string NameColor { get; init; } = "#F4F7FA";
    public string TitleColor { get; init; } = "#44C1A1";
    public string BackgroundColor { get; init; } = "#0C1118";
    public string AccentColor { get; init; } = "#44C1A1";
    public double BackgroundOpacity { get; init; } = .9;
    public double Padding { get; init; } = 20;
    public double Width { get; init; } = .6;
    public double CornerRadius { get; init; } = 8;
    public double SafeOffsetX { get; init; } = .05;
    public double SafeOffsetY { get; init; } = .06;
    public bool ShowLogo { get; init; } = true;
    public double LogoScale { get; init; } = 1;
    public bool IsValid => Version==1 && (Preset is "compact-solid" or "minimal-accent" or "broadcast") && (Anchor is "lower-left" or "lower-right" or "upper-left" or "upper-right") &&
        !string.IsNullOrWhiteSpace(FontFamily) && FontFamily.Length<=128 &&
        In(NameSize,16,120) && In(TitleSize,12,80) && In(BackgroundOpacity,0,1) && In(Padding,0,64) &&
        In(Width,.15,.95) && In(CornerRadius,0,60) && In(SafeOffsetX,0,.25) && In(SafeOffsetY,0,.25) &&
        In(LogoScale,.5,2) && Color(NameColor) && Color(TitleColor) && Color(BackgroundColor) && Color(AccentColor);
    private static bool In(double v,double min,double max) => double.IsFinite(v) && v>=min && v<=max;
    private static bool Color(string color) => color is { Length:7 } && color[0]=='#' && color.AsSpan(1).ToArray().All(Uri.IsHexDigit);
    public static LowerThirdAppearance FromPreset(string id) => id switch {
        "minimal-accent" => new() { Preset=id, BackgroundOpacity=0, NameSize=40, TitleSize=26, CornerRadius=0 },
        "broadcast" => new() { Preset=id, NameSize=52, TitleSize=32, Padding=24, Width=.75, CornerRadius=0, BackgroundOpacity=1 },
        _ => new()
    };
}
