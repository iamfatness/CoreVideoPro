using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class LiveProductionSyncTests
{
    private static readonly LiveProductionSync.LiveProductionSyncContext Context = new()
    {
        ActiveSceneId = "speaker-slides",
        ActiveSceneLayout = "speaker-slides",
        CurrentBreakoutRoomId = "main",
        Participants =
        [
            new()
            {
                Id = "p1",
                Name = "Sophia Martinez",
                Title = "Host",
                RoleLabel = "Host",
                BreakoutRoomName = "Main room",
                IsActiveSpeaker = false,
                IsScreenSharing = false
            },
            new()
            {
                Id = "p2",
                Name = "David Chen",
                Title = "Chief Product Officer",
                RoleLabel = "Presenter",
                BreakoutRoomName = "Main room",
                IsActiveSpeaker = true,
                IsScreenSharing = true
            }
        ]
    };

    [Fact]
    public void MapsCaptionOverlayRecordingAndStreamingFromSnapshot()
    {
        var snapshot = BuildSnapshot(
            captionText: "Welcome to the webinar.",
            captionSpeaker: "Sophia Martinez",
            recordingActive: true,
            streamingLive: true);

        var patch = LiveProductionSync.MapSnapshotToStudioPatch(snapshot, Context);

        Assert.Equal("Welcome to the webinar.", patch.CaptionText);
        Assert.Equal("Sophia Martinez", patch.CaptionSpeaker);
        Assert.True(patch.Recording);
        Assert.True(patch.Streaming);
        Assert.Contains("Recording", patch.OutputStatus);
        Assert.Contains("recording", patch.OutputSessionStatus!, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void ResolvesLowerThirdFromActiveSpeakerRoute()
    {
        var snapshot = WithRenderRoutes(BuildSnapshot(),
        [
            new NativeMediaCoreResolvedRoute
            {
                RouteId = "program",
                Mode = "active-speaker",
                AudioRole = "mix",
                ParticipantId = "p2",
                Status = "resolved"
            }
        ]);

        var lowerThird = LiveProductionSync.ResolveProgramLowerThird(snapshot, Context);

        Assert.Equal("David Chen", lowerThird.Name);
        Assert.Equal("Chief Product Officer", lowerThird.Title);
        Assert.Equal("Main room", lowerThird.Org);
        Assert.Equal("p2", lowerThird.ParticipantId);
    }

    [Fact]
    public void EmitsBreakoutRoomChangeHintWhenSnapshotRoomDiffers()
    {
        var snapshot = BuildSnapshot() with
        {
            BreakoutRoomId = "customer-panel",
            BreakoutRoomName = "Customer panel"
        };

        var patch = LiveProductionSync.MapSnapshotToStudioPatch(snapshot, Context);

        Assert.Contains("Customer panel", patch.BreakoutRoomChangeHint);
        Assert.Contains("turn engine off", patch.BreakoutRoomChangeHint, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void ResolvesAutoStopStatusWhenSnapshotRoomDiffers()
    {
        var snapshot = BuildSnapshot() with
        {
            BreakoutRoomId = "customer-panel",
            BreakoutRoomName = "Customer panel"
        };

        var status = LiveProductionSync.ResolveBreakoutRoomAutoStopStatus(snapshot, Context.CurrentBreakoutRoomId);
        var patch = LiveProductionSync.MapSnapshotToStudioPatch(snapshot, Context);

        Assert.Equal("Engine auto-stopped — breakout room changed to Customer panel", status);
        Assert.Equal(status, patch.EngineAutoStopStatus);
    }

    [Fact]
    public void LeavesAutoStopStatusNullWhenWireFieldAbsent()
    {
        var patch = LiveProductionSync.MapSnapshotToStudioPatch(BuildSnapshot(), Context);

        Assert.Null(LiveProductionSync.ResolveBreakoutRoomAutoStopStatus(BuildSnapshot(), Context.CurrentBreakoutRoomId));
        Assert.Null(patch.EngineAutoStopStatus);
    }

    [Fact]
    public void LeavesBreakoutRoomHintNullWhenWireFieldAbsent()
    {
        var patch = LiveProductionSync.MapSnapshotToStudioPatch(BuildSnapshot(), Context);

        Assert.Null(patch.BreakoutRoomChangeHint);
    }

    [Fact]
    public void InfersMeetingStateFromZoomSourceSnapshotStub()
    {
        var snapshot = BuildSnapshot() with
        {
            SourceSnapshot = new NativeMediaCoreFrameSourceSnapshot
            {
                AdapterId = "zoom-sdk",
                Kind = "zoom-sdk",
                Status = "subscribed"
            }
        };

        var patch = LiveProductionSync.MapSnapshotToStudioPatch(snapshot, Context);

        Assert.Equal("in_meeting", patch.MeetingStateLabel);
        Assert.Equal("Zoom Connected", patch.ZoomStatus);
    }

    [Fact]
    public void CreateDemoFallbackPatchRestoresIdleDemoReadouts()
    {
        var patch = LiveProductionSync.CreateDemoFallbackPatch();

        Assert.Equal(LiveProductionSync.DemoDefaults.CaptionSpeaker, patch.CaptionSpeaker);
        Assert.Equal(LiveProductionSync.DemoDefaults.CaptionText, patch.CaptionText);
        Assert.Equal(LiveProductionSync.DemoDefaults.LowerThirdName, patch.LowerThirdName);
        Assert.False(patch.Recording);
        Assert.False(patch.Streaming);
        Assert.Equal(LiveProductionSync.DemoDefaults.OutputStatus, patch.OutputStatus);
        Assert.Null(patch.BreakoutRoomChangeHint);
    }

    private static NativeMediaCoreStateSnapshot BuildSnapshot(
        string? captionText = null,
        string? captionSpeaker = null,
        bool recordingActive = false,
        bool streamingLive = false)
    {
        NativeMediaCoreRecordingSession? recording = recordingActive
            ? new NativeMediaCoreRecordingSession
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
                ProgramPath = "Recordings/program-program-0.mp4"
            }
            : null;

        var outputHealth = streamingLive
            ? new List<NativeMediaCoreOutputHealth>
            {
                new()
                {
                    Destination = "rtmp",
                    Status = "live",
                    Message = "RTMP sender live.",
                    DroppedFrames = 0
                }
            }
            : [];

        var outputSenderSession = streamingLive
            ? new NativeMediaCoreOutputSenderSession
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
            }
            : new NativeMediaCoreOutputSenderSession { Status = "idle" };

        NativeMediaCoreCaptionCue? cue = captionText is null && captionSpeaker is null
            ? null
            : new NativeMediaCoreCaptionCue
            {
                Text = captionText ?? string.Empty,
                Speaker = captionSpeaker,
                AtMs = 1000,
                Confidence = 95
            };

        return new NativeMediaCoreStateSnapshot
        {
            CaptionTrack = new NativeMediaCoreCaptionTrack
            {
                Enabled = cue is not null,
                Status = cue is null ? "idle" : "live",
                CurrentCue = cue,
                LatencyMs = 180
            },
            BrandKit = new NativeMediaCoreBrandKit
            {
                Name = "CoreVideo",
                LogoText = "CV",
                BrandColor = "#111111",
                AccentColor = "#3366ff",
                BackgroundColor = "#000000",
                FontFamily = "Inter",
                LowerThirdStyle = "solid",
                Summary = "Brand kit live"
            },
            Recording = recording,
            OutputHealth = outputHealth,
            OutputSenderSession = outputSenderSession,
            EncoderSession = new NativeMediaCoreEncoderSession
            {
                Status = streamingLive ? "encoding" : "idle",
                Lifecycle = new NativeMediaCoreEncoderLifecycle
                {
                    Status = streamingLive ? "encoding" : "idle",
                    LastTransition = "test"
                }
            }
        };
    }
    private static NativeMediaCoreStateSnapshot WithRenderRoutes(
        NativeMediaCoreStateSnapshot snapshot,
        IReadOnlyList<NativeMediaCoreResolvedRoute> routes)
    {
        var renderPlan = new NativeMediaCoreRenderPlan
        {
            RenderPlanId = snapshot.RenderPlan.RenderPlanId,
            SceneId = snapshot.RenderPlan.SceneId,
            OutputProfile = snapshot.RenderPlan.OutputProfile,
            ColorGrade = snapshot.RenderPlan.ColorGrade,
            SourceCount = snapshot.RenderPlan.SourceCount,
            ResolvedRouteCount = routes.Count,
            Layers = snapshot.RenderPlan.Layers,
            Routes = routes,
            Warnings = snapshot.RenderPlan.Warnings
        };

        return snapshot with { RenderPlan = renderPlan };
    }
}