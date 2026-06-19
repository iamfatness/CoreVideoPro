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
        Assert.Equal("Zoom Live", patch.ZoomStatus);
    }

    [Fact]
    public void MapSnapshotParticipantsAcceptsHyphenatedInMeetingState()
    {
        var snapshot = BuildSnapshot() with
        {
            MeetingState = "in-meeting",
            Participants =
            [
                new RawParticipantEvent
                {
                    UserId = "42",
                    DisplayName = "Alex Host",
                    Role = "Host"
                }
            ]
        };

        var participants = LiveProductionSync.MapSnapshotParticipants(snapshot);

        Assert.NotNull(participants);
        Assert.Single(participants!);
        Assert.Equal("Alex Host", participants![0].Name);
    }

    [Fact]
    public void MapsRawParticipantsIntoLiveProductionContext()
    {
        var participants = LiveProductionSync.MapRawParticipants(
        [
            new RawParticipantEvent
            {
                UserId = "42",
                DisplayName = "  Dana Lee  ",
                Role = "Presenter",
                Title = "Demo Lead",
                BreakoutRoomId = "green-room",
                BreakoutRoomName = "Green Room",
                Talking = false,
                SharingScreen = true,
                AudioLevel = 91,
                NetworkQuality = "good"
            },
            new RawParticipantEvent
            {
                UserId = "guest-7",
                DisplayName = "",
                VideoOn = false,
                NetworkQuality = "low"
            }
        ],
        activeSpeakerId: "42");

        Assert.Equal("Dana Lee", participants[0].Name);
        Assert.Equal("Demo Lead", participants[0].Title);
        Assert.Equal("Presenter", participants[0].RoleLabel);
        Assert.Equal("green-room", participants[0].BreakoutRoomId);
        Assert.Equal("Green Room", participants[0].BreakoutRoomName);
        Assert.True(participants[0].IsActiveSpeaker);
        Assert.True(participants[0].IsScreenSharing);
        Assert.Equal("live", participants[0].HealthLabel);
        Assert.Equal("guest-7", participants[1].Id);
        Assert.Equal("Zoom User guest-7", participants[1].Name);
        Assert.Equal("video-off", participants[1].HealthLabel);
    }

    [Fact]
    public void MapsSnapshotParticipantsWhenMeetingIsLive()
    {
        var snapshot = BuildSnapshot() with
        {
            MeetingState = "in_meeting",
            ActiveSpeakerId = "operator-1",
            Participants =
            [
                new RawParticipantEvent
                {
                    UserId = "operator-1",
                    DisplayName = "Operator",
                    Role = "Host",
                    Talking = true,
                    VideoOn = true
                },
                new RawParticipantEvent
                {
                    UserId = "guest-1",
                    DisplayName = "Guest 1",
                    Role = "Guest",
                    VideoOn = true
                }
            ]
        };

        var patch = LiveProductionSync.MapSnapshotToStudioPatch(snapshot, Context);

        Assert.NotNull(patch.Participants);
        Assert.Equal(2, patch.Participants!.Count);
        Assert.Equal("Operator", patch.Participants[0].Name);
        Assert.Equal("Guest 1", patch.Participants[1].Name);
        Assert.Equal("in_meeting", patch.MeetingStateLabel);
    }

    [Fact]
    public void MapsSophiaStyleHostParticipantFromCaptureSnapshot()
    {
        var participants = LiveProductionSync.MapCaptureSnapshotParticipants(new RawCaptureSnapshot
        {
            MeetingState = "in_meeting",
            Tick = 2,
            ActiveSpeakerId = "operator-1",
            Participants =
            [
                new RawParticipantEvent
                {
                    UserId = "operator-1",
                    DisplayName = "Sophia Martinez",
                    Role = "Host",
                    Title = "Executive Producer",
                    BreakoutRoomId = "main",
                    BreakoutRoomName = "Main room",
                    Talking = true,
                    VideoOn = true,
                    SharingScreen = true,
                    AudioLevel = 82
                }
            ]
        });

        Assert.Single(participants);
        Assert.Equal("operator-1", participants[0].Id);
        Assert.Equal("Sophia Martinez", participants[0].Name);
        Assert.Equal("Host", participants[0].RoleLabel);
        Assert.Equal("main", participants[0].BreakoutRoomId);
        Assert.True(participants[0].IsActiveSpeaker);
        Assert.True(participants[0].IsScreenSharing);
        Assert.Equal("live", participants[0].HealthLabel);
    }

    [Fact]
    public void MapsCaptureSnapshotParticipantsForInMeetingState()
    {
        var participants = LiveProductionSync.MapCaptureSnapshotParticipants(new RawCaptureSnapshot
        {
            MeetingState = "in_meeting",
            Tick = 3,
            ActiveSpeakerId = "live-1",
            Participants =
            [
                new RawParticipantEvent
                {
                    UserId = "live-1",
                    DisplayName = "Live Host",
                    Role = "Host",
                    Title = "Executive Producer",
                    Talking = true,
                    VideoOn = true
                }
            ]
        });

        Assert.Single(participants);
        Assert.Equal("Live Host", participants[0].Name);
        Assert.Equal("Executive Producer", participants[0].Title);
        Assert.True(participants[0].IsActiveSpeaker);
    }

    [Fact]
    public void SummarizeOutputSessionForRecordingAndStreaming()
    {
        var snapshot = BuildSnapshot(recordingActive: true, streamingLive: true);

        var sessionStatus = LiveProductionSync.SummarizeOutputSession(snapshot);

        Assert.Contains("Recording", sessionStatus);
        Assert.Contains("program", sessionStatus, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void SummarizeOutputSessionReportsIdleWhenOutputsDisabled()
    {
        var snapshot = BuildSnapshot();

        Assert.Equal("Outputs idle", LiveProductionSync.SummarizeOutputSession(snapshot));
    }

    [Fact]
    public void IsStreamingLiveUsesSenderSessionWhenOutputHealthIsEmpty()
    {
        var snapshot = BuildSnapshot() with
        {
            OutputHealth = [],
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
                        FramesSent = 4,
                        RetryCount = 0,
                        LatencyMs = 1800,
                        BitrateMbps = 6
                    }
                ]
            }
        };

        Assert.True(LiveProductionSync.IsStreamingLive(snapshot));
    }

    [Fact]
    public void MapsTransportReadoutsFromIdleSnapshotPatch()
    {
        var patch = LiveProductionSync.MapSnapshotToStudioPatch(BuildSnapshot(), Context);

        Assert.False(patch.Recording);
        Assert.False(patch.Streaming);
        Assert.Equal("Outputs idle", patch.OutputStatus);
        Assert.Equal("Outputs idle", patch.OutputSessionStatus);
    }

    [Fact]
    public void CreateIdleProductionPatchReturnsNeutralReadouts()
    {
        var patch = LiveProductionSync.CreateIdleProductionPatch();

        Assert.Null(patch.CaptionSpeaker);
        Assert.Null(patch.CaptionText);
        Assert.Null(patch.LowerThirdName);
        Assert.False(patch.Recording);
        Assert.False(patch.Streaming);
        Assert.Equal("Outputs idle", patch.OutputStatus);
        Assert.Equal("Outputs idle", patch.OutputSessionStatus);
        Assert.Equal("Zoom Offline", patch.ZoomStatus);
        Assert.Equal("idle", patch.MeetingStateLabel);
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
                CaptionStyle = "medium sentence captions",
                DefaultOverlayBehavior = "brand-bug",
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
