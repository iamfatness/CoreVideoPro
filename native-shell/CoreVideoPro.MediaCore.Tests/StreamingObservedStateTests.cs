using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class StreamingObservedStateTests
{
    [Theory]
    [InlineData("warning", 0, false)]
    [InlineData("warning", 12, true)]
    [InlineData("starting", 0, false)]
    [InlineData("failed", 12, false)]
    [InlineData("live", 12, true)]
    public void SenderWarningRequiresObservedMedia(string status, int frames, bool expected)
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            OutputSenderSession = new NativeMediaCoreOutputSenderSession
            {
                Status = "warning", ActiveSenderCount = 1,
                Senders = [new NativeMediaCoreOutputSender { SenderId = "rtmp:program", Destination = "rtmp",
                    Status = status, FramesSent = frames, Warning = "Connection degraded" }]
            },
            OutputHealth = [new NativeMediaCoreOutputHealth { Destination = "rtmp", Status = "warning", Message = "Connection degraded" }]
        };
        Assert.Equal(expected, LiveProductionSync.IsStreamingLive(snapshot));
    }

    [Fact]
    public void AggregateWarningAloneCannotProveStreaming()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            OutputHealth = [new NativeMediaCoreOutputHealth { Destination = "rtmp", Status = "warning", Message = "Authentication failed" }]
        };
        Assert.False(LiveProductionSync.IsStreamingLive(snapshot));
    }

    [Fact]
    public void FailedSenderOverridesStaleLiveAggregate()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            OutputHealth = [new NativeMediaCoreOutputHealth { Destination = "rtmp", Status = "live", Message = "Old status" }],
            OutputSenderSession = new NativeMediaCoreOutputSenderSession { Status = "failed", Senders =
                [new NativeMediaCoreOutputSender { SenderId = "rtmp:program", Destination = "rtmp", Status = "failed", FramesSent = 20 }] }
        };
        Assert.False(LiveProductionSync.IsStreamingLive(snapshot));
    }
}
