using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class ZoomMediaSpineSnapshotMergerTests
{
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
                    Talking = true,
                    VideoOn = true
                }
            ],
            Subscriptions =
            [
                new ZoomMediaSpineSubscription
                {
                    Kind = "participant-video",
                    Status = "subscribed",
                    FramesReceived = 3
                }
            ]
        };

        var merged = ZoomMediaSpineSnapshotMerger.Merge(null, spine);

        Assert.Equal("in_meeting", merged.MeetingState);
        Assert.Single(merged.Participants);
        Assert.Equal("operator-1", merged.Participants[0].UserId);
        Assert.Equal("operator-1", merged.ActiveSpeakerId);
        Assert.True(merged.SourceSnapshot.SubscribedSourceCount >= 1);
    }

    [Fact]
    public void NormalizeMeetingStateMapsHyphenatedValues()
    {
        Assert.Equal("in_meeting", ZoomMediaSpineSnapshotMerger.NormalizeMeetingState("in-meeting"));
        Assert.Equal("idle", ZoomMediaSpineSnapshotMerger.NormalizeMeetingState("leaving"));
    }
}