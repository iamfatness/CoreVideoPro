using System.Globalization;

namespace CoreVideoPro.MediaCore.Models;

/// <summary>One colour as the shared colour field edits it (graphics styles spec 4.2, slice 2).
/// Parses #RGB, #RRGGBB and #RRGGBBAA (the # is optional) and writes either the opaque
/// #RRGGBB form (fields that carry no alpha, such as a row appearance) or #RRGGBBAA.</summary>
public readonly record struct GraphicsRgba(byte R, byte G, byte B, byte A = 255)
{
    public static bool TryParse(string? text, out GraphicsRgba value)
    {
        value = default;
        if (!GraphicsPalette.TryNormalize(text, out var hex)) return false;
        byte At(int i) => byte.Parse(hex.AsSpan(i, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        value = new(At(1), At(3), At(5), At(7)); return true;
    }
    public string Hex6 => $"#{R:X2}{G:X2}{B:X2}";
    public string Hex8 => $"#{R:X2}{G:X2}{B:X2}{A:X2}";
    public string Format(bool alpha) => alpha ? Hex8 : Hex6;
    /// <summary>Alpha as a whole percent, 0..100.</summary>
    public int AlphaPercent => (int)Math.Round(A * 100 / 255.0, MidpointRounding.AwayFromZero);
    public GraphicsRgba WithAlphaPercent(double percent) => this with { A = (byte)Math.Round(Math.Clamp(percent, 0, 100) * 255 / 100, MidpointRounding.AwayFromZero) };

    /// <summary>Hue in degrees (0..360) and HSV saturation/value in 0..1.</summary>
    public (double Hue, double Saturation, double Value) Hsv
    {
        get {
            double r = R / 255d, g = G / 255d, b = B / 255d;
            double max = Math.Max(r, Math.Max(g, b)), min = Math.Min(r, Math.Min(g, b)), delta = max - min;
            double hue = delta == 0 ? 0 : max == r ? 60 * (((g - b) / delta) % 6) : max == g ? 60 * ((b - r) / delta + 2) : 60 * ((r - g) / delta + 4);
            if (hue < 0) hue += 360;
            return (hue, max == 0 ? 0 : delta / max, max);
        }
    }
}

/// <summary>The last twelve colours committed through any colour field, newest first, no
/// repeats (compared as #RRGGBBAA). Process-wide and in memory: it is a convenience, not show data.</summary>
public sealed class GraphicsColorRecents
{
    public const int Capacity = 12;
    public static GraphicsColorRecents Shared { get; } = new();
    private readonly List<string> _items = [];
    private readonly object _gate = new();
    public event EventHandler? Changed;
    public IReadOnlyList<string> Items { get { lock (_gate) return _items.ToArray(); } }
    public void Add(string colour)
    {
        if (!GraphicsRgba.TryParse(colour, out var value)) return;
        lock (_gate) {
            _items.Remove(value.Hex8); _items.Insert(0, value.Hex8);
            if (_items.Count > Capacity) _items.RemoveRange(Capacity, _items.Count - Capacity);
        }
        Changed?.Invoke(this, EventArgs.Empty);
    }
}

/// <summary>What a colour is used for, so the field can judge it (spec 4.2 "Checks").</summary>
public sealed record GraphicsColorCheckContext
{
    /// <summary>The colour is text drawn on this plate (#RRGGBB or #RRGGBBAA). Null when it is not text, or has no plate.</summary>
    public string? TextOnPlate { get; init; }
    /// <summary>The colour is text with no plate behind it (lyrics, minimal looks).</summary>
    public bool PlatelessText { get; init; }
    /// <summary>Text size in pixels on a 1080-line canvas, when the colour is a text colour.</summary>
    public double? TextSizePx { get; init; }
    /// <summary>The colour fills a large area (a plate, a full-frame card).</summary>
    public bool LargeArea { get; init; }
    /// <summary>The colour is a stroke of this width in pixels.</summary>
    public double? LineWidthPx { get; init; }
    /// <summary>The plate colour, when the colour being edited is a plate and the text on it is known.</summary>
    public string? TextOnThis { get; init; }
    public static GraphicsColorCheckContext None { get; } = new();
}

public sealed record GraphicsColorWarning(string Kind, string Message, string? FixValue = null, string? FixLabel = null);

/// <summary>Non-blocking on-air checks (spec 4.2): WCAG contrast below 4.5:1 for every on-air text
/// colour (stricter than WCAG's large-text 3:1 because compression and moving video lower
/// contrast), saturated red or blue text under 28 px (chroma bleed after 4:2:0), pure white over a
/// large area (offer a 235 cap) and 1 px lines (twitter). Never refuses a value.</summary>
public static class GraphicsColorChecks
{
    public const double MinimumContrast = 4.5;
    public const double SmallTextPx = 28;
    public const string CappedWhite = "#EBEBEB";

    public static IReadOnlyList<GraphicsColorWarning> Evaluate(string colour, GraphicsColorCheckContext context)
    {
        var warnings = new List<GraphicsColorWarning>();
        if (!GraphicsRgba.TryParse(colour, out var value)) return warnings;
        if (context.TextOnPlate is { } plateText && GraphicsRgba.TryParse(plateText, out var plate)) {
            var ratio = GraphicsPalette.Contrast(value.Hex8, plate.Hex8);
            if (ratio < MinimumContrast)
                warnings.Add(new("contrast", $"Text on this plate: {ratio.ToString("0.0", CultureInfo.InvariantCulture)}:1 — below 4.5:1 on air."));
        }
        if (context.TextOnThis is { } onThis && GraphicsRgba.TryParse(onThis, out var text)) {
            var ratio = GraphicsPalette.Contrast(text.Hex8, value.Hex8);
            if (ratio < MinimumContrast)
                warnings.Add(new("contrast", $"Text on this colour: {ratio.ToString("0.0", CultureInfo.InvariantCulture)}:1 — below 4.5:1 on air."));
        }
        if (context.PlatelessText) warnings.Add(new("contrast", "No plate behind this text: legibility depends on the video behind it."));
        if (context.TextSizePx is { } size && size < SmallTextPx && SaturatedRedOrBlue(value))
            warnings.Add(new("broadcast", $"Saturated {(IsRed(value) ? "red" : "blue")} text under {SmallTextPx:0} px smears after 4:2:0 chroma subsampling."));
        if (context.LargeArea && value is { R: 255, G: 255, B: 255 } && value.A > 0)
            warnings.Add(new("broadcast", "Pure white over a large area can exceed broadcast white.", CappedWhite + value.A.ToString("X2", CultureInfo.InvariantCulture), "Cap at 235"));
        if (context.LineWidthPx is > 0 and < 2)
            warnings.Add(new("broadcast", "1 px lines twitter on scaled or interlaced outputs. Use 2 px or more."));
        return warnings;
    }

    private static bool IsRed(GraphicsRgba value) { var hue = value.Hsv.Hue; return hue <= 15 || hue >= 345; }
    private static bool IsBlue(GraphicsRgba value) { var hue = value.Hsv.Hue; return hue is >= 210 and <= 255; }
    private static bool SaturatedRedOrBlue(GraphicsRgba value) { var (_, s, v) = value.Hsv; return s >= 0.8 && v >= 0.5 && (IsRed(value) || IsBlue(value)); }
}
