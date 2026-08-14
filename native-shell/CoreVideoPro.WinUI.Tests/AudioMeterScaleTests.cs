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
}
