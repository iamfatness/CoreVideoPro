using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.ViewModels.Transport;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class OutputLifecycleTransportTests
{
    private static NativeMediaCoreStateSnapshot Snapshot(params (string Destination, string Status)[] senders) => new()
    {
        OutputSenderSession = new NativeMediaCoreOutputSenderSession
        {
            Status = "live", ActiveSenderCount = senders.Length,
            Senders = senders.Select(s => new NativeMediaCoreOutputSender
            {
                SenderId = s.Destination, Destination = s.Destination, Status = s.Status,
                Warning = s.Status == "failed" ? "Network unavailable" : null
            }).ToList()
        }
    };

    [Fact]
    public void StartingIsAcceptedButNotProvenLive()
    {
        var snapshot = Snapshot(("rtmp", "starting"));
        Assert.False(TransportStatusFormatter.IsStreamingStartProven(snapshot, ["rtmp"]));
        Assert.False(TransportStatusFormatter.TryFormatStreamingStartNoSenderFailure(snapshot, ["rtmp"], out _));
    }

    [Fact]
    public void PartialFailureDoesNotStopHealthyDestinationOrProveAllLive()
    {
        var snapshot = Snapshot(("rtmp", "live"), ("ndi", "failed"));
        Assert.False(TransportStatusFormatter.IsStreamingStartProven(snapshot, ["rtmp", "ndi"]));
        Assert.False(TransportStatusFormatter.TryFormatStreamingStartHealthFailure(snapshot, out _));
        Assert.False(TransportStatusFormatter.TryFormatStreamingStartNoSenderFailure(snapshot, ["rtmp", "ndi"], out _));
    }

    [Fact]
    public void EveryRequestedDestinationMustBeLive()
    {
        var snapshot = Snapshot(("rtmp", "live"), ("ndi", "live"));
        Assert.True(TransportStatusFormatter.IsStreamingStartProven(snapshot, ["rtmp", "ndi"]));
        Assert.False(TransportStatusFormatter.IsStreamingStartProven(snapshot, ["rtmp", "srt"]));
        Assert.True(TransportStatusFormatter.TryFormatStreamingStartHealthFailure(Snapshot(("rtmp", "failed")), out _));
    }
}
