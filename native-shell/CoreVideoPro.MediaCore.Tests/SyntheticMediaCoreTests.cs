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
}
