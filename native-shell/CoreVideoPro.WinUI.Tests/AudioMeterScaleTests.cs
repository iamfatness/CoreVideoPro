using CoreVideoPro.WinUI.Models;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class AudioMeterScaleTests
{
    [Theory]
    [InlineData(-120, 0)]
    [InlineData(-60, 0)]
    [InlineData(-48, 20)]
    [InlineData(-36, 40)]
    [InlineData(-24, 60)]
    [InlineData(-12, 80)]
    [InlineData(-6, 90)]
    [InlineData(0, 100)]
    public void ToLevel_UsesCalibratedMinus60ToZeroDbfsScale(double dbfs, int expected) =>
        Assert.Equal(expected, AudioMeterScale.ToLevel(dbfs));

    [Fact]
    public void ToLevel_MutedAlwaysReportsSilence() =>
        Assert.Equal(0, AudioMeterScale.ToLevel(-3, muted: true));

    [Theory]
    [InlineData(324)]
    [InlineData(576)]
    [InlineData(900)]
    public void FitVerticalSegments_FillsAvailableWindowHeight(double availableHeight)
    {
        var layout = AudioMeterScale.FitVerticalSegments(availableHeight, 36);

        Assert.Equal(36, layout.SegmentCount);
        Assert.Equal(availableHeight, layout.OccupiedSize, precision: 6);
    }

    [Fact]
    public void FitVerticalSegments_GrowsSegmentsInTallWindow()
    {
        var compact = AudioMeterScale.FitVerticalSegments(324, 36);
        var tall = AudioMeterScale.FitVerticalSegments(576, 36);

        Assert.True(tall.SegmentSize > compact.SegmentSize);
        Assert.True(tall.SegmentSize > 7);
    }

    [Fact]
    public void FitVerticalSegments_ReducesResolutionRatherThanOverflowingShortWindow()
    {
        var layout = AudioMeterScale.FitVerticalSegments(72, 36);

        Assert.True(layout.SegmentCount < 36);
        Assert.True(layout.SegmentSize >= 2);
        Assert.Equal(72, layout.OccupiedSize, precision: 6);
    }
}
