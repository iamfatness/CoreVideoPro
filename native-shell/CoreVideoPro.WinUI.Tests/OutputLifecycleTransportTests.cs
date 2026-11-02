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
        var snapshot = Snapshot(("rtmp", "live"), ("srt", "failed"));
        Assert.False(TransportStatusFormatter.IsStreamingStartProven(snapshot, ["rtmp", "srt"]));
        Assert.False(TransportStatusFormatter.TryFormatStreamingStartHealthFailure(snapshot, out _));
        Assert.False(TransportStatusFormatter.TryFormatStreamingStartNoSenderFailure(snapshot, ["rtmp", "srt"], out _));
    }

    [Fact]
    public void EveryRequestedDestinationMustBeLive()
    {
        var snapshot = Snapshot(("rtmp", "live"), ("srt", "live"));
        Assert.True(TransportStatusFormatter.IsStreamingStartProven(snapshot, ["rtmp", "srt"]));
        Assert.False(TransportStatusFormatter.IsStreamingStartProven(snapshot, ["rtmp", "hls"]));
        Assert.True(TransportStatusFormatter.TryFormatStreamingStartHealthFailure(Snapshot(("rtmp", "failed")), out _));
    }

    // NDI is its own output: its senders never prove, fail or hold a stream start.
    [Fact]
    public void StreamStartProofIgnoresNdiSenders()
    {
        var snapshot = Snapshot(("rtmp", "live"), ("ndi", "failed"));
        Assert.True(TransportStatusFormatter.IsStreamingStartProven(snapshot, ["rtmp"]));
        Assert.True(TransportStatusFormatter.IsStreamingStartProven(snapshot, ["rtmp", "ndi", "ndi-graphics"]));
        Assert.False(TransportStatusFormatter.IsStreamingStartProven(Snapshot(("ndi", "live")), ["ndi"]));
        Assert.False(TransportStatusFormatter.TryFormatStreamingStartHealthFailure(snapshot, out _));

        // A live NDI sender must not mask a failed platform, and a failed NDI sender is not a stream failure.
        Assert.True(TransportStatusFormatter.TryFormatStreamingStartHealthFailure(
            Snapshot(("rtmp", "failed"), ("ndi", "live")), out var failure));
        Assert.Contains("Network unavailable", failure);
        Assert.False(TransportStatusFormatter.TryFormatStreamingStartHealthFailure(Snapshot(("ndi", "failed")), out _));
    }
}
