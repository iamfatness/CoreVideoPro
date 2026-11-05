using System;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class BridgeCaptureStopPolicyTests
{
    [Theory]
    [InlineData(BridgeCaptureStopReason.DeviceRemoved)]
    [InlineData(BridgeCaptureStopReason.ReaderLost)]
    public void LossPublishesLostAndRetainsMappingPastCoreDeadline(BridgeCaptureStopReason reason)
    {
        var stop = BridgeCaptureStopPolicy.For(reason);
        Assert.Equal(CaptureBridgeSignal.Lost, stop.Signal);
        Assert.True(stop.ReleaseDelay.TotalMilliseconds > BridgeCaptureStopPolicy.CoreSilentFailMs);
    }

    [Theory]
    [InlineData(BridgeCaptureStopReason.NativeTakeover)]
    [InlineData(BridgeCaptureStopReason.OperatorDisconnect)]
    [InlineData(BridgeCaptureStopReason.Shutdown)]
    public void DeliberateStopReleasesImmediately(BridgeCaptureStopReason reason)
    {
        var stop = BridgeCaptureStopPolicy.For(reason);
        Assert.Equal(CaptureBridgeSignal.Stopped, stop.Signal);
        Assert.Equal(TimeSpan.Zero, stop.ReleaseDelay);
    }

    [Fact]
    public void DiscoveryStopsOnlyPreviouslyKnownMissingBridgeReaders()
    {
        Assert.Equal(new[] { "lost" }, BridgeCaptureStopPolicy.VanishedBridgedDeviceIds(
            ["kept", "lost", "native"], ["kept"], ["kept", "lost", "opening"]));
    }
}
