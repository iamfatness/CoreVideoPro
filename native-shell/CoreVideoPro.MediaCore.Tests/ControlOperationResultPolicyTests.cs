using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class ControlOperationResultPolicyTests
{
    [Fact]
    public void LostRepliesResolveFromHistoryButRetiredEpochResultsCannotApply()
    {
        var route = new NativeMediaCoreAudioRouteControl
        {
            AuthorityEpoch = "route-new", Revision = 4,
            LastResult = new NativeMediaCoreAudioRouteResult
            {
                AuthorityEpoch = "route-new", OperationId = "other", Status = "applied"
            },
            RecentResults =
            [
                new NativeMediaCoreAudioRouteResult
                {
                    AuthorityEpoch = "route-old", OperationId = "reused", Status = "applied"
                },
                new NativeMediaCoreAudioRouteResult
                {
                    AuthorityEpoch = "route-new", OperationId = "lost-route", Status = "applied",
                    Revision = 4
                }
            ]
        };
        Assert.Equal("applied", ControlOperationResultPolicy.AudioRoute(route, "lost-route")?.Status);
        Assert.Null(ControlOperationResultPolicy.AudioRoute(route, "reused"));

        var monitor = new NativeMediaCoreMonitorControl
        {
            AuthorityEpoch = "monitor-new", Revision = 2,
            RecentResults =
            [
                new NativeMediaCoreMonitorControlResult
                {
                    AuthorityEpoch = "monitor-new", OperationId = "lost-monitor", Status = "conflict"
                }
            ]
        };
        Assert.Equal("conflict", ControlOperationResultPolicy.Monitor(monitor, "lost-monitor")?.Status);
        Assert.Null(ControlOperationResultPolicy.Monitor(monitor, "unknown"));
    }
}
