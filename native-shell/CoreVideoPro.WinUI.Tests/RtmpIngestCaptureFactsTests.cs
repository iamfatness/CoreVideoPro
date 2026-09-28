using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class RtmpIngestCaptureFactsTests
{
    [Fact]
    public void ExistingCoreSnapshotUpdatesSignalAndClearsItAfterPublisherExit()
    {
        using var liveResponse = JsonDocument.Parse("""
            {"ok":true,"snapshot":{"captureDevices":[{"id":"rtmp-ingest-01","connectionState":"connected",
              "signalPresent":true,"resolution":{"width":1280,"height":720},"frameRate":30}]}}
            """);
        var live = CoreProtocolParser.TryParseSyncSnapshot(liveResponse)!;
        var device = new CaptureDevice
        {
            Id = "rtmp-ingest-01", NativeDeviceId = "rtmp-ingest-01", Vendor = "rtmp",
            Name = "RTMP 1", Inputs = [], SelectedInputId = "rtmp-source-01"
        };
        var fact = Assert.Single(RtmpIngestCaptureFacts.Read(live.CaptureDevices));
        Assert.True(RtmpIngestCaptureFacts.Apply(device, fact));
        Assert.True(device.SignalPresent);
        Assert.Equal(1280, device.ObservedFrameWidth);
        Assert.Equal(30, device.ObservedFrameRate);
        Assert.False(RtmpIngestCaptureFacts.Apply(device, fact));

        using var stoppedResponse = JsonDocument.Parse("""
            {"ok":true,"snapshot":{"captureDevices":[{"id":"rtmp-ingest-01","connectionState":"connecting",
              "signalPresent":false,"resolution":{"width":1920,"height":1080},"frameRate":60}]}}
            """);
        var stopped = CoreProtocolParser.TryParseSyncSnapshot(stoppedResponse)!;
        Assert.True(RtmpIngestCaptureFacts.Apply(device,
            Assert.Single(RtmpIngestCaptureFacts.Read(stopped.CaptureDevices))));
        Assert.False(device.SignalPresent);
        Assert.Equal(0, device.ObservedFrameWidth);
        Assert.Equal("No signal", device.ObservedSignalLabel);
    }
}
