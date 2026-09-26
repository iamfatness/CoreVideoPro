using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class ZoomCaptureSnapshotMergerTests
{
    [Fact]
    public void VersionedRosterConvergesMuteWithoutPreviewAndRejectsReusedIdFromOldMeeting()
    {
        // The same generated wire vocabulary validated by all four language
        // fixtures drives the shipping roster consumer below.
        var barrier = System.Text.Json.JsonSerializer.Deserialize<ZoomRosterSnapshotRevision>(
            "{\"rosterEpoch\":\"1:1:engine-a\",\"rosterRevision\":1}")!;
        Assert.Equal(1, barrier.RosterRevision);
        static RawCaptureSnapshot Capture(string epoch, long revision, string name, bool muted) => new()
        {
            MeetingState = "in_meeting",
            RosterEpoch = epoch,
            RosterRevision = revision,
            Participants = [new RawParticipantEvent { UserId = "42", DisplayName = name, Muted = muted, VideoOn = true }]
        };

        var muted = ZoomCaptureSnapshotMerger.Merge(null, Capture(barrier.RosterEpoch, barrier.RosterRevision, "Guest", true));
        var unmuted = ZoomCaptureSnapshotMerger.Merge(muted, Capture("1:1:engine-a", 2, "Guest", false));
        Assert.False(Assert.Single(LiveProductionSync.MapSnapshotParticipants(unmuted)!).IsMuted);
        Assert.Same(unmuted, ZoomCaptureSnapshotMerger.Merge(unmuted, Capture("1:1:engine-a", 1, "Guest", true)));

        var rejoined = ZoomCaptureSnapshotMerger.Merge(unmuted, Capture("1:2:engine-a", 1, "New guest", false));
        Assert.Equal("New guest", Assert.Single(LiveProductionSync.MapSnapshotParticipants(rejoined)!).Name);
        Assert.Same(rejoined, ZoomCaptureSnapshotMerger.Merge(rejoined, Capture("1:1:engine-a", 3, "Old guest", true)));
        Assert.Equal(1, rejoined.RosterRevision);

        var left = ZoomCaptureSnapshotMerger.Merge(rejoined, new RawCaptureSnapshot
        {
            MeetingState = "idle", RosterEpoch = "1:2:engine-a", RosterRevision = 2,
            Participants = []
        });
        Assert.Empty(LiveProductionSync.MapSnapshotParticipants(left)!);
        Assert.Same(left, ZoomCaptureSnapshotMerger.Merge(left, Capture("1:2:engine-a", 1, "New guest", false)));

        var newerProgram = SyntheticMediaCore.SynthesizeSnapshot([], 5000, 8) with
        {
            ProgramFrameCount = 8,
            MeetingState = "in_meeting",
            RosterEpoch = "1:2:engine-a",
            RosterRevision = 1,
            Participants = Capture("1:2:engine-a", 1, "Stale guest", true).Participants
        };
        var reconciled = ZoomMediaSpineSnapshotMerger.CarrySubscriptions(left, newerProgram);
        Assert.Equal(8, reconciled.ProgramFrameCount);
        Assert.Equal("idle", reconciled.MeetingState);
        Assert.Empty(reconciled.Participants);
    }

    [Fact]
    public void MergeInMeetingCaptureSnapshotUpdatesRosterAndMeetingState()
    {
        var existing = SyntheticMediaCore.SynthesizeSnapshot([], 1000, 3);
        var capture = new RawCaptureSnapshot
        {
            MeetingState = "in_meeting",
            Tick = 4,
            ActiveSpeakerId = "operator-1",
            Participants =
            [
                new RawParticipantEvent
                {
                    UserId = "operator-1",
                    DisplayName = "Operator",
                    Role = "Host",
                    VideoOn = true,
                    Talking = true
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

        var merged = ZoomCaptureSnapshotMerger.Merge(existing, capture);

        Assert.Equal("in_meeting", merged.MeetingState);
        Assert.Equal("operator-1", merged.ActiveSpeakerId);
        Assert.Equal(2, merged.Participants.Count);
        Assert.Equal("operator-1", merged.Participants[0].UserId);
        Assert.Equal("subscribed", merged.SourceSnapshot.Status);
        Assert.Equal("zoom-sdk", merged.SourceSnapshot.Kind);
    }

    [Fact]
    public void MergeNormalizesHyphenatedMeetingState()
    {
        var capture = new RawCaptureSnapshot
        {
            MeetingState = "in-meeting",
            ActiveSpeakerId = "42",
            Participants =
            [
                new RawParticipantEvent
                {
                    UserId = "42",
                    DisplayName = "Host",
                    Role = "Host"
                }
            ]
        };

        var merged = ZoomCaptureSnapshotMerger.Merge(null, capture);

        Assert.Equal("in_meeting", merged.MeetingState);
        Assert.Single(merged.Participants);
    }

    [Fact]
    public void MergeIdleCaptureSnapshotClearsRosterAndMarksSourceIdle()
    {
        var existing = ZoomCaptureSnapshotMerger.Merge(
            SyntheticMediaCore.SynthesizeSnapshot([], 0, 0),
            new RawCaptureSnapshot
            {
                MeetingState = "in_meeting",
                ActiveSpeakerId = "operator-1",
                Participants =
                [
                    new RawParticipantEvent
                    {
                        UserId = "operator-1",
                        DisplayName = "Operator",
                        Role = "Host"
                    }
                ]
            });

        var merged = ZoomCaptureSnapshotMerger.Merge(
            existing,
            new RawCaptureSnapshot
            {
                MeetingState = "idle",
                Participants = [],
                Tick = 2
            });

        Assert.Equal("idle", merged.MeetingState);
        Assert.Null(merged.ActiveSpeakerId);
        Assert.Empty(merged.Participants);
        Assert.Equal("idle", merged.SourceSnapshot.Status);
    }

    [Fact]
    public void MergePreservesExistingCompositorStateWhileUpdatingZoomFields()
    {
        var existing = SyntheticMediaCore.SynthesizeSnapshot(
        [
            new NativeMediaCoreCommand
            {
                Type = "start-program-output",
                ExtensionData = new Dictionary<string, System.Text.Json.JsonElement>
                {
                    ["destinations"] = System.Text.Json.JsonSerializer.SerializeToElement(new[] { "rtmp" })
                }
            }
        ],
        2000,
        5);

        var merged = ZoomCaptureSnapshotMerger.Merge(
            existing,
            new RawCaptureSnapshot
            {
                MeetingState = "in_meeting",
                ActiveSpeakerId = "operator-1",
                Participants =
                [
                    new RawParticipantEvent
                    {
                        UserId = "operator-1",
                        DisplayName = "Operator",
                        Role = "Host"
                    }
                ]
            });

        Assert.Equal("in_meeting", merged.MeetingState);
        Assert.Contains("rtmp", merged.Outputs);
        Assert.True(merged.ProgramFrameCount > 0);
    }

    [Fact]
    public void MergeCaptureSnapshotParticipantsMapToLiveProductionRoster()
    {
        var capture = new RawCaptureSnapshot
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
                    Title = "Executive Producer",
                    Talking = true,
                    VideoOn = true
                }
            ]
        };

        var merged = ZoomCaptureSnapshotMerger.Merge(null, capture);
        var participants = LiveProductionSync.MapSnapshotParticipants(merged);

        Assert.NotNull(participants);
        Assert.Single(participants!);
        Assert.Equal("operator-1", participants![0].Id);
        Assert.Equal("Operator", participants[0].Name);
        Assert.Equal("Executive Producer", participants[0].Title);
        Assert.True(participants[0].IsActiveSpeaker);
    }
}
