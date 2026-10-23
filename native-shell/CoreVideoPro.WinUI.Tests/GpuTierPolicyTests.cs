using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class GpuTierPolicyTests
{
    [Theory]
    [InlineData("nvidia-vram<2gb", GpuVramTier.Under8Gb)]
    [InlineData("amd-vram2-4gb", GpuVramTier.Under8Gb)]
    [InlineData("intel-vram4-8gb", GpuVramTier.Under8Gb)]
    [InlineData("nvidia-vram8-12gb", GpuVramTier.From8To16Gb)]
    [InlineData("nvidia-vram12-16gb", GpuVramTier.From8To16Gb)]
    [InlineData("nvidia-vram16gb+", GpuVramTier.AtLeast16Gb)]
    [InlineData("other-vram-unknown", GpuVramTier.Unknown)]
    [InlineData(null, GpuVramTier.Unknown)]
    [InlineData("", GpuVramTier.Unknown)]
    [InlineData("garbage", GpuVramTier.Unknown)]
    public void TierFromBand_MapsEveryProbeBand(string? band, GpuVramTier expected) =>
        Assert.Equal(expected, GpuTierPolicy.TierFromBand(band));

    [Fact]
    public void PolicyTable_MatchesOwnerRuling()
    {
        var low = GpuTierPolicy.ForTier(GpuVramTier.Under8Gb);
        Assert.Equal("1920x1080", low.DefaultCanvas);
        Assert.Equal("1920x1080", low.CanvasCeiling);
        Assert.Equal(9, low.TileCeiling);

        var mid = GpuTierPolicy.ForTier(GpuVramTier.From8To16Gb);
        Assert.Equal("1920x1080", mid.DefaultCanvas);
        Assert.Equal("3840x2160", mid.CanvasCeiling);
        Assert.Equal(16, mid.TileCeiling);

        var high = GpuTierPolicy.ForTier(GpuVramTier.AtLeast16Gb);
        Assert.Equal("3840x2160", high.DefaultCanvas);
        Assert.Equal("3840x2160", high.CanvasCeiling);
        Assert.Equal(25, high.TileCeiling);
    }

    [Fact]
    public void UnknownVram_IsUnder8GbWithVisibleNotice()
    {
        var unknown = GpuTierPolicy.ForBand(null);
        var low = GpuTierPolicy.ForTier(GpuVramTier.Under8Gb);
        Assert.Equal(low.DefaultCanvas, unknown.DefaultCanvas);
        Assert.Equal(low.TileCeiling, unknown.TileCeiling);
        Assert.False(string.IsNullOrWhiteSpace(unknown.Notice));
        Assert.Null(low.Notice);
    }

    [Fact]
    public void EffectiveTileCeiling_NeverExceedsWhatTheProductCanRender()
    {
        Assert.Equal(9, GpuTierPolicy.ForTier(GpuVramTier.Under8Gb).EffectiveTileCeiling);
        Assert.Equal(GpuTierPolicy.ProductTileCapacity, GpuTierPolicy.ForTier(GpuVramTier.AtLeast16Gb).EffectiveTileCeiling);
        Assert.Equal(GpuTierPolicy.ProductTileCapacity, GpuTierPolicy.ForTier(GpuVramTier.From8To16Gb).EffectiveDefaultTileCount);
    }

    [Fact]
    public void NeverSaved_GetsTierDefaults()
    {
        var p = GpuTierPolicy.ForTier(GpuVramTier.AtLeast16Gb);
        Assert.Equal("3840x2160", p.ResolveCanvas(null, out var above));
        Assert.False(above);
        Assert.Equal(10, p.ResolveTileCount(null, out above));
        Assert.False(above);
    }

    [Fact]
    public void SavedValuesAboveCeiling_ArePreservedAndFlagged()
    {
        var low = GpuTierPolicy.ForTier(GpuVramTier.Under8Gb);
        Assert.Equal("3840x2160", low.ResolveCanvas("3840x2160", out var canvasAbove));
        Assert.True(canvasAbove);
        Assert.Equal(10, low.ResolveTileCount(10, out var tilesAbove));
        Assert.True(tilesAbove);
    }

    [Fact]
    public void SavedValuesWithinCeiling_AreNotFlagged()
    {
        var low = GpuTierPolicy.ForTier(GpuVramTier.Under8Gb);
        Assert.Equal("1280x720", low.ResolveCanvas("1280x720", out var canvasAbove));
        Assert.False(canvasAbove);
        Assert.Equal(4, low.ResolveTileCount(4, out var tilesAbove));
        Assert.False(tilesAbove);
    }

    [Fact]
    public void OfferedCanvases_HideAboveCeilingUnlessOverriddenOrCurrent()
    {
        var all = new[] { "1280x720", "1920x1080", "2560x1440", "3840x2160" };
        var low = GpuTierPolicy.ForTier(GpuVramTier.Under8Gb);
        Assert.Equal(new[] { "1280x720", "1920x1080" }, low.OfferedCanvases(all, "1920x1080", false));
        Assert.Contains("3840x2160", low.OfferedCanvases(all, "3840x2160", false));
        Assert.Equal(all, low.OfferedCanvases(all, "1920x1080", true));
        Assert.Equal(all, GpuTierPolicy.ForTier(GpuVramTier.From8To16Gb).OfferedCanvases(all, null, false));
    }

    [Fact]
    public void LogLine_CarriesOnlyTierAndOverride()
    {
        var line = GpuTierPolicy.ForTier(GpuVramTier.From8To16Gb).DescribeForLog(true);
        Assert.Equal("gpu-tier-defaults tier=8-16gb override=yes", line);
    }

    [Fact]
    public void Preferences_OverrideFlagRoundTripsAndDefaultsOff()
    {
        Assert.False(new ProductionOutputPreferences().GpuTierLimitsOverride);
        var json = ProductionOutputPreferencesSerializer.Serialize(new ProductionOutputPreferences { GpuTierLimitsOverride = true });
        Assert.True(ProductionOutputPreferencesSerializer.Deserialize(json)!.GpuTierLimitsOverride);
    }
}
