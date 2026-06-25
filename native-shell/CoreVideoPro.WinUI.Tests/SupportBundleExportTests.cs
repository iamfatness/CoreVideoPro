using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Validates the WinUI shell's support-bundle export contract: the bundle the shell
/// assembles from its configured stream outputs never leaks stream keys, passphrases,
/// or endpoint credentials. Mirrors the redaction guarantee from src/engine/supportBundle.ts.
/// </summary>
public sealed class SupportBundleExportTests
{
    [Fact]
    public void ShellOutputs_AreRedactedInExportedBundle()
    {
        // Shell-shaped destinations as StudioViewModel.BuildSupportBundleOutputDestinations() produces them.
        var outputs = new[]
        {
            new SupportBundleOutputDestination
            {
                Id = "rtmp",
                Name = "RTMP",
                Protocol = "rtmps",
                Enabled = true,
                Active = true,
                Resolution = "1920x1080",
                Fps = "60",
                Codec = "h264",
                EncoderMode = "auto",
                TargetBitrateMbps = 8.2,
                Endpoint = "rtmps://a.rtmp.youtube.com/live2",
                StreamKey = "abcd-1234-SECRET-stream-key"
            },
            new SupportBundleOutputDestination
            {
                Id = "srt",
                Name = "SRT",
                Protocol = "srt",
                Enabled = true,
                Active = false,
                Resolution = "1920x1080",
                Fps = "60",
                Codec = "h264",
                EncoderMode = "auto",
                TargetBitrateMbps = 8.2,
                Endpoint = "srt://cdn.example.com:9000",
                StreamKey = "srt-passphrase-SECRET"
            },
            new SupportBundleOutputDestination
            {
                Id = "recording",
                Name = "Recording",
                Protocol = "file",
                Enabled = true,
                Active = false,
                Resolution = "1920x1080",
                Fps = "60",
                Codec = "h265",
                TargetBitrateMbps = 18,
                Format = "mp4",
                Quality = "high",
                Endpoint = "D:\\Shows",
                StreamKey = null
            }
        };

        var bundle = SupportBundleBuilder.Build(
            snapshot: null,
            health: new MediaCoreHealth { Stopped = true },
            app: new SupportBundleAppInfo { Platform = "winui-desktop" },
            outputs: outputs);
        var json = SupportBundleBuilder.Serialize(bundle);

        Assert.DoesNotContain("abcd-1234-SECRET-stream-key", json, StringComparison.Ordinal);
        Assert.DoesNotContain("srt-passphrase-SECRET", json, StringComparison.Ordinal);

        Assert.Equal(3, bundle.Outputs.Count);
        Assert.Equal("present-redacted", bundle.Outputs[0].StreamKey);
        Assert.Equal("present-redacted", bundle.Outputs[1].StreamKey);
        Assert.Equal("absent", bundle.Outputs[2].StreamKey);
        Assert.All(bundle.Outputs, destination => Assert.Equal("1920x1080", destination.Resolution));
        Assert.All(bundle.Outputs, destination => Assert.NotNull(destination.TargetBitrateMbps));
        Assert.Equal("mp4", bundle.Outputs[2].Format);
        Assert.Equal("high", bundle.Outputs[2].Quality);
        Assert.Equal("winui-desktop", bundle.App.Platform);
    }

    [Fact]
    public void RecoveryState_IsSurfacedFromSupervisorHealth()
    {
        var health = new MediaCoreHealth
        {
            RestartCount = 3,
            Recovering = true,
            Stopped = false,
            CrashEvents =
            [
                new MediaCoreCrashEvent { At = "2026-06-20T12:00:00.000Z", ExitCode = 134, RestartCount = 3 }
            ]
        };

        var bundle = SupportBundleBuilder.Build(snapshot: null, health: health);

        Assert.NotNull(bundle.Runtime);
        Assert.Equal("recovering", bundle.Runtime!.Status);
        Assert.Equal(3, bundle.Runtime.RestartCount);
        Assert.NotNull(bundle.CrashRecovery);
        Assert.Equal(134, bundle.CrashRecovery!.Events[^1].ExitCode);
    }
}
