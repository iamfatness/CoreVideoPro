using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.ViewModels.Transport;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// T1.5 (#432): the launch sync colliding with the bridge's poll used to leave a running core
/// reported as "Media core unavailable - media-core sync in flight; skipped for backpressure"
/// until Engine On.
/// </summary>
public sealed class MediaCoreLaunchStatusPolicyTests
{
    [Fact]
    public void ASkippedLaunchSyncReportsTheRunningCoreAsReadyAndQueuesTheSync()
    {
        var status = MediaCoreLaunchStatusPolicy.Resolve(new MediaCoreSyncInFlightException(), "GPU 1080p60");

        Assert.DoesNotContain("unavailable", status.EngineStatus, StringComparison.OrdinalIgnoreCase);
        Assert.Equal("Media core ready - GPU 1080p60", status.EngineStatus);
        Assert.False(status.Failed);
        Assert.True(status.QueueSyncRetry);
    }

    [Fact]
    public void ASingleBackpressureSkipNeverLeavesAnUnavailableStatusBehind()
    {
        // Launch skipped once for backpressure, then the retry worker's sync succeeds: at no
        // point is the operator told the core is unavailable.
        var afterSkip = MediaCoreLaunchStatusPolicy.Resolve(new MediaCoreSyncInFlightException(), "GPU 1080p60");
        var afterNextSuccess = MediaCoreLaunchStatusPolicy.Resolve(null, "GPU 1080p60");

        Assert.All(new[] { afterSkip, afterNextSuccess }, status =>
            Assert.False(status.EngineStatus.StartsWith("Media core unavailable", StringComparison.Ordinal)));
        Assert.False(afterNextSuccess.QueueSyncRetry);
    }

    [Fact]
    public void ARealLaunchFailureIsStillReportedLoudly()
    {
        var status = MediaCoreLaunchStatusPolicy.Resolve(
            new InvalidOperationException("Media core is not running."), "GPU 1080p60");

        Assert.Equal("Media core unavailable - Media core is not running.", status.EngineStatus);
        Assert.True(status.Failed);
        Assert.False(status.QueueSyncRetry);
    }
}
