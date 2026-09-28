using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class RtmpIngestCaptureFactsTests
{
    [Fact]
    public void ExistingCoreSnapshotUpdatesSignalAndClearsItAfterPublisherExit()
    {
        var live = JsonSerializer.Deserialize<NativeMediaCoreStateSnapshot>("""
            {"captureDevices":[{"id":"rtmp-ingest-01","connectionState":"connected",
              "signalPresent":true,"resolution":{"width":1280,"height":720},"frameRate":30}]}
            """, new JsonSerializerOptions { PropertyNameCaseInsensitive = true })!;
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

        var stopped = JsonSerializer.Deserialize<NativeMediaCoreStateSnapshot>("""
            {"captureDevices":[{"id":"rtmp-ingest-01","connectionState":"connecting",
              "signalPresent":false,"resolution":{"width":1920,"height":1080},"frameRate":60}]}
            """, new JsonSerializerOptions { PropertyNameCaseInsensitive = true })!;
        Assert.True(RtmpIngestCaptureFacts.Apply(device,
            Assert.Single(RtmpIngestCaptureFacts.Read(stopped.CaptureDevices))));
        Assert.False(device.SignalPresent);
        Assert.Equal(0, device.ObservedFrameWidth);
        Assert.Equal("No signal", device.ObservedSignalLabel);
    }
}
