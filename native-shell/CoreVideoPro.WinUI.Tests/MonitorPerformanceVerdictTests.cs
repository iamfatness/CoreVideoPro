using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class MonitorPerformanceVerdictTests
{
    [Theory]
    [InlineData(33.4)]
    [InlineData(100)]
    [InlineData(249)]
    public void GoodAverageDoesNotHideStutter(double gap)
    {
        var verdict = MonitorPerformanceVerdict.Evaluate(true, [new("preview", 10, 600, gap)]);
        Assert.Equal("failed", verdict.Status);
        Assert.False(verdict.SubmissionCadencePassed);
        Assert.False(verdict.FramePerformancePassed);
    }
    [Fact]
    public void SmoothSubmissionsWithoutDisplayEvidenceCannotPassPerformance()
    {
        var verdict = MonitorPerformanceVerdict.Evaluate(true, [new("multiview", 10, 600, 16.666667)]);
        Assert.True(verdict.SubmissionCadencePassed);
        Assert.False(verdict.FramePerformancePassed);
        Assert.Equal("unverified-display-delivery", verdict.Status);
    }
    [Fact]
    public void MissingTimingAndLowCadenceFail()
    {
        Assert.Equal("failed", MonitorPerformanceVerdict.Evaluate(true, []).Status);
        Assert.Equal("failed", MonitorPerformanceVerdict.Evaluate(true, [new("source", 6, 150, 16)]).Status);
        Assert.Equal("failed", MonitorPerformanceVerdict.Evaluate(true, [new("source", 6, 360, double.NaN)]).Status);
    }
}
