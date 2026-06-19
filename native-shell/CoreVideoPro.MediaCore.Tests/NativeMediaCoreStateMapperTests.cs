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
                MaxParticipantFeeds = 6,
                MaxIsoRecordings = 2,
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
        Assert.Contains("corevideo-mf-recording-123.mp4", snapshot.Warnings[0]);
        Assert.Contains(
            snapshot.OutputHealth,
            item => item.Destination == "rtmp" && item.Status == "live");
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
            }
        });

        Assert.Equal("Program mix balanced", snapshot.AudioMixSession.Summary);
        Assert.Equal(72, snapshot.AudioMixSession.MasterLevel);
        Assert.Equal("Welcome to the webinar.", snapshot.CaptionTrack.CurrentCue?.Text);
        Assert.Equal(72, snapshot.Diagnostics.AudioMixSession.MasterLevel);
        Assert.Equal("Sophia Martinez", snapshot.Diagnostics.CaptionTrack.CurrentCue?.Speaker);
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
                MaxParticipantFeeds = 6,
                MaxIsoRecordings = 2,
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
