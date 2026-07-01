using System;
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
}
