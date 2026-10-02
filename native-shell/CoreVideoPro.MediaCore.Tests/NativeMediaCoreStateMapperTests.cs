using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class NativeMediaCoreStateMapperTests
{
    [Theory]
    [InlineData("snapshot")]
    [InlineData("state")]
    public void PreviewSceneSurvivesRealWireParserAndMapper(string envelope)
    {
        using var response = JsonDocument.Parse("{\"ok\":true,\"" + envelope + "\":" + """
            {"health":null,"profile":null,"sceneId":"native-program","previewScene":{
              "sceneId":"native-preview","routeCount":2,"layerCount":3,"composite":true}}}
            """);
        var wire = CoreProtocolParser.TryParseWireState(response);
        Assert.NotNull(wire);
        var commands = new[] { MediaCoreCommandBuilder.BuildPreviewSceneCommand("desired-preview", []) };
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(100, 3, wire);
        Assert.Equal("native-program", snapshot.SceneId);
        Assert.NotNull(snapshot.PreviewScene);
        Assert.Equal("native-preview", snapshot.PreviewScene.SceneId);
        Assert.Equal(2, snapshot.PreviewScene.RouteCount);
        Assert.Equal(3, snapshot.PreviewScene.LayerCount);
        Assert.True(snapshot.PreviewScene.Composite);
    }

    [Fact]
    public void MissingNativePreviewDoesNotInventObservedPreviewFromCommand()
    {
        using var response = JsonDocument.Parse("""{"ok":true,"snapshot":{"health":null,"profile":null,"sceneId":"native-program"}}""");
        var wire = CoreProtocolParser.TryParseWireState(response);
        Assert.NotNull(wire);
        var commands = new[] { MediaCoreCommandBuilder.BuildPreviewSceneCommand("desired-preview", []) };
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(100, 3, wire);
        Assert.Null(snapshot.PreviewScene);
    }

    [Fact]
    public void NestedProgramFramePreservesActualSceneAndSourcesAcrossCommandAcknowledgment()
    {
        using var response = JsonDocument.Parse("""
            {"ok":true,"snapshot":{"health":null,"profile":null,"sceneId":"new-scene","programFrameCount":10,
            "programFrame":{"frameNumber":9,"sceneId":"old-scene","renderPlanId":"old-scene:2:0","health":"live",
            "videoSources":[{"layerId":"speaker","sourceId":"zoom:jamal","participantId":"jamal","kind":"participant-video"}]}}}
            """);
        var wire = CoreProtocolParser.TryParseWireState(response)!;
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(0, 0, wire);
        Assert.Equal("new-scene", snapshot.SceneId);
        Assert.Equal("old-scene", snapshot.ProgramFrame?.SceneId);
        Assert.Equal(9, snapshot.ProgramFrame?.FrameNumber);
        Assert.Equal("zoom:jamal", Assert.Single(snapshot.ProgramFrame!.VideoSources!).SourceId);
    }

    [Fact]
    public void LegacyFrameCannotInventActualSourceOrSceneFromDesiredCommands()
    {
        using var response = JsonDocument.Parse("""{"ok":true,"snapshot":{"health":null,"profile":null,"sceneId":"desired","programFrameCount":10,"renderPlanId":"desired:2:0"}}""");
        var wire = CoreProtocolParser.TryParseWireState(response)!;
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(0, 0, wire);
        Assert.Null(snapshot.ProgramFrame?.SceneId);
        Assert.Null(snapshot.ProgramFrame?.VideoSources);
    }

    [Fact]
    public void MergesNativeEncoderRecordingAndSenderStateIntoSnapshot()
    {
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(3000, 12, new NativeMediaCoreWireState
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
                ProgramFrameHealth = "live",
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
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(3000, 12, new NativeMediaCoreWireState
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
    public void MapsOutputSenderResultAndRuntimeDetailIntoOutputHealthWhenWarningIsMissing()
    {
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(3000, 12, new NativeMediaCoreWireState
        {
            Outputs = ["ndi"],
            OutputSenderSession = new NativeMediaCoreOutputSenderSession
            {
                Status = "warning",
                ActiveSenderCount = 1,
                Senders =
                [
                    new NativeMediaCoreOutputSender
                    {
                        SenderId = "ndi:program",
                        Destination = "ndi",
                        Status = "warning",
                        FramesSent = 0,
                        RetryCount = 0,
                        LatencyMs = 0,
                        BitrateMbps = 0,
                        LastResultCode = "ndi-output-unavailable",
                        RuntimeDetail = "LibNDI runtime-missing on this machine."
                    }
                ]
            }
        });

        var ndi = Assert.Single(snapshot.OutputHealth, item => item.Destination == "ndi");
        Assert.Equal("warning", ndi.Status);
        Assert.Contains("ndi-output-unavailable", ndi.Message, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("runtime-missing", ndi.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void MergesNativeAudioMixAndCaptionTrackStateFromWirePayload()
    {
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(3000, 12, new NativeMediaCoreWireState
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
            // A LEGACY core's mediaPlayback shape (pre-#535-slice-3b vocabulary, with a
            // playback key): the mapper must pass it through verbatim rather than reinterpret it.
            MediaPlayback = new NativeMediaCoreMediaPlaybackState
            {
                Status = "playing",
                MediaAssetId = "clip-intro",
                MediaAssetName = "Intro Sting",
                MediaAssetKind = "stinger",
                MediaAssetPath = @"C:\media\intro.mp4",
                MediaPlaybackKey = "media:clip-intro:live:3",
                Playing = true,
                Summary = "Playing Intro Sting with key media:clip-intro:live:3.",
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
        Assert.Equal("media:clip-intro:live:3", snapshot.MediaPlayback.MediaPlaybackKey);
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
    public void OptionalCaptureAudioWarningsStayScopedToTheCaptureSource()
    {
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(3000, 12, new NativeMediaCoreWireState
        {
            AudioMixSession = new NativeMediaCoreAudioMixSession
            {
                Status = "live",
                MasterLevel = 42,
                LoudnessLufs = -18,
                PluginHost = new NativeMediaCorePluginHost
                {
                    Status = "ready",
                    Plugins =
                    [
                        new NativeMediaCorePluginInfo
                        {
                            Id = "waves",
                            Name = "WaveShell",
                            Probe = "pass",
                            ClassNames = ["Curves AQ Stereo"]
                        }
                    ]
                },
                MasteringEnabled = true,
                MasteringRideDb = 1.5,
                // B2: the post-mastering master meter must survive the mapper's
                // warning-merge copy (CopyAudioMixSession) untouched.
                MasterMeter = new NativeMediaCoreMasterMeter
                {
                    MomentaryLufs = -13.2,
                    ShortTermLufs = -14.1,
                    IntegratedLufs = -14.6,
                    TruePeakDbfs = -1.4,
                    WindowMs = 3000
                },
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

        Assert.Empty(snapshot.AudioMixSession.Warnings);
        Assert.Equal("ready", snapshot.AudioMixSession.PluginHost.Status);
        Assert.Equal("Curves AQ Stereo", Assert.Single(snapshot.AudioMixSession.PluginHost.Plugins).ClassNames[0]);
        Assert.True(snapshot.AudioMixSession.MasteringEnabled);
        Assert.Equal(1.5, snapshot.AudioMixSession.MasteringRideDb);
        Assert.Equal(-14.6, snapshot.AudioMixSession.MasterMeter.IntegratedLufs);
        Assert.Equal(-1.4, snapshot.AudioMixSession.MasterMeter.TruePeakDbfs);
        Assert.Equal(3000, snapshot.AudioMixSession.MasterMeter.WindowMs);
        Assert.DoesNotContain(
            "local-machine-audio: Audio capture stream is open but no PCM frames have arrived.",
            snapshot.Diagnostics.Warnings);
        Assert.Contains(
            "local-machine-audio: Audio capture stream is open but no PCM frames have arrived.",
            snapshot.Diagnostics.CaptureAudioSources.Warnings);
        Assert.DoesNotContain(
            snapshot.Diagnostics.Warnings,
            warning => warning.StartsWith("Capture audio:", StringComparison.Ordinal));
    }

    [Fact]
    public void MapsBreakoutRoomAndMeetingStateFromWirePayload()
    {
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(3000, 12, new NativeMediaCoreWireState
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
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(3000, 12, new NativeMediaCoreWireState
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
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(3000, 12, new NativeMediaCoreWireState
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

    [Fact]
    public void CarriesVirtualCameraStatusFromWireState()
    {
        // Regression: the wire-state mapper synthesizes a fresh base (virtual
        // camera off), so it must read the camera status from the wire - else the
        // shell shows the camera off even when the core reports it live.
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(3000, 12, new NativeMediaCoreWireState
        {
            VirtualCamera = new NativeMediaCoreVirtualCamera
            {
                Enabled = true,
                Status = "live",
                Resolution = new NativeMediaCoreVirtualCameraResolution { Width = 1280, Height = 720 },
                Fps = 30
            }
        });

        Assert.Equal("live", snapshot.VirtualCamera.Status);
        Assert.True(snapshot.VirtualCamera.Enabled);
        Assert.Equal(1280, snapshot.VirtualCamera.Resolution.Width);
    }

    [Fact]
    public void CarriesNativeRenderDropsIntoTransportAndSupportTelemetry()
    {
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(3000,
            12,
            new NativeMediaCoreWireState
            {
                ProgramFrameCount = 120,
                Compositor = new NativeMediaCoreCompositorState
                {
                    Status = "live",
                    ProgramFrameCount = 120,
                    DroppedFrameCount = 1499
                }
            });

        Assert.Equal(1499, snapshot.Compositor.DroppedFrameCount);
        Assert.Equal(1499, snapshot.Diagnostics.Compositor.DroppedFrameCount);
    }

    // #535 slice 3b: mediaSources is the core's per-source media TRANSPORT truth. It must
    // survive the real wire parser and the mapper, and default to EMPTY (never null) so a
    // reader can ask "is this clip live" without a null check.
    [Fact]
    public void MediaSourcesRoundTripFromTheWire()
    {
        using var response = JsonDocument.Parse("""
            {"ok":true,"snapshot":{"health":null,"profile":null,"sceneId":"media-program",
            "mediaSources":[
              {"sourceId":"media:clip-intro","mediaAssetId":"clip-intro","state":"live","loop":false,
               "onProgram":true,"onPreview":false,"positionMs":1250,"durationMs":9000},
              {"sourceId":"background:bg-loop","mediaAssetId":"bg-loop","state":"cued","loop":true,
               "onProgram":false,"onPreview":true,"positionMs":0,"durationMs":-1}]}}
            """);
        var wire = CoreProtocolParser.TryParseWireState(response);
        Assert.NotNull(wire);
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(0, 0, wire);

        Assert.Equal(2, snapshot.MediaSources.Count);
        var clip = snapshot.MediaSources[0];
        Assert.Equal("media:clip-intro", clip.SourceId);
        Assert.Equal("clip-intro", clip.MediaAssetId);
        Assert.Equal("live", clip.State);
        Assert.False(clip.Loop);
        Assert.True(clip.OnProgram);
        Assert.False(clip.OnPreview);
        Assert.Equal(1250, clip.PositionMs);
        Assert.Equal(9000, clip.DurationMs);

        var loop = snapshot.MediaSources[1];
        Assert.Equal("background:bg-loop", loop.SourceId);
        Assert.Equal("cued", loop.State);
        Assert.True(loop.Loop);
        Assert.True(loop.OnPreview);
        Assert.Equal(-1, loop.DurationMs);
    }

    [Fact]
    public void MediaSourcesDefaultToAnEmptyListWhenTheCoreSendsNone()
    {
        using var response = JsonDocument.Parse("""{"ok":true,"snapshot":{"health":null,"profile":null,"sceneId":"s"}}""");
        var wire = CoreProtocolParser.TryParseWireState(response)!;
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(0, 0, wire);
        Assert.Empty(snapshot.MediaSources);
    }

    // #740: the mapper's base used to be synthesized from the commands the shell sent, so a
    // node the core omitted read as the healthy outcome the shell had asked for. These pin
    // the rule that only the core's own JSON can make a snapshot say something is happening.

    private static NativeMediaCoreStateSnapshot MapJson(string snapshotJson)
    {
        using var response = JsonDocument.Parse("{\"ok\":true,\"snapshot\":" + snapshotJson + "}");
        return NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(
            3000, 12, CoreProtocolParser.TryParseWireState(response)!);
    }

    [Fact]
    public void ACoreThatPublishesNoRecordingNodeReportsNoRecording()
    {
        var snapshot = MapJson("""{"health":{"frameCount":12},"profile":null,"outputs":["recording","rtmp"],"programFrameCount":12}""");

        Assert.Null(snapshot.Recording);
        Assert.Null(snapshot.Diagnostics.Recording);
        Assert.Equal("idle", snapshot.OutputHealth.Single(health => health.Destination == "recording").Status);
    }

    [Fact]
    public void ACoreThatPublishesNoSendersReportsNoLiveStream()
    {
        var snapshot = MapJson("""{"health":{"frameCount":12},"profile":null,"outputs":["rtmp"],"programFrameCount":12}""");

        Assert.Empty(snapshot.OutputSenderSession.Senders);
        Assert.Equal("idle", snapshot.OutputSenderSession.Status);
        Assert.Equal("idle", snapshot.EncoderSession.Status);
        Assert.Equal("idle", snapshot.OutputHealth.Single(health => health.Destination == "rtmp").Status);
    }

    [Fact]
    public void ACoreThatPublishesNoPreviewPixelsHasNoProgramPreview()
    {
        var snapshot = MapJson("""{"health":{"frameCount":12},"profile":null,"outputs":["rtmp"],"programFrameCount":12}""");

        Assert.Null(snapshot.ProgramFramePreview);
        Assert.Null(snapshot.Diagnostics.ProgramFramePreview);
    }

    [Fact]
    public void AnUndecodablePreviewIsDroppedNotReplaced()
    {
        var snapshot = MapJson("""{"health":{"frameCount":12},"profile":null,"programFrameCount":12,"programFramePreview":{"frameNumber":12,"width":320,"height":180,"renderPlanId":"p","renderer":"d3d11","health":"live","pixelFormat":"bgra","bgraBase64":"AAAA"}}""");

        Assert.Null(snapshot.ProgramFramePreview);
    }

    [Fact]
    public void TheOutputProfileIsTheCoresNotAHardCodedDefault()
    {
        var snapshot = MapJson("""{"health":{},"profile":null,"outputProfile":{"profileId":"720p30","resolution":"1280x720","width":1280,"height":720,"fps":30,"targetBitrateMbps":4.5}}""");

        Assert.Equal("720p30", snapshot.OutputProfile.ProfileId);
        Assert.Equal(1280, snapshot.OutputProfile.Width);
        Assert.Equal(30, snapshot.OutputProfile.Fps);
        Assert.Equal(4.5, snapshot.OutputProfile.TargetBitrateMbps);
        Assert.Equal("720p30", snapshot.Diagnostics.OutputProfile.ProfileId);
    }

    [Theory]
    [InlineData("""{"frameCount":12}""")]
    [InlineData("""{"frameCount":12,"programFrameHealth":"sparkling"}""")]
    public void AMissingOrUnrecognisedFrameHealthIsNotReportedLive(string health)
    {
        var snapshot = MapJson("{\"health\":" + health + ",\"profile\":null,\"programFrameCount\":12}");

        Assert.Equal("unknown", snapshot.Compositor.Status);
    }

    [Fact]
    public void LiveFrameHealthIsStillReportedLive()
    {
        var snapshot = MapJson("""{"health":{"frameCount":12,"programFrameHealth":"live"},"profile":null,"programFrameCount":12}""");

        Assert.Equal("live", snapshot.Compositor.Status);
    }

    [Fact]
    public void TheSourceSnapshotClaimsNoSubscriptionsOrFramesTheCoreDidNotReport()
    {
        var snapshot = MapJson("""{"health":{"frameCount":12},"profile":null,"outputs":["rtmp"],"programFrameCount":12}""");

        Assert.Equal("idle", snapshot.SourceSnapshot.Status);
        Assert.Equal(0, snapshot.SourceSnapshot.SubscribedSourceCount);
        Assert.Equal(0, snapshot.SourceSnapshot.LiveFrameCount);
        Assert.Null(snapshot.MeetingState);
        Assert.Null(snapshot.BreakoutRoomId);
    }

    [Fact]
    public void AnEmptyRecordingArtifactPathAddsNoWarning()
    {
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(3000, 12, new NativeMediaCoreWireState
        {
            Health = new NativeMediaCoreWireHealth { RecordingArtifactPath = "" },
            Recording = new NativeMediaCoreRecordingSession
            {
                SessionId = "show-1",
                Active = true,
                Status = "recording",
                TargetFolder = "Recordings",
                WriterStatus = "writing",
                ProgramPath = "Recordings/program.mp4",
                FilenamePrefix = "program",
                Format = "mp4",
                Quality = "high",
                Encoder = new NativeMediaCoreRecordingEncoder { Codec = "h264" },
                Streams = []
            }
        });

        Assert.DoesNotContain(snapshot.Warnings, warning => warning.StartsWith("Recording artifact", StringComparison.Ordinal));
    }

    // #760: SRT/RTMP ingest status reads the snapshot's CaptureDevices. A real core's state
    // takes the wire path, which used to drop the node.
    [Fact]
    public void CaptureDevicesFromARealCoreStateReachTheSnapshot()
    {
        var snapshot = MapJson("""{"health":{"frameCount":12},"profile":null,"captureDevices":[{"id":"srt:feed-1","vendor":"srt","name":"Remote feed","inputs":[],"selectedInputId":"","resolution":{"width":1920,"height":1080},"frameRate":30,"connectionState":"connected","signalPresent":true,"droppedFrames":0,"audioSyncOffsetMs":0,"lastFrameAgeMs":16,"decoderFailures":2,"rttMs":41.5,"rttStatus":"measured"},{"id":"screen:0","vendor":"Windows Graphics Capture","name":"DISPLAY1","inputs":[],"selectedInputId":"","resolution":{"width":2560,"height":1440},"frameRate":60,"connectionState":"detected","signalPresent":false,"droppedFrames":0,"audioSyncOffsetMs":0}]}""");

        Assert.Equal(2, snapshot.CaptureDevices.Count);
        var srt = snapshot.CaptureDevices.Single(device => device.Id == "srt:feed-1");
        Assert.Equal("srt", srt.Vendor);
        Assert.Equal("connected", srt.ConnectionState);
        Assert.True(srt.SignalPresent);
        Assert.Equal(1920, srt.Width);
        Assert.Equal(1080, srt.Height);
        Assert.Equal(16, srt.LastFrameAgeMs);
        Assert.Equal(2, srt.DecoderFailures);
        Assert.Equal(41.5, srt.RttMs);
        Assert.Equal("measured", srt.RttStatus);
    }

    [Fact]
    public void AStateWithNoCaptureDeviceNodeYieldsAnEmptyList()
    {
        var snapshot = MapJson("""{"health":{"frameCount":12},"profile":null}""");

        Assert.Empty(snapshot.CaptureDevices);
    }

    // #759: a sender that is still connecting is "starting", not "live".
    [Fact]
    public void AStartingSenderIsNotReportedLive()
    {
        var snapshot = MapJson("""{"health":{"frameCount":12},"profile":null,"outputs":["rtmp"],"outputSenderSession":{"status":"starting","activeSenderCount":0,"senders":[{"senderId":"rtmp:program","destination":"rtmp","status":"starting","framesSent":0,"retryCount":0,"latencyMs":0,"bitrateMbps":0}],"warnings":[]}}""");

        Assert.Equal("starting", snapshot.OutputHealth.Single(health => health.Destination == "rtmp").Status);
        Assert.False(LiveProductionSync.IsStreamingLive(snapshot));
        Assert.DoesNotContain("Live", MediaCoreBridgeService.SummarizeOutputs(snapshot), StringComparison.Ordinal);
    }
}
