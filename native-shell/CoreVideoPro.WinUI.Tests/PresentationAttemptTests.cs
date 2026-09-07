using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class PresentationAttemptTests
{
    [Fact]
    public void BusyPresentKeepsGenerationPendingUntilSuccessfulRetry()
    {
        var shown = 10L;
        const long pending = 11;
        Assert.False(PresentationAttempt.Commit(PresentationAttempt.StillDrawing, () => shown = pending));
        Assert.Equal(10, shown);
        Assert.True(PresentationAttempt.Commit(0, () => shown = pending));
        Assert.Equal(pending, shown);
    }

    [Fact]
    public void DeviceFailureDoesNotCommitAndReachesFallbackHandler()
    {
        var committed = false;
        Assert.ThrowsAny<Exception>(() => PresentationAttempt.Commit(unchecked((int)0x887A0005), () => committed = true));
        Assert.False(committed);
    }

    [Fact]
    public void OcclusionDoesNotClaimNewGenerationShown()
    {
        var committed = false;
        Assert.False(PresentationAttempt.Commit(0x087A0001, () => committed = true));
        Assert.False(committed);
    }

    [Fact]
    public void WatchdogReportsOutstandingStageOnceAndClearsOnCompletion()
    {
        long now = 1000;
        var reports = new List<string>();
        using var watchdog = new PresentationStageWatchdog((stage, elapsed) => reports.Add($"{stage}:{elapsed}"), () => now, false);
        watchdog.Mark("copy-back-buffer");
        now += 249;
        watchdog.Check();
        Assert.Empty(reports);
        now++;
        watchdog.Check();
        watchdog.Check();
        Assert.Equal(new[] { "copy-back-buffer:250" }, reports);
        watchdog.Mark(null);
        now += 5000;
        watchdog.Check();
        Assert.Single(reports);
        watchdog.Mark("present");
        now += 300;
        watchdog.Check();
        Assert.Equal("present:300", reports[1]);
        watchdog.Dispose();
        watchdog.Mark("after-dispose");
        now += 1000;
        watchdog.Check();
        Assert.Equal(2, reports.Count);
    }
}
