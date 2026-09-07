using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class DiagnosticLogTests
{
    [Theory]
    [InlineData("CoreVideoPro.WinUI.Tests", true)]
    [InlineData("CoreVideoPro.MediaCore.Tests", true)]
    [InlineData("testhost", true)]
    [InlineData("xunit.runner.visualstudio", true)]
    [InlineData("CoreVideoPro.WinUI", false)]
    [InlineData(null, false)]
    public void IdentifiesTestHostWithoutChangingAppClassification(string? assembly, bool expected) =>
        Assert.Equal(expected, DiagnosticLog.IsTestAssembly(assembly));

    [Fact]
    public void AppPathsRemainCompatibleWithSupportBundleWhileTestSessionsAreIsolated()
    {
        var root = Path.Combine(Path.GetTempPath(), "corevideo-log-paths");
        foreach (var name in new[] { "launch.log", "media-core.log", "perf.log" })
        {
            Assert.Equal(Path.Combine(root, name), DiagnosticLog.ResolvePath(root, name, false, "app-session"));
            Assert.Equal(Path.Combine(root, "test-logs", "test-session", name),
                DiagnosticLog.ResolvePath(root, name, true, "test-session"));
        }
    }

    [Fact]
    public void RepeatErrorsRetainFirstAndReportSuppressedCountAfterWindow()
    {
        var limiter = new DiagnosticExceptionLimiter();
        Assert.True(limiter.ShouldWrite("preview-sync", 0, out var first));
        Assert.Equal(0, first);
        Assert.False(limiter.ShouldWrite("preview-sync", 1, out _));
        Assert.False(limiter.ShouldWrite("preview-sync", 9999, out _));
        Assert.True(limiter.ShouldWrite("preview-sync", 10000, out var suppressed));
        Assert.Equal(2, suppressed);
        Assert.True(limiter.ShouldWrite("different-error", 10001, out _));
    }

    [Fact]
    public void LoadedTestAssemblyRoutesThisProcessAwayFromAppLogs() => Assert.True(DiagnosticLog.IsTestHost);
}
