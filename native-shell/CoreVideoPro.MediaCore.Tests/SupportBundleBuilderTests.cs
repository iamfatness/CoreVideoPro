using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class SupportBundleBuilderTests
{
    [Fact]
    public void RedactEndpoint_StripsCredentialsAndSecretQueryParams()
    {
        var redacted = SupportBundleBuilder.RedactEndpoint(
            "rtmps://user:hunter2@live.example.com/app?token=abc123&latency=120&key=zzz");

        Assert.DoesNotContain("hunter2", redacted, StringComparison.Ordinal);
        Assert.DoesNotContain("abc123", redacted, StringComparison.Ordinal);
        Assert.DoesNotContain("zzz", redacted, StringComparison.Ordinal);
        Assert.Contains("redacted", redacted, StringComparison.Ordinal);
        // Non-secret query params are preserved.
        Assert.Contains("latency=120", redacted, StringComparison.Ordinal);
    }

    [Fact]
    public void RedactEndpoint_HandlesNonUrlAndEmpty()
    {
        Assert.Equal(string.Empty, SupportBundleBuilder.RedactEndpoint(null));
        Assert.Equal(string.Empty, SupportBundleBuilder.RedactEndpoint(""));

        var redacted = SupportBundleBuilder.RedactEndpoint("host=cdn passphrase=topsecret");
        Assert.DoesNotContain("topsecret", redacted, StringComparison.Ordinal);
        Assert.Contains("passphrase=redacted", redacted, StringComparison.Ordinal);
    }

    [Fact]
    public void RedactSecret_NeverEmitsRawValue()
    {
        Assert.Equal("absent", SupportBundleBuilder.RedactSecret(null));
        Assert.Equal("absent", SupportBundleBuilder.RedactSecret(""));
        Assert.Equal("present-redacted", SupportBundleBuilder.RedactSecret("sk_live_supersecret"));
    }

    [Fact]
    public void Build_RedactsOutputsAndDoesNotLeakSecrets()
    {
        var snapshot = BuildSampleSnapshot();
        var health = new MediaCoreHealth { RestartCount = 0, Recovering = false, Stopped = false };
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
                EncoderMode = "nvenc",
                TargetBitrateMbps = 8.2,
                Endpoint = "rtmps://live.example.com/app?token=SECRET_TOKEN",
                StreamKey = "sk_live_DO_NOT_LEAK"
            }
        };

        var bundle = SupportBundleBuilder.Build(snapshot, health, outputs: outputs);
        var json = SupportBundleBuilder.Serialize(bundle);

        Assert.DoesNotContain("SECRET_TOKEN", json, StringComparison.Ordinal);
        Assert.DoesNotContain("sk_live_DO_NOT_LEAK", json, StringComparison.Ordinal);
        Assert.DoesNotContain("rtmp-key-leak", json, StringComparison.Ordinal);

        var destination = Assert.Single(bundle.Outputs);
        Assert.Equal("present-redacted", destination.StreamKey);
        Assert.DoesNotContain("SECRET_TOKEN", destination.Endpoint, StringComparison.Ordinal);
        Assert.Equal("1920x1080", destination.Resolution);
        Assert.Equal("60", destination.Fps);
        Assert.Equal("h264", destination.Codec);
        Assert.Equal("nvenc", destination.EncoderMode);
        Assert.Equal(8.2, destination.TargetBitrateMbps);
    }

    [Fact]
    public void Build_RedactsSenderDestinationStrings()
    {
        var snapshot = BuildSampleSnapshot() with
        {
            OutputSenderSession = new NativeMediaCoreOutputSenderSession
            {
                Status = "live",
                ActiveSenderCount = 1,
                Senders =
                [
                    new NativeMediaCoreOutputSender
                    {
                        SenderId = "s1",
                        Destination = "rtmps://cdn.example.com/live?key=PRIVATE_KEY",
                        Status = "live",
                        FramesSent = 10
                    }
                ]
            }
        };

        var bundle = SupportBundleBuilder.Build(snapshot, new MediaCoreHealth());
        var json = SupportBundleBuilder.Serialize(bundle);

        Assert.DoesNotContain("PRIVATE_KEY", json, StringComparison.Ordinal);
        Assert.NotNull(bundle.MediaCore);
        var sender = Assert.Single(bundle.MediaCore!.Senders.Destinations);
        Assert.Contains("key=redacted", sender.Destination, StringComparison.Ordinal);
    }

    [Fact]
    public void Build_AssemblesMediaCoreSummaryFromSnapshot()
    {
        var snapshot = BuildSampleSnapshot();
        var health = new MediaCoreHealth
        {
            RestartCount = 2,
            Recovering = true,
            Stopped = false,
            CrashEvents =
            [
                new MediaCoreCrashEvent { At = "2026-06-20T10:00:00.000Z", ExitCode = 1, RestartCount = 1 },
                new MediaCoreCrashEvent { At = "2026-06-20T10:05:00.000Z", ExitCode = 9, RestartCount = 2 }
            ]
        };

        var bundle = SupportBundleBuilder.Build(snapshot, health);

        Assert.NotNull(bundle.MediaCore);
        Assert.Equal("scene-main", bundle.MediaCore!.SceneId);
        Assert.Equal("plan-1", bundle.MediaCore.RenderPlanId);
        Assert.Equal(42, bundle.MediaCore.Source.DeliveredFrameCount);
        Assert.Equal("native-frame", bundle.MediaCore.Source.Backing);
        Assert.Contains("compositor degraded", bundle.MediaCore.Warnings);

        Assert.NotNull(bundle.Runtime);
        Assert.Equal(2, bundle.Runtime!.RestartCount);
        Assert.True(bundle.Runtime.Recovering);
        Assert.Equal("recovering", bundle.Runtime.Status);

        Assert.NotNull(bundle.CrashRecovery);
        Assert.Equal(2, bundle.CrashRecovery!.Events.Count);
        Assert.Equal(9, bundle.CrashRecovery.Events[^1].ExitCode);

        Assert.Contains(bundle.TriageLines, line => line.Contains("Media core restarts: 2", StringComparison.Ordinal));
        Assert.Contains(bundle.TriageLines, line => line.Contains("Latest crash: exit 9", StringComparison.Ordinal));
    }

    [Fact]
    public void Build_WithoutSnapshot_StillProducesRuntimeAndTriage()
    {
        var bundle = SupportBundleBuilder.Build(
            snapshot: null,
            health: new MediaCoreHealth { Stopped = true });

        Assert.Null(bundle.MediaCore);
        Assert.NotNull(bundle.Runtime);
        Assert.Equal("stopped", bundle.Runtime!.Status);
        Assert.Contains(bundle.TriageLines, line => line.Contains("unavailable", StringComparison.Ordinal));
    }

    [Fact]
    public async Task WriteAsync_WritesRedactedJsonToDisk()
    {
        var path = Path.Combine(Path.GetTempPath(), $"cv-support-{Guid.NewGuid():N}.json");
        try
        {
            var outputs = new[]
            {
                new SupportBundleOutputDestination
                {
                    Id = "rtmp",
                    Protocol = "rtmps",
                    Endpoint = "rtmps://live.example.com/app",
                    StreamKey = "leak_me_if_you_can"
                }
            };

            var bundle = await SupportBundleBuilder.WriteAsync(
                path,
                BuildSampleSnapshot(),
                new MediaCoreHealth(),
                outputs: outputs);

            Assert.True(File.Exists(path));
            var json = await File.ReadAllTextAsync(path);
            Assert.DoesNotContain("leak_me_if_you_can", json, StringComparison.Ordinal);

            // Round-trips as valid JSON.
            using var doc = JsonDocument.Parse(json);
            Assert.Equal(bundle.Id, doc.RootElement.GetProperty("id").GetString());
        }
        finally
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
    }

    private static NativeMediaCoreStateSnapshot BuildSampleSnapshot() => new()
    {
        SceneId = "scene-main",
        FrameCount = 42,
        RenderPlan = new NativeMediaCoreRenderPlan { RenderPlanId = "plan-1" },
        SourceSnapshot = new NativeMediaCoreFrameSourceSnapshot
        {
            AdapterId = "zoom-adapter-1",
            Kind = "zoom-sdk",
            Status = "subscribed",
            SubscribedSourceCount = 4,
            LiveFrameCount = 40,
            StaleFrameCount = 2,
            DroppedFrameCount = 1,
            Warnings = ["feed p3 stale"]
        },
        Compositor = new NativeMediaCoreCompositorState
        {
            Status = "degraded",
            ProgramFrameCount = 100,
            DroppedFrameCount = 3,
            DegradedFrameCount = 5
        },
        AudioMixSession = new NativeMediaCoreAudioMixSession
        {
            Status = "warning",
            Summary = "Limiter active",
            LimiterActive = true,
            Warnings = ["clipping on master bus"]
        },
        OperatorActions =
        [
            new NativeMediaCoreOperatorAction
            {
                ActionId = "a1",
                Severity = "warning",
                Area = "source",
                Title = "Resubscribe feed",
                Detail = "p3 went stale"
            }
        ],
        EventLog =
        [
            new NativeMediaCoreEvent
            {
                EventId = "e1",
                AtMs = 1000,
                Severity = "warning",
                Area = "program",
                Title = "Compositor degraded",
                Detail = "dropped frames"
            }
        ],
        Warnings = ["compositor degraded"]
    };
}
