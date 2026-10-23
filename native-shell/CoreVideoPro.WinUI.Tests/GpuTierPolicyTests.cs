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
    public void StartupLog_ReportsSwitchAndAboveFactsSeparately()
    {
        var low = GpuTierPolicy.ForTier(GpuVramTier.Under8Gb);
        var r = low.Resolve(new ProductionOutputPreferences { CanvasResolution = "3840x2160", MultiviewTileCount = 10 });
        Assert.Equal(
            "gpu-tier-defaults tier=under-8gb switch=off canvasAbove=yes tilesAbove=yes canvasSource=saved tilesSource=saved",
            low.FormatStartupLog(r));
        var on = low.Resolve(new ProductionOutputPreferences { CanvasResolution = "1920x1080", GpuTierLimitsOverride = true });
        Assert.Contains("switch=on canvasAbove=no tilesAbove=yes", low.FormatStartupLog(on));
        Assert.Contains("canvasSource=tier-default tilesSource=tier-default", low.FormatStartupLog(low.Resolve(null)));
    }

    [Fact]
    public void CanvasChangeAboveTierDefault_IsLogged_OtherwiseNot()
    {
        var mid = GpuTierPolicy.ForTier(GpuVramTier.From8To16Gb);
        Assert.Equal("gpu-tier-defaults tier=8-16gb canvasOverride=3840x2160 default=1920x1080", mid.FormatCanvasChangeLog("3840x2160"));
        Assert.Null(mid.FormatCanvasChangeLog("1920x1080"));
        Assert.Null(mid.FormatCanvasChangeLog(null));
    }

    [Theory]
    [InlineData(GpuVramTier.Under8Gb, false, 10, 10)]   // saved 10 above ceiling 9: Maximum must not coerce it
    [InlineData(GpuVramTier.Unknown, false, 10, 10)]
    [InlineData(GpuVramTier.Under8Gb, false, 4, 9)]
    [InlineData(GpuVramTier.Under8Gb, true, 4, 10)]
    [InlineData(GpuVramTier.AtLeast16Gb, false, 4, 10)]
    public void TileMaximum_NeverFallsBelowCurrentValue(GpuVramTier tier, bool over, int current, int expected)
    {
        var max = GpuTierPolicy.ForTier(tier).TileMaximum(over, current);
        Assert.Equal(expected, max);
        Assert.True(max >= current);
    }

    [Fact]
    public void SavedTenTilesOnUnder8Gb_AreKeptAndSerializeUnchanged()
    {
        var saved = new ProductionOutputPreferences { CanvasResolution = "1920x1080", MultiviewTileCount = 10 };
        var low = GpuTierPolicy.ForTier(GpuVramTier.Under8Gb);
        var r = low.Resolve(saved);
        Assert.Equal(10, r.TileCount);
        Assert.Equal("1920x1080", r.Canvas);
        Assert.Equal(10, low.TileMaximum(r.SwitchOn, r.TileCount)); // NumberBox cannot coerce down
        Assert.Equal(saved.MultiviewTileCount, r.TileCount);
    }

    [Fact]
    public void SavedTilesWithBlankCanvas_AreStillKept()
    {
        var saved = new ProductionOutputPreferences { CanvasResolution = null, MultiviewTileCount = 6 };
        var r = GpuTierPolicy.ForTier(GpuVramTier.AtLeast16Gb).Resolve(saved);
        Assert.Equal(6, r.TileCount);
        Assert.False(r.TilesFromDefault);
        Assert.Equal("3840x2160", r.Canvas);
        Assert.True(r.CanvasFromDefault);
    }

    [Fact]
    public void OlderVersionProfile_OnSixteenGbTier_ChangesNothing()
    {
        var prefs = ProductionOutputPreferencesSerializer.Deserialize(
            "{\"Version\":11,\"CanvasResolution\":\"1920x1080\",\"MultiviewTileCount\":10}")!;
        var r = GpuTierPolicy.ForBand("nvidia-vram16gb+").Resolve(prefs);
        Assert.Equal("1920x1080", r.Canvas);
        Assert.Equal(10, r.TileCount);
        Assert.False(r.CanvasFromDefault);
        Assert.False(r.TilesFromDefault);
        Assert.False(r.CanvasAboveCeiling);
        Assert.False(r.TilesAboveCeiling);
    }

    [Fact]
    public void StatusLine_StatesCeilingsCapAndFourteenFortyRule()
    {
        var line = GpuTierPolicy.ForTier(GpuVramTier.AtLeast16Gb).StatusLine(false);
        Assert.Contains("canvas up to 4K", line);
        Assert.Contains("tiles up to 10 (product limit)", line);
        Assert.Contains("1440p counts as above the 1080p ceiling", line);
        Assert.Contains("not measured", GpuTierPolicy.ForTier(GpuVramTier.Unknown).StatusLine(false));
    }

    [Fact]
    public void Preferences_OverrideFlagRoundTripsAndDefaultsOff()
    {
        Assert.False(new ProductionOutputPreferences().GpuTierLimitsOverride);
        var json = ProductionOutputPreferencesSerializer.Serialize(new ProductionOutputPreferences { GpuTierLimitsOverride = true });
        Assert.True(ProductionOutputPreferencesSerializer.Deserialize(json)!.GpuTierLimitsOverride);
    }
}
