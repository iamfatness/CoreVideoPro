using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class SyntheticMediaCoreTests
{
    [Fact]
    public void SynthesizeSnapshotReflectsLimiterCommandState()
    {
        var limiterEnabled = JsonSerializer.SerializeToElement(false);
        var monitorEnabled = JsonSerializer.SerializeToElement(true);
        var monitorDeviceId = JsonSerializer.SerializeToElement("render-01");
        var monitorDeviceName = JsonSerializer.SerializeToElement("Studio Headphones");
        var monitorVolume = JsonSerializer.SerializeToElement(0.75);
        var snapshot = SyntheticMediaCore.SynthesizeSnapshot(
            [
                new NativeMediaCoreCommand
                {
                    Type = "sync-participant-audio-mix",
                    ExtensionData = new Dictionary<string, JsonElement>
                    {
                        ["limiterEnabled"] = limiterEnabled
                    }
                },
                new NativeMediaCoreCommand
                {
                    Type = "sync-audio-monitor",
                    ExtensionData = new Dictionary<string, JsonElement>
                    {
                        ["enabled"] = monitorEnabled,
                        ["deviceId"] = monitorDeviceId,
                        ["deviceName"] = monitorDeviceName,
                        ["volume"] = monitorVolume
                    }
                }
            ],
            elapsedMs: 1000,
            frameNumber: 1);

        Assert.False(snapshot.AudioMixSession.LimiterEnabled);
        Assert.False(snapshot.Diagnostics.AudioMixSession.LimiterEnabled);
        Assert.True(snapshot.AudioMixSession.MonitorEnabled);
        Assert.Equal("armed", snapshot.AudioMixSession.MonitorStatus);
        Assert.Equal("render-01", snapshot.AudioMixSession.MonitorDeviceId);
        Assert.Equal("Studio Headphones", snapshot.AudioMixSession.MonitorDeviceName);
        Assert.Equal(0.75, snapshot.AudioMixSession.MonitorVolume);
    }

    [Fact]
    public void SynthesizeSnapshotReflectsRequestedRecordingVideoCodec()
    {
        var renderProfile = JsonSerializer.SerializeToElement(new
        {
            profileId = "recording-av1",
            resolution = "1920x1080",
            width = 1920,
            height = 1080,
            fps = 60,
            targetBitrateMbps = 8,
            codec = "av1"
        });

        var snapshot = SyntheticMediaCore.SynthesizeSnapshot(
            [
                new NativeMediaCoreCommand
                {
                    Type = "start-recording-session",
                    ExtensionData = new Dictionary<string, JsonElement>
                    {
                        ["renderProfile"] = renderProfile
                    }
                }
            ],
            elapsedMs: 1000,
            frameNumber: 1);

        Assert.NotNull(snapshot.Recording);
        Assert.Equal("av1", snapshot.Recording!.Encoder.Codec);
        Assert.Equal("av1", snapshot.Diagnostics.Recording!.Encoder.Codec);
    }
}
