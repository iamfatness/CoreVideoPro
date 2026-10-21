using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class ZoomMediaSpineSnapshotMergerTests
{
    [Fact]
    public void EngineCaptureFactsSurviveOrdinaryPollsAndFollowStartStopWithoutRosterChanges()
    {
        NativeMediaCoreStateSnapshot? state = null;
        foreach (var active in new[] { false, true, false, true })
        {
            using var response = System.Text.Json.JsonDocument.Parse(
                "{\"ok\":true,\"type\":\"zoom-media-spine-sync\",\"spineSnapshot\":{" +
                "\"meetingState\":\"in-meeting\",\"rosterEpoch\":\"1:1:test\",\"rosterRevision\":2," +
                "\"rawMediaActive\":" + (active ? "true" : "false") + "}}");
            var fact = CoreProtocolParser.TryParseZoomMediaSpineSnapshot(response)!;
            state = ZoomMediaSpineSnapshotMerger.Merge(state, fact);
            Assert.Equal(active, state.RawMediaActive);
            var poll = new NativeMediaCoreStateSnapshot
            {
                MeetingState = "in_meeting", RosterEpoch = "1:1:test", RosterRevision = 2
            };
            state = ZoomMediaSpineSnapshotMerger.CarrySubscriptions(state, poll);
            Assert.Equal(active, state.RawMediaActive);
            Assert.Equal(!active, IsoCapturePreflight.Describe(true, 7, state.RawMediaActive) is not null);
        }
        var rejoined = new NativeMediaCoreStateSnapshot
        {
            MeetingState = "in_meeting", RosterEpoch = "1:2:test", RosterRevision = 1
        };
        Assert.Null(ZoomMediaSpineSnapshotMerger.CarrySubscriptions(state, rejoined).RawMediaActive);
        Assert.Null(ZoomMediaSpineSnapshotMerger.CarrySubscriptions(state,
            new NativeMediaCoreStateSnapshot { MeetingState = "idle", RosterEpoch = "1:1:test", RosterRevision = 3 }).RawMediaActive);
        var confirmedOff = state! with { RawMediaActive = false, RosterRevision = 3 };
        var staleOn = state with { RawMediaActive = true, RosterRevision = 2 };
        Assert.False(ZoomMediaSpineSnapshotMerger.CarrySubscriptions(confirmedOff, staleOn).RawMediaActive);
        Assert.Same(confirmedOff, ZoomMediaSpineSnapshotMerger.Merge(confirmedOff,
            new ZoomMediaSpineNativeSnapshot
            {
                MeetingState = "in-meeting", RosterEpoch = "1:1:test", RosterRevision = 2, RawMediaActive = true
            }));
        Assert.Null(ZoomMediaSpineSnapshotMerger.Merge(null,
            new ZoomMediaSpineNativeSnapshot { MeetingState = "in-meeting" }).RawMediaActive);
    }

    [Fact]
    public void MergeNormalizesMeetingStateAndParticipants()
    {
        var spine = new ZoomMediaSpineNativeSnapshot
        {
            MeetingState = "in-meeting",
            ActiveSpeakerId = "operator-1",
            Participants =
            [
                new ZoomMediaSpineParticipant
                {
                    SdkUserId = "operator-1",
                    DisplayName = "Operator",
                    PersistentId = "operator-pid",
                    Talking = true,
                    VideoOn = true
                }
            ],
            Subscriptions =
            [
                new ZoomMediaSpineSubscription
                {
                    ParticipantId = "operator-1",
                    Kind = "participant-video",
                    Status = "subscribed",
                    LastResultCode = "ok",
                    DeliveredWidth = 1280,
                    DeliveredHeight = 720,
                    DeliveredFps = 30,
                    FramesReceived = 3,
                    FrameFresh = true
                }
            ]
        };

        var merged = ZoomMediaSpineSnapshotMerger.Merge(null, spine);

        Assert.Equal("in_meeting", merged.MeetingState);
        Assert.Single(merged.Participants);
        Assert.Equal("operator-1", merged.Participants[0].UserId);
        Assert.Equal("operator-pid", merged.Participants[0].PersistentId);
        Assert.Equal("operator-1", merged.ActiveSpeakerId);
        Assert.True(merged.SourceSnapshot.SubscribedSourceCount >= 1);
        var subscription = Assert.Single(merged.ZoomSubscriptions);
        Assert.Equal("operator-1", subscription.ParticipantId);
        Assert.Equal(1280, subscription.DeliveredWidth);
        Assert.True(subscription.FrameFresh);
    }

    [Fact]
    public void NormalizeMeetingStateMapsHyphenatedValues()
    {
        Assert.Equal("in_meeting", ZoomMediaSpineSnapshotMerger.NormalizeMeetingState("in-meeting"));
        Assert.Equal("idle", ZoomMediaSpineSnapshotMerger.NormalizeMeetingState("leaving"));
    }

    // Sources page rows read `LastSnapshot.ZoomSubscriptions`. The 2 Hz media-core sync
    // publishes a snapshot parsed from the core's sessionState, which carries NO
    // subscription evidence, and it REPLACED the last snapshot wholesale — so the
    // list the spine sync had merged in was wiped until the next spine tick and every
    // guest row read "Video not-requested · No frames" while the core was streaming
    // all of them (owner screenshot 2026-09-19). A sync that carries no subscription
    // evidence must keep the evidence the spine already delivered.
    [Fact]
    public void CarrySubscriptionsKeepsSpineEvidenceAcrossASyncThatCarriesNone()
    {
        var withEvidence = new NativeMediaCoreStateSnapshot
        {
            MeetingState = "in_meeting",
            ZoomSubscriptions =
            [
                new ZoomMediaSpineSubscription
                {
                    ParticipantId = "16778240", Kind = "participant-video", Status = "subscribed",
                    DeliveredWidth = 1280, DeliveredHeight = 720, DeliveredFps = 30, FramesReceived = 77
                }
            ]
        };
        var syncWithoutEvidence = new NativeMediaCoreStateSnapshot { MeetingState = "in_meeting" };

        var carried = ZoomMediaSpineSnapshotMerger.CarrySubscriptions(withEvidence, syncWithoutEvidence);

        Assert.Single(carried.ZoomSubscriptions);
        Assert.Equal(77, carried.ZoomSubscriptions[0].FramesReceived);
        Assert.Same(syncWithoutEvidence.Participants, carried.Participants); // everything else is the new sync
    }

    [Fact]
    public void CarrySubscriptionsPrefersFreshEvidenceAndClearsWhenTheMeetingEnds()
    {
        var older = new NativeMediaCoreStateSnapshot
        {
            MeetingState = "in_meeting",
            ZoomSubscriptions = [new ZoomMediaSpineSubscription { ParticipantId = "1", Kind = "participant-video", FramesReceived = 5 }]
        };
        var fresh = new NativeMediaCoreStateSnapshot
        {
            MeetingState = "in_meeting",
            ZoomSubscriptions = [new ZoomMediaSpineSubscription { ParticipantId = "1", Kind = "participant-video", FramesReceived = 9 }]
        };
        Assert.Equal(9, ZoomMediaSpineSnapshotMerger.CarrySubscriptions(older, fresh).ZoomSubscriptions[0].FramesReceived);

        var left = new NativeMediaCoreStateSnapshot { MeetingState = "idle" };
        Assert.Empty(ZoomMediaSpineSnapshotMerger.CarrySubscriptions(older, left).ZoomSubscriptions);

        Assert.Empty(ZoomMediaSpineSnapshotMerger.CarrySubscriptions(null, left).ZoomSubscriptions);
    }
}
