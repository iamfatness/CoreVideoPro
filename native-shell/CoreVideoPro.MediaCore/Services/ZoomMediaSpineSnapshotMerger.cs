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
        if (!ZoomRosterSnapshotPolicy.Accept(existing, spine.RosterEpoch, spine.RosterRevision))
            return existing!;
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
            Diagnostics = merged.Diagnostics with { SourceSnapshot = sourceSnapshot },
            ZoomSubscriptions = spine.Subscriptions
        };
    }

    /// <summary>
    /// A media-core sync publishes a snapshot parsed from the core's sessionState, which
    /// carries no per-subscription evidence; only the spine sync does. Replacing the last
    /// snapshot wholesale wiped <see cref="NativeMediaCoreStateSnapshot.ZoomSubscriptions"/>
    /// every 500 ms, so the Sources page read "not-requested" for guests the core was
    /// streaming. Keep the spine's evidence when the incoming snapshot has none and the
    /// meeting is still on; fresh evidence always wins; leaving the meeting clears it.
    /// </summary>
    public static NativeMediaCoreStateSnapshot CarrySubscriptions(
        NativeMediaCoreStateSnapshot? existing,
        NativeMediaCoreStateSnapshot incoming)
    {
        if (!ZoomRosterSnapshotPolicy.Accept(existing, incoming.RosterEpoch, incoming.RosterRevision))
        {
            // Keep current Program/output state from the core sync while
            // preserving the newer roster barrier already installed by a
            // concurrent capture/spine response.
            return incoming with
            {
                MeetingState = existing!.MeetingState,
                ActiveSpeakerId = existing.ActiveSpeakerId,
                Participants = existing.Participants,
                RosterEpoch = existing.RosterEpoch,
                RosterRevision = existing.RosterRevision,
                ZoomSubscriptions = existing.ZoomSubscriptions
            };
        }
        if (incoming.ZoomSubscriptions.Count > 0 || existing is null || existing.ZoomSubscriptions.Count == 0)
        {
            return incoming;
        }
        var meetingState = NormalizeMeetingState(incoming.MeetingState ?? existing.MeetingState);
        if (!meetingState.Equals("in_meeting", StringComparison.Ordinal))
        {
            return incoming;
        }
        return incoming with { ZoomSubscriptions = existing.ZoomSubscriptions };
    }

    public static RawCaptureSnapshot ToCaptureSnapshot(
        ZoomMediaSpineNativeSnapshot spine,
        string? meetingStateOverride = null)
    {
        var meetingState = meetingStateOverride ?? NormalizeMeetingState(spine.MeetingState);
        return new RawCaptureSnapshot
        {
            MeetingState = meetingState,
            RosterEpoch = spine.RosterEpoch,
            RosterRevision = spine.RosterRevision,
            ActiveSpeakerId = spine.ActiveSpeakerId,
            Participants = spine.Participants
                .Select(participant => new RawParticipantEvent
                {
                    UserId = participant.SdkUserId,
                    DisplayName = participant.DisplayName,
                    PersistentId = participant.PersistentId,
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
