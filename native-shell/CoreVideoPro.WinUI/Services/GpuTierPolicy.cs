namespace CoreVideoPro.WinUI.Services;

/// <summary>Dedicated-VRAM tier that drives canvas and multiview defaults (#439, owner ruling 2026-10-23).</summary>
public enum GpuVramTier
{
    /// <summary>VRAM could not be measured. Treated as <see cref="Under8Gb"/> with a visible notice.</summary>
    Unknown,
    Under8Gb,
    From8To16Gb,
    AtLeast16Gb
}

/// <summary>
/// Pure policy: maps the <see cref="GpuTierProbe"/> band string (e.g.
/// <c>nvidia-vram8-12gb</c>) to the default canvas, the offered canvas ceiling and
/// the multiview tile ceiling. It only ever supplies DEFAULTS and the OFFERED
/// ceiling; it never rewrites a value the operator already saved
/// (see <see cref="ResolveCanvas"/> / <see cref="ResolveTileCount"/>).
/// Nothing here is a per-unit identifier: the tier is the same coarse band the
/// telemetry payload already carries.
/// </summary>
public sealed record GpuTierPolicy(
    GpuVramTier Tier,
    string DefaultCanvas,
    string CanvasCeiling,
    int DefaultTileCount,
    int TileCeiling,
    string? Notice)
{
    public const string Canvas1080 = "1920x1080";
    public const string Canvas4K = "3840x2160";

    /// <summary>
    /// Tile capacity the product can actually render today (the 10 Show Input
    /// slots). The tier ceilings of 16 and 25 are the ruled policy; the
    /// effective ceiling is min(tier ceiling, this) until the wall supports more.
    /// </summary>
    public const int ProductTileCapacity = 10;

    public const string UnknownVramNotice =
        "GPU memory could not be measured, so canvas and multiview limits use the under-8 GB tier.";

    public int EffectiveTileCeiling => Math.Min(TileCeiling, ProductTileCapacity);
    public int EffectiveDefaultTileCount => Math.Min(DefaultTileCount, ProductTileCapacity);

    public string TierLabel => Tier switch
    {
        GpuVramTier.Under8Gb => "under-8gb",
        GpuVramTier.From8To16Gb => "8-16gb",
        GpuVramTier.AtLeast16Gb => "16gb+",
        _ => "unknown"
    };

    public static GpuVramTier TierFromBand(string? band)
    {
        if (string.IsNullOrWhiteSpace(band))
        {
            return GpuVramTier.Unknown;
        }

        var text = band.Trim().ToLowerInvariant();
        // Check the longer/ambiguous suffixes first; "vram16gb+" must not match "vram<2gb" etc.
        if (text.EndsWith("vram16gb+", StringComparison.Ordinal)) return GpuVramTier.AtLeast16Gb;
        if (text.EndsWith("vram12-16gb", StringComparison.Ordinal)
            || text.EndsWith("vram8-12gb", StringComparison.Ordinal)) return GpuVramTier.From8To16Gb;
        if (text.EndsWith("vram4-8gb", StringComparison.Ordinal)
            || text.EndsWith("vram2-4gb", StringComparison.Ordinal)
            || text.EndsWith("vram<2gb", StringComparison.Ordinal)) return GpuVramTier.Under8Gb;
        return GpuVramTier.Unknown;
    }

    public static GpuTierPolicy ForBand(string? band) => ForTier(TierFromBand(band));

    public static GpuTierPolicy ForTier(GpuVramTier tier) => tier switch
    {
        GpuVramTier.From8To16Gb => new(tier, Canvas1080, Canvas4K, 16, 16, null),
        GpuVramTier.AtLeast16Gb => new(tier, Canvas4K, Canvas4K, 25, 25, null),
        GpuVramTier.Under8Gb => new(tier, Canvas1080, Canvas1080, 9, 9, null),
        _ => new(GpuVramTier.Unknown, Canvas1080, Canvas1080, 9, 9, UnknownVramNotice)
    };

    /// <summary>Pixel height of a "WxH" resolution string, or 0 when unparseable.</summary>
    public static int HeightOf(string? resolution)
    {
        var parts = (resolution ?? string.Empty).Split('x', 'X');
        return parts.Length == 2 && int.TryParse(parts[1], out var h) ? h : 0;
    }

    public bool CanvasExceedsCeiling(string? resolution) => HeightOf(resolution) > HeightOf(CanvasCeiling);

    /// <summary>
    /// Saved value wins, always. Null/blank saved value = the operator never
    /// chose, so the tier default applies.
    /// </summary>
    public string ResolveCanvas(string? saved, out bool exceedsCeiling)
    {
        if (string.IsNullOrWhiteSpace(saved))
        {
            exceedsCeiling = false;
            return DefaultCanvas;
        }

        exceedsCeiling = CanvasExceedsCeiling(saved);
        return saved;
    }

    /// <summary>
    /// <paramref name="saved"/> is null when the profile has never persisted a
    /// tile count. A saved value is kept (clamped only by the product's own
    /// hard range, done by the caller), and flagged when above the tier ceiling.
    /// </summary>
    public int ResolveTileCount(int? saved, out bool exceedsCeiling)
    {
        if (saved is null)
        {
            exceedsCeiling = false;
            return EffectiveDefaultTileCount;
        }

        exceedsCeiling = saved.Value > EffectiveTileCeiling;
        return saved.Value;
    }

    /// <summary>Canvas sizes offered in the picker: those within the ceiling, plus the current value so a saved choice never vanishes.</summary>
    public IReadOnlyList<string> OfferedCanvases(IEnumerable<string> all, string? current, bool operatorOverride) =>
        all.Where(r => operatorOverride || !CanvasExceedsCeiling(r)
                       || string.Equals(r, current, StringComparison.Ordinal)).ToList();

    /// <summary>Single log/support-bundle line: tier + override pair only.</summary>
    public string DescribeForLog(bool overrideActive) =>
        $"gpu-tier-defaults tier={TierLabel} override={(overrideActive ? "yes" : "no")}";
}
