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
                        FramesSent = 10,
                        AudioFramesSent = 48000,
                        AudioBytesSent = 384000,
                        AudioChannels = 2,
                        AudioSampleRate = 48000
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
        Assert.Equal(48000, sender.AudioFramesSent);
        Assert.Equal(384000, sender.AudioBytesSent);
        Assert.Equal(2, sender.AudioChannels);
        Assert.Equal(48000, sender.AudioSampleRate);
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
        Assert.Equal(48000, bundle.MediaCore.Audio.Capture.CaptureFramesReceived);
        Assert.Equal(48000, bundle.MediaCore.Audio.Capture.RoutedMasterFrames);
        Assert.Equal(0, bundle.MediaCore.Audio.Capture.RoutedStreamFrames);
        Assert.Equal(0, bundle.MediaCore.Audio.Capture.RoutedMonitorFrames);
        Assert.Equal(0, bundle.MediaCore.Audio.Capture.FallbackMonitorFrames);
        Assert.Equal(0, bundle.MediaCore.Audio.MonitorFramesPlayed);
        Assert.Equal("playing", bundle.MediaCore.Audio.MonitorStatus);
        Assert.Equal("Speaker Out", bundle.MediaCore.Audio.MonitorDeviceName);
        Assert.Equal(2, bundle.MediaCore.Audio.Routing.RoutedSendCount);
        Assert.Equal(48000, Assert.Single(bundle.MediaCore.Audio.Routing.BusTaps).Frames);
        var captureSource = Assert.Single(bundle.MediaCore.Audio.Capture.Sources);
        Assert.Equal("local-machine-audio", captureSource.CaptureDeviceId);
        Assert.Equal("system-loopback", captureSource.AudioDeviceId);
        Assert.Equal("wasapi:{speaker-out}", captureSource.NativeAudioDeviceId);
        Assert.Equal("WASAPI", captureSource.AudioDriverName);
        Assert.Equal("loopback", captureSource.AudioSourceKind);
        Assert.True(captureSource.SignalPresent);
        Assert.Equal(47952, captureSource.CaptureFramesRendered);
        Assert.Equal(48, captureSource.CaptureQueuedFrames);
        Assert.Equal(2, captureSource.CaptureUnderrunCount);
        Assert.Equal(-12.5, captureSource.PeakDbfs);
        Assert.Equal(-24.2, captureSource.RmsDbfs);
        Assert.Equal("{speaker-out}", captureSource.EndpointId);
        Assert.Equal("Speaker Out", captureSource.EndpointName);
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
        Assert.Contains(bundle.TriageLines, line => line.Contains("Audio capture: 1/1 streaming; PCM 48000; master 48000; stream 0; MON 0; monitor played 0", StringComparison.Ordinal));
        Assert.Contains(bundle.TriageLines, line => line.Contains("Audio stream fault: source PCM is present but not reaching the stream bus.", StringComparison.Ordinal));
        Assert.Contains(bundle.TriageLines, line => line.Contains("Audio monitor fault: program/master PCM is present but the MON bus has no routed PCM.", StringComparison.Ordinal));
    }

    [Fact]
    public void Build_FlagsLiveSenderWithNoAcceptedProgramAudio()
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
                        SenderId = "rtmp",
                        Destination = "rtmps://cdn.example.com/live",
                        Status = "live",
                        FramesSent = 120,
                        AudioFramesSent = 0
                    }
                ]
            }
        };

        var bundle = SupportBundleBuilder.Build(snapshot, new MediaCoreHealth());

        Assert.Contains(
            bundle.TriageLines,
            line => line.Contains("Output audio fault: rtmps://cdn.example.com/live is live but has accepted no program-audio frames.", StringComparison.Ordinal));
    }

    [Fact]
    public void Build_MapsRecordingAudioProof()
    {
        var snapshot = BuildSampleSnapshot() with
        {
            Recording = BuildRecordingSession(new NativeMediaCoreRecordingProof
            {
                AudioPacketsObserved = 3,
                AudioPresent = true,
                AudioSampleCount = 1440,
                AudioChannels = 2,
                AudioSampleRate = 48000,
                MetadataValid = true,
                ContainerFormat = "mp4",
                AudioCodec = "aac"
            })
        };

        var bundle = SupportBundleBuilder.Build(snapshot, new MediaCoreHealth());

        Assert.NotNull(bundle.MediaCore?.Recording?.Proof);
        Assert.Equal(3, bundle.MediaCore!.Recording!.Proof!.AudioPacketsObserved);
        Assert.Equal(1440, bundle.MediaCore.Recording.Proof.AudioSampleCount);
        Assert.Equal(2, bundle.MediaCore.Recording.Proof.AudioChannels);
        Assert.Equal(48000, bundle.MediaCore.Recording.Proof.AudioSampleRate);
        Assert.Contains(
            bundle.TriageLines,
            line => line.Contains("Recording audio proof: packets 3; samples 1440; 2 ch @ 48000 Hz", StringComparison.Ordinal));
    }

    [Fact]
    public void Build_FlagsActiveRecordingWithNoAudioSamples()
    {
        var snapshot = BuildSampleSnapshot() with
        {
            Recording = BuildRecordingSession(new NativeMediaCoreRecordingProof
            {
                AudioPacketsObserved = 0,
                AudioPresent = false,
                AudioSampleCount = 0,
                AudioChannels = 0,
                AudioSampleRate = 0
            })
        };

        var bundle = SupportBundleBuilder.Build(snapshot, new MediaCoreHealth());

        Assert.Contains(
            bundle.TriageLines,
            line => line.Contains("Recording audio fault: program/master PCM is present but the active recording has no audio samples.", StringComparison.Ordinal));
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
            MonitorEnabled = true,
            MonitorStatus = "playing",
            MonitorDeviceName = "Speaker Out",
            Warnings = ["clipping on master bus"]
        },
        AudioRoutingMatrix = new NativeMediaCoreAudioRoutingMatrix
        {
            Status = "ready",
            RoutedSendCount = 2,
            RoutedSourceCount = 1,
            ProgramTapFrames = 48000,
            Summary = "2 sends routed",
            BusTaps =
            [
                new NativeMediaCoreAudioBusTap
                {
                    BusId = "master",
                    Channels = 2,
                    Frames = 48000,
                    PeakDbfs = -12.5,
                    RmsDbfs = -24.2
                }
            ]
        },
        CaptureAudioSources = new NativeMediaCoreCaptureAudioSources
        {
            Status = "ready",
            SourceCount = 1,
            PairedCount = 1,
            StreamingCount = 1,
            CaptureFramesReceived = 48000,
            RoutedMasterFrames = 48000,
            RoutedStreamFrames = 0,
            RoutedMonitorFrames = 0,
            FallbackMonitorFrames = 0,
            MonitorFramesPlayed = 0,
            Summary = "1 capture source paired",
            Sources =
            [
                new NativeMediaCoreCaptureAudioSource
                {
                    CaptureDeviceId = "local-machine-audio",
                    SourceId = "local-machine-audio",
                    AudioDeviceId = "system-loopback",
                    AudioDeviceName = "System loopback",
                    AudioSourceKind = "loopback",
                    NativeAudioDeviceId = "wasapi:{speaker-out}",
                    AudioDriverName = "WASAPI",
                    Paired = true,
                    CaptureStreaming = true,
                    CaptureFramesReceived = 48000,
                    CaptureFramesRendered = 47952,
                    CaptureQueuedFrames = 48,
                    CaptureUnderrunCount = 2,
                    CaptureSampleRate = 48000,
                    CaptureChannels = 2,
                    PeakDbfs = -12.5,
                    RmsDbfs = -24.2,
                    SignalPresent = true,
                    EndpointId = "{speaker-out}",
                    EndpointName = "Speaker Out"
                }
            ]
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

    private static NativeMediaCoreRecordingSession BuildRecordingSession(NativeMediaCoreRecordingProof proof) =>
        new()
        {
            SessionId = "recording",
            Active = true,
            Status = "recording",
            WriterStatus = "writing",
            StartedAtMs = 1000,
            ElapsedMs = 2000,
            TargetFolder = "Recordings",
            FilenamePrefix = "show",
            Format = "mp4",
            Quality = "high",
            EstimatedDiskRateMBps = 4.99,
            ProgramPath = "Recordings/show-program-0.mp4",
            Proof = proof,
            TotalFramesWritten = 120,
            TotalDroppedFrames = 0,
            TotalBytesWritten = 4096
        };
}
