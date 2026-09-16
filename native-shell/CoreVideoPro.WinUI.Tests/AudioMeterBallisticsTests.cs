using CoreVideoPro.WinUI.Models;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class AudioMeterBallisticsTests
{
    [Fact]
    public void AttackIsImmediateAndReleaseIsTimeBased()
    {
        var meter = new AudioMeterBallistics();
        meter.Reset(0, false, false, 0);
        meter.SetInput(100, false, false, 0);
        Assert.Equal(100, meter.Level);
        meter.SetInput(0, false, false, 0);
        meter.Advance(300);
        Assert.InRange(meter.Level, 33, 37);
        Assert.Equal(100, meter.Peak);
        Assert.True(meter.NeedsAnimation);
        meter.Advance(2000);
        Assert.Equal(0, meter.Level);
        Assert.Equal(0, meter.Peak);
        Assert.False(meter.NeedsAnimation);
    }

    [Fact]
    public void PeakHoldKeepsTimerNeededAfterBarHasReachedSilence()
    {
        var meter = new AudioMeterBallistics();
        meter.Reset(1, false, false, 0);
        meter.SetInput(0, false, false, 0);
        meter.Advance(400);
        Assert.Equal(0, meter.Level);
        Assert.Equal(1, meter.Peak);
        Assert.True(meter.NeedsAnimation);
        meter.Advance(833);
        Assert.Equal(0, meter.Peak);
        Assert.False(meter.NeedsAnimation);
    }

    [Fact]
    public void MuteDropsOutputAndPeakImmediatelyButCanShowInput()
    {
        var meter = new AudioMeterBallistics();
        meter.Reset(90, false, false, 0);
        meter.SetInput(90, true, false, 10);
        Assert.Equal(0, meter.Level);
        Assert.Equal(0, meter.Peak);
        Assert.False(meter.NeedsAnimation);
        meter.SetInput(90, true, true, 20);
        Assert.Equal(90, meter.Level);
        meter.SetInput(0, true, true, 30);
        Assert.True(meter.NeedsAnimation);
        meter.Reset(0, true, true, 5000); // unloaded/reloaded page
        Assert.Equal(0, meter.Peak);
        Assert.False(meter.NeedsAnimation);
    }

    [Theory]
    [InlineData(double.NaN)]
    [InlineData(double.PositiveInfinity)]
    [InlineData(double.NegativeInfinity)]
    [InlineData(-100)]
    public void InvalidTelemetryIsSilentAndFinite(double value)
    {
        var meter = new AudioMeterBallistics();
        meter.Reset(value, false, false, 0);
        meter.SetInput(value, false, false, 33);
        Assert.Equal(0, meter.Level);
        Assert.Equal(0, meter.Peak);
        Assert.False(meter.NeedsAnimation);
    }

    [Fact]
    public void BurstUpdatesStayBoundedAndEventuallyIdle()
    {
        var meter = new AudioMeterBallistics();
        for (var i = 0; i < 100000; i++)
        {
            meter.SetInput((i * 17) % 150, i % 3 == 0, i % 7 == 0, i * 17L);
            Assert.InRange(meter.Level, 0, 100);
            Assert.InRange(meter.Peak, 0, 100);
        }
        meter.SetInput(0, false, false, 1700000);
        meter.Advance(1710000);
        Assert.False(meter.NeedsAnimation);
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void ResizedMeterAlwaysFitsEvenAtOnePixel(bool vertical)
    {
        foreach (var size in new[] { 0.5, 1, 2, 12, 35, 72, 129, 324, 900 })
        foreach (var requested in new[] { int.MinValue, 8, 18, 36, 48, int.MaxValue })
        {
            var fit = vertical ? AudioMeterScale.FitVerticalSegments(size, requested)
                : AudioMeterScale.FitHorizontalSegments(size, requested);
            Assert.InRange(fit.SegmentCount, 1, 48);
            Assert.InRange(fit.SegmentSize, 0, size);
            Assert.True(fit.OccupiedSize <= size + 0.000001);
        }
    }
}
