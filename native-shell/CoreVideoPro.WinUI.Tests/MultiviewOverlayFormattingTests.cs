using System;
using System.Collections.Generic;
using System.Linq;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Controls;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class MultiviewOverlayFormattingTests
{
    [Fact]
    public void ResolveLabel_ProgramRole_ReturnsProgramCaption()
    {
        var tile = new MultiviewTile { Role = "pgm", Label = "Alice" };
        Assert.Equal("PROGRAM", MultiviewOverlayFormatting.ResolveLabel(tile));
    }

    [Fact]
    public void ResolveLabel_PreviewRole_ReturnsPreviewCaption()
    {
        var tile = new MultiviewTile { Role = "pvw", Label = "Bob" };
        Assert.Equal("PREVIEW", MultiviewOverlayFormatting.ResolveLabel(tile));
    }

    [Fact]
    public void ResolveLabel_SourceRole_UsesCoreLabel()
    {
        var tile = new MultiviewTile { Role = "source", Label = "Camera 2" };
        Assert.Equal("Camera 2", MultiviewOverlayFormatting.ResolveLabel(tile));
    }

    [Fact]
    public void ResolveTally_ProgramRole_OverridesPerSourceTally()
    {
        var tile = new MultiviewTile { Role = "pgm", Tally = "none" };
        Assert.Equal(MultiviewOverlayFormatting.TallyProgram, MultiviewOverlayFormatting.ResolveTally(tile));
    }

    [Fact]
    public void ResolveTally_PreviewRole_OverridesPerSourceTally()
    {
        var tile = new MultiviewTile { Role = "pvw", Tally = "pgm" };
        Assert.Equal(MultiviewOverlayFormatting.TallyPreview, MultiviewOverlayFormatting.ResolveTally(tile));
    }

    [Theory]
    [InlineData("pgm", "pgm")]
    [InlineData("program", "pgm")]
    [InlineData("pvw", "pvw")]
    [InlineData("preview", "pvw")]
    [InlineData("none", "none")]
    [InlineData("", "none")]
    [InlineData("garbage", "none")]
    public void ResolveTally_SourceRole_NormalizesPerSourceTally(string tally, string expected)
    {
        var tile = new MultiviewTile { Role = "source", Tally = tally };
        Assert.Equal(expected, MultiviewOverlayFormatting.ResolveTally(tile));
    }

    [Fact]
    public void ShouldShowMeter_ProgramTile_IsTrue()
    {
        Assert.True(MultiviewOverlayFormatting.ShouldShowMeter(new MultiviewTile { Role = "pgm" }));
        Assert.True(MultiviewOverlayFormatting.ShouldShowMeter(new MultiviewTile { Tally = "pgm" }));
    }

    [Fact]
    public void ShouldShowMeter_PlainSourceTile_IsFalse()
    {
        Assert.False(MultiviewOverlayFormatting.ShouldShowMeter(new MultiviewTile { Role = "source", Tally = "pvw" }));
    }

    [Fact]
    public void FormatClock_ProducesHhMmSs()
    {
        var time = new DateTime(2026, 6, 30, 14, 5, 9);
        Assert.Equal("14:05:09", MultiviewOverlayFormatting.FormatClock(time));
    }

    // Regression: the overlay capped at 10 tiles TOTAL, but the core's list leads with the PGM
    // and PVW cells, so sources 9 and 10 got no click target (not cueable) and no label/tally.
    [Fact]
    public void SelectOverlayTiles_FullWall_KeepsProgramPreviewAndAllTenSources()
    {
        var tiles = new List<MultiviewTile>
        {
            new() { Role = "pgm", Slot = -2 },
            new() { Role = "pvw", Slot = -1 }
        };
        for (var slot = 0; slot < 10; slot++)
        {
            tiles.Add(new MultiviewTile { Role = "source", Slot = slot });
        }

        var selected = MultiviewOverlayFormatting.SelectOverlayTiles(tiles);

        Assert.Equal(12, selected.Count);
        Assert.Contains(selected, tile => tile.Role == "source" && tile.Slot == 8);
        Assert.Contains(selected, tile => tile.Role == "source" && tile.Slot == 9);
    }

    [Fact]
    public void SelectOverlayTiles_MoreSourcesThanShowInputs_CapsSourcesOnly()
    {
        var tiles = new List<MultiviewTile> { new() { Role = "pgm", Slot = -2 } };
        for (var slot = 0; slot < 14; slot++)
        {
            tiles.Add(new MultiviewTile { Role = "source", Slot = slot });
        }

        var selected = MultiviewOverlayFormatting.SelectOverlayTiles(tiles);

        Assert.Single(selected, tile => tile.Role == "pgm");
        Assert.Equal(10, selected.Count(tile => tile.Role == "source"));
    }
}
