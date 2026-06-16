using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Merges <c>zoom-media-spine-sync</c> responses into the media-core read model.
/// </summary>
public static class ZoomMediaSpineSnapshotMerger
{
    public static NativeMediaCoreStateSnapshot Merge(
        NativeMediaCoreStateSnapshot? existing,
        ZoomMediaSpineNativeSnapshot spine)
    {
        var meetingState = NormalizeMeetingState(spine.MeetingState);
        var inMeeting = meetingState.Equals("in_meeting", StringComparison.Ordinal);
        var capture = ToCaptureSnapshot(spine, meetingState);
        var merged = ZoomCaptureSnapshotMerger.Merge(existing, capture);
        var subscribedVideoFeeds = spine.Subscriptions
            .Count(subscription =>
                subscription.Kind == "participant-video" &&
                !subscription.Status.Equals("failed", StringComparison.Ordinal));

        var sourceSnapshot = new NativeMediaCoreFrameSourceSnapshot
        {
            AdapterId = merged.SourceSnapshot.AdapterId,
            Kind = merged.SourceSnapshot.Kind,
            Status = merged.SourceSnapshot.Status,
            SubscribedSourceCount = inMeeting
                ? Math.Max(subscribedVideoFeeds, merged.SourceSnapshot.SubscribedSourceCount)
                : 0,
            LiveFrameCount = inMeeting
                ? Math.Max(
                    spine.Subscriptions.Sum(subscription => subscription.FramesReceived),
                    merged.SourceSnapshot.LiveFrameCount)
                : 0,
            StaleFrameCount = merged.SourceSnapshot.StaleFrameCount,
            DroppedFrameCount = merged.SourceSnapshot.DroppedFrameCount,
            LowResolutionFrameCount = merged.SourceSnapshot.LowResolutionFrameCount,
            LastFrameTimestampMs = merged.SourceSnapshot.LastFrameTimestampMs,
            Warnings = merged.SourceSnapshot.Warnings
        };

        return merged with
        {
            SourceSnapshot = sourceSnapshot,
            Diagnostics = merged.Diagnostics with { SourceSnapshot = sourceSnapshot }
        };
    }

    public static RawCaptureSnapshot ToCaptureSnapshot(
        ZoomMediaSpineNativeSnapshot spine,
        string? meetingStateOverride = null)
    {
        var meetingState = meetingStateOverride ?? NormalizeMeetingState(spine.MeetingState);
        return new RawCaptureSnapshot
        {
            MeetingState = meetingState,
            ActiveSpeakerId = spine.ActiveSpeakerId,
            Participants = spine.Participants
                .Select(participant => new RawParticipantEvent
                {
                    UserId = participant.SdkUserId,
                    DisplayName = participant.DisplayName,
                    Role = participant.Role,
                    Muted = participant.Muted,
                    VideoOn = participant.VideoOn,
                    Talking = participant.Talking,
                    SharingScreen = participant.SharingScreen,
                    AudioLevel = participant.AudioLevel,
                    NetworkQuality = participant.NetworkQuality
                })
                .ToList()
        };
    }

    public static string NormalizeMeetingState(string? meetingState) => meetingState switch
    {
        "in-meeting" or "joining" => "in_meeting",
        "leaving" => "idle",
        null or "" => "idle",
        _ => meetingState
    };
}