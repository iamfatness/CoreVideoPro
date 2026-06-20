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
        var snapshot = SyntheticMediaCore.SynthesizeSnapshot(
            [
                new NativeMediaCoreCommand
                {
                    Type = "sync-participant-audio-mix",
                    ExtensionData = new Dictionary<string, JsonElement>
                    {
                        ["limiterEnabled"] = limiterEnabled
                    }
                }
            ],
            elapsedMs: 1000,
            frameNumber: 1);

        Assert.False(snapshot.AudioMixSession.LimiterEnabled);
        Assert.False(snapshot.Diagnostics.AudioMixSession.LimiterEnabled);
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
