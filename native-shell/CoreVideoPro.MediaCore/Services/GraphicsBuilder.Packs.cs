using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>A built-in style pack as the Builder's style strip shows it: its templates for one kind.</summary>
public sealed record GraphicsBuilderPack(string Id, string Name, string Genre, IReadOnlyList<NativeGraphicsTemplate> Templates)
{
    /// <summary>The strip label: "News" for "News: Signal".</summary>
    public string Label => Name.IndexOf(':') is > 0 and var colon ? Name[..colon].Trim() : Name;
}

/// <summary>Graphics styles slice 11: the Builder reshaped around style packs (spec section 5). Pure decisions
/// only (no UI, no native calls): which packs and looks a kind offers, whether a card's thumbnail is current
/// for the show palette, the operator knobs every pack exposes (colour roles, position, safe-area snapping,
/// size, motion speed, optional elements) and the names-list "Add speaker".</summary>
public static partial class GraphicsBuilder
{
    /// <summary>The style id for looks that are not in a pack (curated, imported, compatibility).</summary>
    public const string ClassicStyleId = "classic";
    /// <summary>Spec 6.3 launch order; any other genre follows, by name.</summary>
    public static IReadOnlyList<string> PackGenreOrder { get; } = ["news", "sports", "worship", "corporate", "esports", "talk", "minimal"];

    /// <summary>Installed packs with at least one look for <paramref name="kind"/>, in launch order. Each pack's
    /// templates are its newest versions for that family, one-line before two-line before tagged, then by name.</summary>
    public static IReadOnlyList<GraphicsBuilderPack> PacksFor(GraphicsBuilderKind kind, NativeGraphicsState? state)
    {
        if (state is null || kind.Family.Length == 0) return [];
        return TemplatesFor(kind, state).Where(t => t.Pack is { Id.Length: > 0 })
            .GroupBy(t => t.Pack!.Id, StringComparer.Ordinal)
            .Select(g => new GraphicsBuilderPack(g.Key, g.First().Pack!.Name.Length > 0 ? g.First().Pack!.Name : g.Key, g.First().Pack!.Genre,
                g.OrderBy(LookRank).ThenBy(t => t.Name, StringComparer.CurrentCultureIgnoreCase).ToArray()))
            .OrderBy(p => GenreRank(p.Genre))
            .ThenBy(p => p.Name, StringComparer.CurrentCultureIgnoreCase).ToArray();
    }
    private static int GenreRank(string genre) { for (var i = 0; i < PackGenreOrder.Count; i++) if (PackGenreOrder[i] == genre) return i; return int.MaxValue; }
    private static int LookRank(NativeGraphicsTemplate t) => !HasSecondLine(t) && !HasTag(t) ? 0 : HasSecondLine(t) && !HasTag(t) ? 1 : HasTag(t) ? 2 : 3;

    /// <summary>Looks for <paramref name="kind"/> that are not in a pack.</summary>
    public static IReadOnlyList<NativeGraphicsTemplate> ClassicTemplatesFor(GraphicsBuilderKind kind, NativeGraphicsState? state) =>
        TemplatesFor(kind, state).Where(t => t.Pack is null).ToArray();

    public static string StyleOf(NativeGraphicsTemplate? template) => template?.Pack is { Id.Length: > 0 } pack ? pack.Id : ClassicStyleId;

    /// <summary>The look a new graphic of <paramref name="kind"/> starts on: the first pack's two-line lower third
    /// (or its first look), else the first classic look. Null when nothing is installed.</summary>
    public static NativeGraphicsTemplate? DefaultLookFor(GraphicsBuilderKind kind, NativeGraphicsState? state)
    {
        var packs = PacksFor(kind, state);
        if (packs.Count > 0) return FirstLookOf(packs[0], kind);
        return TemplatesFor(kind, state).FirstOrDefault();
    }
    public static NativeGraphicsTemplate FirstLookOf(GraphicsBuilderPack pack, GraphicsBuilderKind kind) =>
        (kind.Family == "lower-third" ? pack.Templates.FirstOrDefault(t => HasSecondLine(t) && !HasTag(t) && IsPlainLowerThird(t)) : null) ?? pack.Templates[0];

    /// <summary>True when a card's thumbnail shows <paramref name="palette"/>: compatibility presets always (they
    /// ignore the palette); a declarative look only when the core rendered it under the current thumbnail palette
    /// revision and that palette is the one the Builder asked for. Anything else is "recolouring", never current.</summary>
    public static bool ThumbnailCurrent(NativeGraphicsTemplate template, NativeGraphicsState? state, IReadOnlyDictionary<string, string>? palette)
    {
        if (template.Runtime != "declarative") return true;
        if (template.Thumbnail is not { State: "ready" or "unavailable" or "unsupported", PaletteRevision: { } revision } || state?.TemplateThumbnailPalette is not { } current) return false;
        return revision == current.Revision && GraphicsPalette.SameResolved(current.Palette is { Count: > 0 } p ? p : null, palette is { Count: > 0 } q ? q : null);
    }

    // ---- optional elements -----------------------------------------------------------------
    public static bool HasSecondLine(NativeGraphicsTemplate t) => t.Fields.Any(f => f.Id == "title");
    public static bool HasTag(NativeGraphicsTemplate t) => t.Fields.Any(f => f.Id == "tag");
    /// <summary>A name / second line / tag lower third, nothing else to fill in (so it has siblings to switch to).</summary>
    public static bool IsPlainLowerThird(NativeGraphicsTemplate t) => t.Family == "lower-third" && t.Fields.Count > 0 &&
        t.Fields.All(f => f.Id is "name" or "title" or "tag") && t.Fields.Any(f => f.Id == "name");

    /// <summary>Optional elements are sibling looks in the same pack (spec 5: tag / second line): the one-line,
    /// two-line and name + tag lower thirds. Returns the pack look with exactly the asked elements, or null.</summary>
    public static NativeGraphicsTemplate? Sibling(GraphicsBuilderPack pack, NativeGraphicsTemplate current, bool secondLine, bool tag)
    {
        if (!IsPlainLowerThird(current)) return null;
        if (HasSecondLine(current) == secondLine && HasTag(current) == tag) return current;
        return pack.Templates.Where(t => t.Family == current.Family && IsPlainLowerThird(t) && HasSecondLine(t) == secondLine && HasTag(t) == tag)
            .OrderBy(t => t.Fields.Count).ThenBy(t => t.TemplateId, StringComparer.Ordinal).FirstOrDefault();
    }

    /// <summary>The draft's typed values carried onto <paramref name="to"/>: every field the new look also has keeps
    /// its value; the rest take the new look's defaults.</summary>
    public static JsonElement CarryFields(JsonElement from, NativeGraphicsTemplate to, JsonElement? into = null)
    {
        var values = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        if (into is { ValueKind: JsonValueKind.Object } start) foreach (var p in start.EnumerateObject()) values[p.Name] = p.Value.Clone();
        else foreach (var field in to.Fields) if (field.Default is { } fallback) values[field.Id] = fallback.Clone();
        // Only declared text fields travel; a compatibility look (no field list) keeps what it started with.
        foreach (var field in to.Fields)
            if (field.Type == "string" && from.ValueKind == JsonValueKind.Object && from.TryGetProperty(field.Id, out var typed) && typed.ValueKind == JsonValueKind.String)
                values[field.Id] = typed.Clone();
        return JsonSerializer.SerializeToElement(values);
    }

    // ---- colours by role -------------------------------------------------------------------
    private static readonly IReadOnlyDictionary<string, string> RoleLabels = new Dictionary<string, string>(StringComparer.Ordinal) {
        [GraphicsPalette.Primary] = "Plate", [GraphicsPalette.Secondary] = "Second plate", [GraphicsPalette.Accent] = "Accent",
        [GraphicsPalette.Surface] = "Surface", [GraphicsPalette.OnPrimary] = "Name text", [GraphicsPalette.OnPrimaryMuted] = "Second line text",
        [GraphicsPalette.OnSecondary] = "Title text", [GraphicsPalette.OnAccent] = "Tag text", [GraphicsPalette.TeamA] = "Team A", [GraphicsPalette.TeamB] = "Team B" };

    /// <summary>The palette colour roles a look actually draws with (its declared brand tokens and every
    /// <c>{"token": ...}</c> in its layers), in palette order, with operator labels.</summary>
    public static IReadOnlyList<(string Token, string Label)> ColourRoles(NativeGraphicsTemplate template)
    {
        var used = new HashSet<string>(template.BrandTokens, StringComparer.Ordinal);
        void Scan(JsonElement element, int depth) {
            if (depth > 16) return;
            if (element.ValueKind == JsonValueKind.Object) {
                foreach (var property in element.EnumerateObject()) {
                    if (property.Name == "token" && property.Value.ValueKind == JsonValueKind.String) used.Add(property.Value.GetString()!);
                    else Scan(property.Value, depth + 1);
                }
            } else if (element.ValueKind == JsonValueKind.Array) foreach (var item in element.EnumerateArray()) Scan(item, depth + 1);
        }
        if (template.Layers is { } layers) Scan(layers, 0);
        else if (template.Definition is { } definition && definition.ValueKind == JsonValueKind.Object && definition.TryGetProperty("layers", out var defined)) Scan(defined, 0);
        return GraphicsPalette.Tokens.Where(used.Contains).Select(t => (t, RoleLabels[t])).ToArray();
    }

    // ---- position, safe area, size ---------------------------------------------------------
    /// <summary>Graphics-safe at 1080p (90 %, spec 5 / EBU R 95): x 96-1824, y 54-1026. The lower-third band is y 820-1000.</summary>
    public const double SafeLeft = 96, SafeRight = 1824, SafeTop = 54, SafeBottom = 1026, BandTop = 820, BandBottom = 1000;
    public static IReadOnlyList<(string Id, string Label)> PositionChoices { get; } = [("left", "Left"), ("center", "Centre"), ("right", "Right")];
    public static IReadOnlyList<(string Id, string Label, double Scale)> SizeChoices { get; } = [("s", "S", .85), ("m", "M", 1), ("l", "L", 1.15)];
    /// <summary>Motion speed multipliers (core <c>animations.speed</c>).</summary>
    public static IReadOnlyList<(string Label, double Speed)> SpeedChoices { get; } = [("0.5×", .5), ("0.75×", .75), ("1.0×", 1), ("1.5×", 1.5), ("2.0×", 2)];

    public static (double Width, double Height) BoundsOf(NativeGraphicsTemplate template)
    {
        static double Read(JsonElement? box, string name) =>
            box is { ValueKind: JsonValueKind.Object } b && b.TryGetProperty(name, out var v) && v.TryGetDouble(out var d) && double.IsFinite(d) && d > 0 ? d : 0;
        var w = Read(template.Bounds, "width"); var h = Read(template.Bounds, "height");
        if (w <= 0 || h <= 0) { w = 1000; h = 150; }
        return (Math.Min(w, 1920), Math.Min(h, 1080));
    }

    private static (double X, double Y) AnchorFraction(string anchor) =>
        (anchor.EndsWith("right", StringComparison.Ordinal) ? 1 : anchor.EndsWith("left", StringComparison.Ordinal) ? 0 : .5,
         anchor.StartsWith("upper", StringComparison.Ordinal) ? 0 : anchor.StartsWith("lower", StringComparison.Ordinal) ? 1 : .5);

    /// <summary>left / center / right from a placement's anchor.</summary>
    public static string PositionOf(NativeGraphicsPlacement? placement) =>
        placement?.Anchor is { } a ? a.EndsWith("right", StringComparison.Ordinal) ? "right" : a.EndsWith("left", StringComparison.Ordinal) ? "left" : "center" : "left";

    /// <summary>The size choice nearest a placement's scale.</summary>
    public static string SizeOf(NativeGraphicsPlacement? placement) =>
        SizeChoices.OrderBy(c => Math.Abs(c.Scale - (placement?.Scale ?? 1))).First().Id;

    /// <summary>Where a look sits for a position and size (spec 5). A lower third sits on the band's bottom edge
    /// (y 1000) at the left or right graphics-safe edge or centred; other looks keep their default row (top, middle,
    /// bottom) and take the safe edge horizontally. With <paramref name="snap"/> the scaled box is kept inside
    /// graphics-safe (the scale shrinks first if the look cannot fit).</summary>
    public static NativeGraphicsPlacement PlaceInSafeArea(NativeGraphicsTemplate template, string position, double scale, bool snap)
    {
        var (w, h) = BoundsOf(template);
        var row = template.Family == "lower-third" ? "lower" : (template.DefaultPlacement?.Anchor ?? "lower-left") switch {
            var a when a.StartsWith("upper", StringComparison.Ordinal) => "upper",
            var a when a.StartsWith("lower", StringComparison.Ordinal) => "lower",
            _ => "center" };
        var column = position is "right" ? "right" : position is "center" ? "center" : "left";
        var anchor = row == "center" ? (column == "center" ? "center" : "center-" + column) : row + "-" + column;
        var x = column == "left" ? SafeLeft : column == "right" ? -(1920 - SafeRight) : 0;
        var y = row == "upper" ? SafeTop : row == "center" ? 0 : template.Family == "lower-third" ? -(1080 - BandBottom) : -(1080 - SafeBottom);
        var placement = new NativeGraphicsPlacement { Anchor = anchor, X = x, Y = y, Scale = Math.Round(Math.Clamp(scale, .1, 4), 3) };
        return snap && SnapsToSafeArea(template) ? ClampToSafeArea(placement, w, h) : placement;
    }

    /// <summary>Full-width bars (a ticker, a full-width title: bounds 1920 wide) run edge to edge by design and are
    /// never snapped or shrunk; every other look is kept inside graphics-safe when Safe area is on.</summary>
    public static bool SnapsToSafeArea(NativeGraphicsTemplate template) => BoundsOf(template).Width < 1920;

    /// <summary>The placement moved (and scaled down if needed) so the look's scaled box lies inside graphics-safe.
    /// Uses the compositor's placement rule: the anchor point of the box sits at the anchor point of the frame plus (x, y).</summary>
    public static NativeGraphicsPlacement ClampToSafeArea(NativeGraphicsPlacement placement, double width, double height)
    {
        const double safeWidth = SafeRight - SafeLeft, safeHeight = SafeBottom - SafeTop;
        var scale = Math.Min(placement.Scale, Math.Min(safeWidth / width, safeHeight / height));
        scale = Math.Round(Math.Max(.1, scale), 3, MidpointRounding.ToZero);
        var (ax, ay) = AnchorFraction(placement.Anchor);
        double sw = width * scale, sh = height * scale;
        var left = ax * (1920 - sw) + placement.X; var top = ay * (1080 - sh) + placement.Y;
        left = Math.Clamp(left, SafeLeft, SafeRight - sw); top = Math.Clamp(top, SafeTop, SafeBottom - sh);
        return placement with { X = Math.Round(left - ax * (1920 - sw), 1), Y = Math.Round(top - ay * (1080 - sh), 1), Scale = scale };
    }

    /// <summary>True when the scaled box lies inside graphics-safe (half a pixel of rounding allowed).</summary>
    public static bool InsideSafeArea(NativeGraphicsPlacement placement, double width, double height)
    {
        var (ax, ay) = AnchorFraction(placement.Anchor);
        double sw = width * placement.Scale, sh = height * placement.Scale;
        var left = ax * (1920 - sw) + placement.X; var top = ay * (1080 - sh) + placement.Y;
        return left >= SafeLeft - .5 && left + sw <= SafeRight + .5 && top >= SafeTop - .5 && top + sh <= SafeBottom + .5;
    }

    // ---- motion ----------------------------------------------------------------------------
    /// <summary>What the instance plays for a pack look: its own authored motion (layered, "own") unless a preset
    /// was picked, at <paramref name="speed"/>. Mirrors <see cref="GraphicsPlayoutService"/> so the private
    /// preview plays exactly what Program will.</summary>
    public static NativeGraphicsAnimations EffectiveMotion(NativeGraphicsTemplate template, NativeGraphicsAnimations? rowMotion, double? speed, NativeGraphicsAnimations playoutDefault)
    {
        var motion = rowMotion is { } chosen ? chosen with { Layers = "whole" }
            : template.Pack is not null || GraphicsDesignIds.IsDesignTemplateId(template.TemplateId) ? template.Animations with { Layers = "own" } : playoutDefault;
        return motion with { Speed = speed is { } s && s != 1 ? s : null };
    }

    // ---- variants ("Add speaker") ----------------------------------------------------------
    /// <summary>"Add speaker" (spec 5 Variants): the names list with one more line for <paramref name="name"/>,
    /// bound to the same design. A tag goes in a <c>tag</c> column when the look has one. Unchanged when the
    /// name is empty or the list already holds 64 people.</summary>
    public static string AddSpeaker(string namesText, string name, string title, string? tag, IReadOnlyCollection<string>? variableIds)
    {
        name = name.Trim(); title = title.Trim(); tag = tag?.Trim();
        if (name.Length == 0) return namesText;
        var variants = ParseNamesList(namesText, null, variableIds).ToList();
        if (variants.Count >= 64) return namesText;
        var cells = new Dictionary<string, string>(StringComparer.Ordinal);
        if (tag is { Length: > 0 } && (variableIds is null || variableIds.Contains("tag"))) cells["tag"] = tag;
        variants.Add(new() { Id = NewRowId(name, []), Name = name.Length > 128 ? name[..128] : name, Title = title.Length > 256 ? title[..256] : title, Cells = cells });
        return NamesListText(variants);
    }

    /// <summary>The speakers a names list holds, in order (the Variants chips).</summary>
    public static IReadOnlyList<string> Speakers(string namesText, IReadOnlyCollection<string>? variableIds = null) =>
        ParseNamesList(namesText, null, variableIds).Select(v => v.Name).ToArray();
}
