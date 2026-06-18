using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class ZoomCaptureSnapshotMergerTests
{
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