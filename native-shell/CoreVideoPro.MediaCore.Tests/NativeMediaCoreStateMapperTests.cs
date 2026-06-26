using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class NativeMediaCoreStateMapperTests
{
    private static readonly IReadOnlyList<NativeMediaCoreCommand> Commands =
    [
        new()
        {
            Type = "load-scene-graph",
            ExtensionData = new Dictionary<string, JsonElement>
            {
                ["sceneId"] = JsonSerializer.SerializeToElement("interview"),
                ["routes"] = JsonSerializer.SerializeToElement(new[]
                {
                    new { routeId = "a", mode = "active-speaker", audioRole = "mix" }
                })
            }
        },
        new()
        {
            Type = "start-program-output",
            ExtensionData = new Dictionary<string, JsonElement>
            {
                ["destinations"] = JsonSerializer.SerializeToElement(new[] { "recording", "rtmp" }),
                ["isoParticipantIds"] = JsonSerializer.SerializeToElement(new[] { "p1" })
            }
        },
        new()
        {
            Type = "start-recording-session",
            ExtensionData = new Dictionary<string, JsonElement>
            {
                ["sessionId"] = JsonSerializer.SerializeToElement("show-1"),
                ["targetFolder"] = JsonSerializer.SerializeToElement("Recordings"),
                ["filenamePrefix"] = JsonSerializer.SerializeToElement("program"),
                ["format"] = JsonSerializer.SerializeToElement("mp4"),
                ["quality"] = JsonSerializer.SerializeToElement("high"),
                ["isoParticipantIds"] = JsonSerializer.SerializeToElement(new[] { "p1" })
            }
        }
    ];

    [Fact]
    public void MergesNativeEncoderRecordingAndSenderStateIntoSnapshot()
    {
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(Commands, 3000, 12, new NativeMediaCoreWireState
        {
            SceneId = "interview",
            RouteCount = 1,
            TransformCount = 0,
            OverlayCount = 0,
            Outputs = ["recording", "rtmp"],
            IsoParticipantIds = ["p1"],
            ProgramFrameCount = 12,
            RenderPlanId = "interview:1:0",
            CompositorRenderer = "d3d11",
            EncoderSession = new NativeMediaCoreEncoderSession
            {
                Status = "encoding",
                RenderPlanId = "interview:1:0",
                ProgramFrameCount = 12,
                Targets =
                [
                    new NativeMediaCoreEncoderTarget
                    {
                        TargetId = "recording:program",
                        Destination = "recording",
                        StreamKind = "program",
                        Status = "attached",
                        AttachedFrameCount = 12
                    }
                ],
                Lifecycle = new NativeMediaCoreEncoderLifecycle
                {
                    Status = "encoding",
                    LastTransition = "Program output encoder session started."
                }
            },
            OutputSenderSession = new NativeMediaCoreOutputSenderSession
            {
                Status = "live",
                ActiveSenderCount = 1,
                Senders =
                [
                    new NativeMediaCoreOutputSender
                    {
                        SenderId = "rtmp:program",
                        Destination = "rtmp",
                        Status = "live",
                        FramesSent = 12,
                        RetryCount = 0,
                        LatencyMs = 2100,
                        BitrateMbps = 6
                    }
                ]
            },
            Recording = new NativeMediaCoreRecordingSession
            {
                SessionId = "show-1",
                Active = true,
                Status = "recording",
                WriterStatus = "writing",
                StartedAtMs = 1000,
                ElapsedMs = 2000,
                TargetFolder = "Recordings",
                FilenamePrefix = "program",
                Format = "mp4",
                Quality = "high",
                Encoder = new NativeMediaCoreRecordingEncoder
                {
                    Codec = "h264",
                    HardwareAccelerated = true,
                    TargetBitrateMbps = 18
                },
                EstimatedDiskRateMBps = 4.99,
                ProgramPath = "Recordings/program-program-0.mp4",
                Streams = [],
                Proof = new NativeMediaCoreRecordingProof
                {
                    AudioPacketsObserved = 2,
                    AudioPresent = true,
                    AudioSampleCount = 960,
                    AudioChannels = 2,
                    AudioSampleRate = 48000
                },
                TotalFramesWritten = 12,
                TotalDroppedFrames = 0,
                TotalBytesWritten = 4096
            },
            Health = new NativeMediaCoreWireHealth
            {
                Status = "live",
                Renderer = "d3d11",
                Encoder = "media-foundation",
                Codec = "h264",
                HardwareEncoder = true,
                RecordingArtifactPath = "C:/Temp/corevideo-mf-recording-123.mp4",
                RecordingBytesWritten = 4096,
                EncodedFrameCount = 12,
                FrameCount = 12
            },
            Profile = new NativeMediaCoreProfile
            {
                Name = "CoreVideo Pro Native Media Core",
                Renderer = "d3d11",
                MaxProgramResolution = "1920x1080",
                MaxProgramFps = 30,
                MaxParticipantFeeds = 8,
                MaxIsoRecordings = 8,
                Capabilities = ["gpu-compositor", "program-recording", "rtmp-output"]
            }
        });

        Assert.Equal("interview", snapshot.SceneId);
        Assert.Equal(["recording", "rtmp"], snapshot.Outputs);
        Assert.Equal(12, snapshot.ProgramFrameCount);
        Assert.Equal("live", snapshot.Compositor.Status);
        Assert.Equal("encoding", snapshot.EncoderSession.Status);
        Assert.Equal("live", snapshot.OutputSenderSession.Status);
        Assert.Equal("recording", snapshot.Recording?.Status);
        Assert.Equal(960, snapshot.Recording?.Proof?.AudioSampleCount);
        Assert.Equal(48000, snapshot.Recording?.Proof?.AudioSampleRate);
        Assert.Contains("corevideo-mf-recording-123.mp4", snapshot.Warnings[0]);
        Assert.Contains(
            snapshot.OutputHealth,
            item => item.Destination == "rtmp" && item.Status == "live");
    }

    [Fact]
    public void MapsOutputSenderLastErrorIntoOutputHealthWhenWarningIsMissing()
    {
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(Commands, 3000, 12, new NativeMediaCoreWireState
        {
            Outputs = ["rtmp"],
            OutputSenderSession = new NativeMediaCoreOutputSenderSession
            {
                Status = "failed",
                ActiveSenderCount = 0,
                Senders =
                [
                    new NativeMediaCoreOutputSender
                    {
                        SenderId = "rtmp:program",
                        Destination = "rtmp",
                        Status = "failed",
                        FramesSent = 0,
                        RetryCount = 0,
                        LatencyMs = 0,
                        BitrateMbps = 6,
                        LastError = "Failed to start FFmpeg process. Win32 error 2."
                    }
                ]
            }
        });

        var rtmp = Assert.Single(snapshot.OutputHealth, item => item.Destination == "rtmp");
        Assert.Equal("failed", rtmp.Status);
        Assert.Equal("Failed to start FFmpeg process. Win32 error 2.", rtmp.Message);
    }

    [Fact]
    public void MergesNativeAudioMixAndCaptionTrackStateFromWirePayload()
    {
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(Commands, 3000, 12, new NativeMediaCoreWireState
        {
            SceneId = "interview",
            RouteCount = 1,
            AudioMixSession = new NativeMediaCoreAudioMixSession
            {
                Status = "live",
                MasterLevel = 72,
                LoudnessLufs = -16,
                LimiterActive = false,
                MixedFrameCount = 12,
                Participants =
                [
                    new NativeMediaCoreParticipantAudioChannel
                    {
                        ParticipantId = "p1",
                        InputLevel = 64,
                        OutputLevel = 68,
                        GainDb = 0,
                        NoiseSuppression = false,
                        LimiterActive = false,
                        Muted = false,
                        Status = "balanced"
                    }
                ],
                Summary = "Program mix balanced",
                Warnings = []
            },
            CaptureAudioSources = new NativeMediaCoreCaptureAudioSources
            {
                Status = "ready",
                SourceCount = 1,
                PairedCount = 1,
                StreamingCount = 1,
                CaptureFramesReceived = 960,
                RoutedMasterFrames = 480,
                RoutedStreamFrames = 480,
                RoutedMonitorFrames = 480,
                FallbackMonitorFrames = 120,
                MonitorFramesPlayed = 480,
                Summary = "1 of 1 capture source paired with audio input; 1 streaming, 960 PCM frames received; 480 master bus frames, 480 stream bus frames, 480 MON bus frames, 480 monitor playback frames.",
                Sources =
                [
                    new NativeMediaCoreCaptureAudioSource
                    {
                        CaptureDeviceId = "local-machine-audio",
                        SourceId = "local-machine-audio",
                        AudioDeviceId = "default-render",
                        AudioDeviceName = "System audio",
                        AudioSourceKind = "wasapi-loopback",
                        NativeAudioDeviceId = "{render-endpoint}",
                        AudioDriverName = "WASAPI",
                        AudioSyncOffsetMs = 20,
                        Paired = true,
                        CaptureStreaming = true,
                        CaptureFramesReceived = 960,
                        CaptureSampleRate = 48000,
                        CaptureChannels = 2,
                        Warning = string.Empty
                    }
                ],
                Warnings = []
            },
            AudioRoutingMatrix = new NativeMediaCoreAudioRoutingMatrix
            {
                Status = "live",
                RoutedSendCount = 2,
                RoutedSourceCount = 1,
                ProgramTapFrames = 480,
                Summary = "Audio routing matrix live.",
                BusTaps =
                [
                    new NativeMediaCoreAudioBusTap
                    {
                        BusId = "master",
                        Channels = 2,
                        Frames = 480,
                        PeakDbfs = -8.5,
                        RmsDbfs = -18.25
                    },
                    new NativeMediaCoreAudioBusTap
                    {
                        BusId = "mon",
                        Channels = 2,
                        Frames = 480,
                        PeakDbfs = -9.75,
                        RmsDbfs = -20.5
                    }
                ],
                Warnings = []
            },
            CaptionTrack = new NativeMediaCoreCaptionTrack
            {
                Enabled = true,
                Status = "live",
                CurrentCue = new NativeMediaCoreCaptionCue
                {
                    Text = "Welcome to the webinar.",
                    Speaker = "Sophia Martinez",
                    AtMs = 2800,
                    Confidence = 95
                },
                LatencyMs = 180,
                Warnings = []
            },
            OverlayState = new NativeMediaCoreOverlayState
            {
                Status = "live",
                OverlayCount = 1,
                LowerThirdCount = 1,
                OnAirCount = 1,
                BuildingCount = 0,
                HiddenCount = 0,
                Summary = "1 lower-third overlay, 1 on-air, 0 building.",
                Overlays =
                [
                    new NativeMediaCoreOverlayAssetState
                    {
                        OverlayId = "key:lower-third",
                        Kind = "lower-third",
                        Position = "lower-third",
                        SourceId = "p2",
                        SourceName = "David Chen",
                        Title = "Chief Product Officer",
                        Org = "Main room",
                        Text = "David Chen",
                        KeyPosition = "lower-left",
                        KeyPhase = "on-air",
                        KeyProgress = 1,
                        Keyer = "downstream",
                        BuildInMs = 350,
                        BuildOutMs = 275,
                        Visible = true
                    }
                ],
                Warnings = []
            },
            MediaPlayback = new NativeMediaCoreMediaPlaybackState
            {
                Status = "playing",
                MediaAssetId = "clip-intro",
                MediaAssetName = "Intro Sting",
                MediaAssetKind = "stinger",
                MediaAssetPath = @"C:\media\intro.mp4",
                MediaPlaybackKey = "program-take:3:media:clip-intro",
                Playing = true,
                Summary = "Playing Intro Sting with key program-take:3:media:clip-intro.",
                Warnings = []
            }
        });

        Assert.Equal("Program mix balanced", snapshot.AudioMixSession.Summary);
        Assert.Equal(72, snapshot.AudioMixSession.MasterLevel);
        Assert.Equal("ready", snapshot.CaptureAudioSources.Status);
        Assert.Equal(960, snapshot.CaptureAudioSources.CaptureFramesReceived);
        Assert.Equal(480, snapshot.CaptureAudioSources.RoutedMasterFrames);
        Assert.Equal(480, snapshot.CaptureAudioSources.RoutedStreamFrames);
        Assert.Equal(480, snapshot.CaptureAudioSources.RoutedMonitorFrames);
        Assert.Equal(120, snapshot.CaptureAudioSources.FallbackMonitorFrames);
        Assert.Equal(480, snapshot.CaptureAudioSources.MonitorFramesPlayed);
        Assert.Single(snapshot.CaptureAudioSources.Sources);
        Assert.Equal("wasapi-loopback", snapshot.CaptureAudioSources.Sources[0].AudioSourceKind);
        Assert.Equal("local-machine-audio", snapshot.CaptureAudioSources.Sources[0].SourceId);
        Assert.Equal("{render-endpoint}", snapshot.CaptureAudioSources.Sources[0].NativeAudioDeviceId);
        Assert.Equal("WASAPI", snapshot.CaptureAudioSources.Sources[0].AudioDriverName);
        Assert.Equal(20, snapshot.CaptureAudioSources.Sources[0].AudioSyncOffsetMs);
        Assert.Equal("live", snapshot.AudioRoutingMatrix.Status);
        Assert.Equal(2, snapshot.AudioRoutingMatrix.RoutedSendCount);
        Assert.Equal(480, snapshot.AudioRoutingMatrix.ProgramTapFrames);
        Assert.Equal(2, snapshot.AudioRoutingMatrix.BusTaps.Count);
        Assert.Equal("master", snapshot.AudioRoutingMatrix.BusTaps[0].BusId);
        Assert.Equal(-8.5, snapshot.AudioRoutingMatrix.BusTaps[0].PeakDbfs);
        Assert.Equal("Welcome to the webinar.", snapshot.CaptionTrack.CurrentCue?.Text);
        Assert.Equal("live", snapshot.OverlayState.Status);
        Assert.Equal(1, snapshot.OverlayState.LowerThirdCount);
        Assert.Equal("key:lower-third", snapshot.OverlayState.Overlays[0].OverlayId);
        Assert.Equal("p2", snapshot.OverlayState.Overlays[0].SourceId);
        Assert.Equal("on-air", snapshot.Diagnostics.OverlayState.Overlays[0].KeyPhase);
        Assert.Equal("playing", snapshot.MediaPlayback.Status);
        Assert.Equal("clip-intro", snapshot.MediaPlayback.MediaAssetId);
        Assert.Equal("program-take:3:media:clip-intro", snapshot.MediaPlayback.MediaPlaybackKey);
        Assert.Equal(@"C:\media\intro.mp4", snapshot.Diagnostics.MediaPlayback.MediaAssetPath);
        Assert.Equal(72, snapshot.Diagnostics.AudioMixSession.MasterLevel);
        Assert.Equal(2, snapshot.Diagnostics.AudioRoutingMatrix.BusTaps.Count);
        Assert.Equal(960, snapshot.Diagnostics.CaptureAudioSources.CaptureFramesReceived);
        Assert.Equal(480, snapshot.Diagnostics.CaptureAudioSources.RoutedStreamFrames);
        Assert.Equal(480, snapshot.Diagnostics.CaptureAudioSources.RoutedMonitorFrames);
        Assert.Equal(120, snapshot.Diagnostics.CaptureAudioSources.FallbackMonitorFrames);
        Assert.Equal("Sophia Martinez", snapshot.Diagnostics.CaptionTrack.CurrentCue?.Speaker);
        Assert.Empty(snapshot.AudioMixSession.Warnings);
        Assert.DoesNotContain(
            snapshot.Diagnostics.Warnings,
            warning => warning.Contains("960 PCM frames received", StringComparison.Ordinal));
        Assert.Contains("960 PCM frames received", snapshot.Diagnostics.CaptureAudioSources.Summary);
        Assert.Contains("480 stream bus frames", snapshot.Diagnostics.CaptureAudioSources.Summary);
        Assert.Contains("480 MON bus frames", snapshot.Diagnostics.CaptureAudioSources.Summary);
        Assert.Contains("480 monitor playback frames", snapshot.Diagnostics.CaptureAudioSources.Summary);
    }

    [Fact]
    public void NativeCaptureAudioWarningsStillPromoteToAudioMixWarnings()
    {
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(Commands, 3000, 12, new NativeMediaCoreWireState
        {
            AudioMixSession = new NativeMediaCoreAudioMixSession
            {
                Status = "live",
                MasterLevel = 42,
                LoudnessLufs = -18,
                MixedFrameCount = 12,
                Summary = "Program mix receiving PCM.",
                Warnings = []
            },
            CaptureAudioSources = new NativeMediaCoreCaptureAudioSources
            {
                Status = "warning",
                SourceCount = 1,
                PairedCount = 1,
                StreamingCount = 1,
                CaptureFramesReceived = 0,
                RoutedMasterFrames = 0,
                RoutedStreamFrames = 0,
                RoutedMonitorFrames = 0,
                Summary = "1 of 1 capture source paired with audio input; 1 streaming, 0 PCM frames received; 0 master bus frames, 0 stream bus frames, 0 MON bus frames, 0 monitor playback frames.",
                Warnings = ["local-machine-audio: Audio capture stream is open but no PCM frames have arrived."]
            }
        });

        Assert.Contains(
            "local-machine-audio: Audio capture stream is open but no PCM frames have arrived.",
            snapshot.AudioMixSession.Warnings);
        Assert.Contains(
            "local-machine-audio: Audio capture stream is open but no PCM frames have arrived.",
            snapshot.Diagnostics.Warnings);
        Assert.DoesNotContain(
            snapshot.Diagnostics.Warnings,
            warning => warning.StartsWith("Capture audio:", StringComparison.Ordinal));
    }

    [Fact]
    public void MapsBreakoutRoomAndMeetingStateFromWirePayload()
    {
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(Commands, 3000, 12, new NativeMediaCoreWireState
        {
            SceneId = "interview",
            RouteCount = 1,
            MeetingState = "in_meeting",
            BreakoutRoomId = "customer-panel",
            BreakoutRoomName = "Customer panel"
        });

        Assert.Equal("in_meeting", snapshot.MeetingState);
        Assert.Equal("customer-panel", snapshot.BreakoutRoomId);
        Assert.Equal("Customer panel", snapshot.BreakoutRoomName);
    }

    [Fact]
    public void NativeWireStateWithoutNativeFramesWaitsForFirstCompositorFrame()
    {
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(Commands, 3000, 12, new NativeMediaCoreWireState
        {
            SceneId = "interview",
            RouteCount = 1,
            CompositorRenderer = "software",
            Health = new NativeMediaCoreWireHealth
            {
                Status = "idle",
                Renderer = "software",
                ProgramFrameHealth = "live",
                FrameCount = 0
            },
            Profile = new NativeMediaCoreProfile
            {
                Name = "CoreVideo Pro Native Media Core Stub",
                Renderer = "software",
                MaxProgramResolution = "1920x1080",
                MaxProgramFps = 30,
                MaxParticipantFeeds = 8,
                MaxIsoRecordings = 8,
                Capabilities = ["scene-graph-rendering"]
            }
        });

        Assert.Equal(0, snapshot.ProgramFrameCount);
        Assert.Null(snapshot.ProgramFrame);
        Assert.Equal("idle", snapshot.Compositor.Status);
        Assert.Equal("idle", snapshot.ProgramTransport.Status);
    }

    [Fact]
    public void NativeWireStateCarriesDegradedProgramFrameHealthToCompositor()
    {
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(Commands, 3000, 12, new NativeMediaCoreWireState
        {
            SceneId = "interview",
            RouteCount = 1,
            ProgramFrameCount = 7,
            RenderPlanId = "interview:degraded",
            CompositorRenderer = "software",
            Health = new NativeMediaCoreWireHealth
            {
                Status = "live",
                Renderer = "software",
                ProgramFrameHealth = "degraded",
                FrameCount = 7
            }
        });

        Assert.Equal(7, snapshot.ProgramFrameCount);
        Assert.Equal("degraded", snapshot.ProgramFrame?.Health);
        Assert.Equal("degraded", snapshot.Compositor.Status);
        Assert.Equal("publishing", snapshot.ProgramTransport.Status);
    }
}
