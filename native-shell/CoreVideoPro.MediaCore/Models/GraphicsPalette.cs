using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace CoreVideoPro.MediaCore.Models;

/// <summary>The show palette (graphics styles spec 4.1, owner rulings 2026-11-27): named colour
/// tokens stored in the show document, seeded from the app default palette for a new show.
/// Values are <c>#RRGGBBAA</c>. The on-colours may be <see cref="Auto"/>, which picks white or
/// near-black by contrast against their plate; the operator pins one by giving it a value.
/// Resolution for a declarative graphic is row override -> show palette -> pack default; the
/// pack default is the core's appearance derivation for any token this map does not set.
/// The Mac twin is <c>GraphicsPalette</c> in <c>mac-shell/.../GraphicsPalette.swift</c>;
/// <c>contracts/graphics-shell/palette.vectors.json</c> pins both to the same answers.</summary>
public sealed record GraphicsPalette
{
    public const string Auto = "auto";
    public const string Primary = "brand.primary", Secondary = "brand.secondary", Accent = "brand.accent", Surface = "brand.surface",
        OnPrimary = "brand.onPrimary", OnPrimaryMuted = "brand.onPrimaryMuted", OnSecondary = "brand.onSecondary", OnAccent = "brand.onAccent",
        TeamA = "brand.teamA", TeamB = "brand.teamB";
    /// <summary>Every token, in the order the Palette surface lists them.</summary>
    public static readonly IReadOnlyList<string> Tokens = [Primary, Secondary, Accent, Surface, OnPrimary, OnPrimaryMuted, OnSecondary, OnAccent, TeamA, TeamB];
    /// <summary>Tokens a palette must carry. The sports team colours are optional.</summary>
    public static readonly IReadOnlyList<string> Required = [Primary, Secondary, Accent, Surface, OnPrimary, OnPrimaryMuted, OnSecondary, OnAccent];
    /// <summary>On-colour -> the plate its automatic value is judged against.</summary>
    public static readonly IReadOnlyDictionary<string, string> OnColourPlates = new Dictionary<string, string>(StringComparer.Ordinal)
        { [OnPrimary] = Primary, [OnPrimaryMuted] = Primary, [OnSecondary] = Secondary, [OnAccent] = Accent };
    public const string White = "#FFFFFFFF", NearBlack = "#111111FF";
    /// <summary>The automatic second-line colour on a light-text plate: today's default title line.</summary>
    public const string MutedOnDark = "#E1E9F2FF", MutedOnLight = "#3A4350FF";

    public int Version { get; init; } = 1;
    public IReadOnlyDictionary<string, string> Colors { get; init; } = new Dictionary<string, string>(StringComparer.Ordinal);
    /// <summary>Keys a newer build wrote. Kept so a round trip never drops them.</summary>
    [JsonExtensionData] public Dictionary<string, JsonElement>? Metadata { get; init; }

    /// <summary>The stock app default: the look every built-in lower third had before the palette
    /// (#172433 plate at 90 %, white name, #E1E9F2 second line, #44C1A1 accent). <c>brand.secondary</c>
    /// keeps its pre-palette value (#E1E9F2) so templates that already use it draw unchanged.</summary>
    public static GraphicsPalette Default { get; } = new() {
        Colors = new Dictionary<string, string>(StringComparer.Ordinal) {
            [Primary] = "#172433E6", [Secondary] = "#E1E9F2FF", [Accent] = "#44C1A1FF", [Surface] = "#172433B3",
            [OnPrimary] = Auto, [OnPrimaryMuted] = Auto, [OnSecondary] = Auto, [OnAccent] = Auto }
    };

    [JsonIgnore] public bool IsValid => Version == 1 && Colors is { } colors && colors.Count <= Tokens.Count &&
        Required.All(colors.ContainsKey) &&
        colors.All(p => Tokens.Contains(p.Key, StringComparer.Ordinal) && p.Value is not null &&
            (IsColor(p.Value) || (p.Value == Auto && OnColourPlates.ContainsKey(p.Key))));

    /// <summary>Strict stored form: <c>#RRGGBBAA</c>.</summary>
    public static bool IsColor(string? value) => value is { Length: 9 } && value[0] == '#' && value.AsSpan(1).IndexOfAnyExcept("0123456789abcdefABCDEF") < 0;

    /// <summary>Operator input to the stored form. Accepts #RGB, #RRGGBB and #RRGGBBAA (the # is
    /// optional); output is upper-case #RRGGBBAA, opaque unless alpha was given.</summary>
    public static bool TryNormalize(string? text, out string value)
    {
        value = "";
        if (string.IsNullOrWhiteSpace(text)) return false;
        var hex = text.Trim(); if (hex.StartsWith('#')) hex = hex[1..];
        if (hex.AsSpan().IndexOfAnyExcept("0123456789abcdefABCDEF") >= 0) return false;
        hex = hex.Length switch {
            3 => string.Concat(hex.Select(c => new string(c, 2))) + "FF",
            6 => hex + "FF",
            8 => hex,
            _ => ""
        };
        if (hex.Length != 8) return false;
        value = "#" + hex.ToUpperInvariant(); return true;
    }

    /// <summary>An appearance colour (#RRGGBB) with an opacity as a palette value.</summary>
    public static string FromHex(string rrggbb, double opacity = 1)
    {
        if (!TryNormalize(rrggbb, out var value) || rrggbb.Trim().TrimStart('#').Length != 6) throw new ArgumentException("Expected #RRGGBB.", nameof(rrggbb));
        var alpha = (int)Math.Round(Math.Clamp(opacity, 0, 1) * 255, MidpointRounding.AwayFromZero);
        return value[..7] + alpha.ToString("X2", CultureInfo.InvariantCulture);
    }

    /// <summary>WCAG 2.1 contrast ratio of two colours' RGB (alpha is ignored: the plate's
    /// backdrop is live video, which this check cannot know).</summary>
    public static double Contrast(string a, string b)
    {
        double L(string c) {
            double Channel(int offset) {
                var s = int.Parse(c.AsSpan(offset, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture) / 255d;
                return s <= 0.04045 ? s / 12.92 : Math.Pow((s + 0.055) / 1.055, 2.4);
            }
            return 0.2126 * Channel(1) + 0.7152 * Channel(3) + 0.0722 * Channel(5);
        }
        var (x, y) = (L(a), L(b));
        return (Math.Max(x, y) + 0.05) / (Math.Min(x, y) + 0.05);
    }

    /// <summary>White or near-black, whichever contrasts more with <paramref name="plate"/> (ties go to white).</summary>
    public static string AutoOn(string plate) => Contrast(White, plate) >= Contrast(NearBlack, plate) ? White : NearBlack;

    public GraphicsPalette With(string token, string value)
    {
        var colors = Colors.ToDictionary(p => p.Key, p => p.Value, StringComparer.Ordinal);
        if (value.Length == 0 && !Required.Contains(token, StringComparer.Ordinal)) colors.Remove(token); else colors[token] = value;
        return this with { Colors = colors };
    }

    /// <summary>Row override -> this palette. Returns a concrete #RRGGBBAA for every required token
    /// (automatic on-colours computed against their resolved plate) plus any team colour set by
    /// either layer. Tokens absent from the result fall to the pack default in the core.</summary>
    public IReadOnlyDictionary<string, string> Resolve(IReadOnlyDictionary<string, string>? rowOverrides = null)
    {
        var resolved = new Dictionary<string, string>(StringComparer.Ordinal);
        string? Layer(string token) => rowOverrides is not null && rowOverrides.TryGetValue(token, out var o) && IsColor(o) ? o.ToUpperInvariant()
            : Colors.TryGetValue(token, out var v) ? (v == Auto ? Auto : IsColor(v) ? v.ToUpperInvariant() : null) : null;
        foreach (var token in new[] { Primary, Secondary, Accent, Surface, TeamA, TeamB })
            if (Layer(token) is { } value && value != Auto) resolved[token] = value;
        foreach (var token in new[] { OnPrimary, OnSecondary, OnAccent }) {
            var value = Layer(token) ?? Auto;
            if (value == Auto) { if (!resolved.TryGetValue(OnColourPlates[token], out var plate)) continue; value = AutoOn(plate); }
            resolved[token] = value;
        }
        var muted = Layer(OnPrimaryMuted) ?? Auto;
        if (muted == Auto && resolved.TryGetValue(OnPrimary, out var name)) muted = Contrast(name, White) <= Contrast(name, NearBlack) ? MutedOnDark : MutedOnLight;
        if (muted != Auto) resolved[OnPrimaryMuted] = muted;
        return resolved;
    }

    /// <summary>Normalized, validated row overrides: known tokens with #RRGGBBAA values. Null when invalid.</summary>
    public static bool ValidOverrides(IReadOnlyDictionary<string, string>? overrides) => overrides is null ||
        (overrides.Count <= Tokens.Count && overrides.All(p => Tokens.Contains(p.Key, StringComparer.Ordinal) && IsColor(p.Value)));

    public static bool SameResolved(IReadOnlyDictionary<string, string>? a, IReadOnlyDictionary<string, string>? b) =>
        (a?.Count ?? 0) == (b?.Count ?? 0) && (a is null || a.All(p => b!.TryGetValue(p.Key, out var v) && string.Equals(v, p.Value, StringComparison.OrdinalIgnoreCase)));

    /// <summary>The palette map the core reported for a revision (null or an object of strings).</summary>
    public static IReadOnlyDictionary<string, string>? FromWire(JsonElement? wire) =>
        wire is { ValueKind: JsonValueKind.Object } value
            ? value.EnumerateObject().Where(p => p.Value.ValueKind == JsonValueKind.String).ToDictionary(p => p.Name, p => p.Value.GetString()!, StringComparer.Ordinal)
            : null;
}

/// <summary>Moves a pre-palette show onto the palette without changing anything already built
/// (spec 4.1 "Migration from the existing brand kit into the palette").</summary>
public static class GraphicsPaletteMigration
{
    /// <summary>The app default seeded from the legacy brand kit. A brand kit still on its stock
    /// values (never edited) seeds <see cref="GraphicsPalette.Default"/>, the look graphics had;
    /// an edited kit maps Background -> primary (90 % plate) and surface (70 %), Brand ->
    /// secondary and Accent -> accent, with automatic on-colours.</summary>
    public static GraphicsPalette FromBrandKit(string brandColor, string accentColor, string backgroundColor, bool stock)
    {
        if (stock || !Opaque(brandColor) || !Opaque(accentColor) || !Opaque(backgroundColor)) return GraphicsPalette.Default;
        return new GraphicsPalette {
            Colors = new Dictionary<string, string>(StringComparer.Ordinal) {
                [GraphicsPalette.Primary] = GraphicsPalette.FromHex(backgroundColor, .9), [GraphicsPalette.Secondary] = GraphicsPalette.FromHex(brandColor),
                [GraphicsPalette.Accent] = GraphicsPalette.FromHex(accentColor), [GraphicsPalette.Surface] = GraphicsPalette.FromHex(backgroundColor, .7),
                [GraphicsPalette.OnPrimary] = GraphicsPalette.Auto, [GraphicsPalette.OnPrimaryMuted] = GraphicsPalette.Auto,
                [GraphicsPalette.OnSecondary] = GraphicsPalette.Auto, [GraphicsPalette.OnAccent] = GraphicsPalette.Auto }
        };
        static bool Opaque(string value) => value?.Trim().TrimStart('#') is { Length: 6 } hex && hex.AsSpan().IndexOfAnyExcept("0123456789abcdefABCDEF") < 0;
    }

    /// <summary>The brand values a declarative row drew before the palette, from its appearance.</summary>
    public static IReadOnlyDictionary<string, string> AppearanceTokens(LowerThirdAppearance style) => new Dictionary<string, string>(StringComparer.Ordinal) {
        [GraphicsPalette.Primary] = GraphicsPalette.FromHex(style.BackgroundColor, style.BackgroundOpacity),
        [GraphicsPalette.OnPrimary] = GraphicsPalette.FromHex(style.NameColor),
        [GraphicsPalette.OnPrimaryMuted] = GraphicsPalette.FromHex(style.TitleColor),
        [GraphicsPalette.Accent] = GraphicsPalette.FromHex(style.AccentColor) };

    /// <summary>Row overrides that keep a declarative row looking exactly as it did: every
    /// appearance colour that differs from what <paramref name="palette"/> resolves to. A row
    /// already in the palette's colours gets none and follows the palette from now on.
    /// Compatibility looks and rows that already carry overrides are returned unchanged.</summary>
    public static GraphicsPlaylistRow MigrateRow(GraphicsPlaylistRow row, GraphicsPalette palette)
    {
        if (row.BrandOverrides is not null || row.TemplateId.Length == 0 || !Declarative(row.TemplateId) || !row.Style.IsValid) return row;
        var resolved = palette.Resolve();
        var overrides = AppearanceTokens(row.Style).Where(p => !resolved.TryGetValue(p.Key, out var v) || !string.Equals(v, p.Value, StringComparison.OrdinalIgnoreCase))
            .ToDictionary(p => p.Key, p => p.Value, StringComparer.Ordinal);
        return overrides.Count == 0 ? row : row with { BrandOverrides = overrides };
    }

    public static bool Declarative(string templateId) => templateId is not ("compact-solid" or "minimal-accent" or "broadcast");
}
