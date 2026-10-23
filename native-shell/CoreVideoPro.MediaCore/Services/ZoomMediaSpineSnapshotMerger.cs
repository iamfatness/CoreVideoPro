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

        // #766: the spine's per-subscription counters are the only source of these numbers.
        var subscribedSourceCount = inMeeting
            ? Math.Max(subscribedVideoFeeds, merged.SourceSnapshot.SubscribedSourceCount)
            : 0;
        var liveFrameCount = inMeeting
            ? Math.Max(
                spine.Subscriptions.Sum(subscription => subscription.FramesReceived),
                merged.SourceSnapshot.LiveFrameCount)
            : 0;
        var sourceSnapshot = new NativeMediaCoreFrameSourceSnapshot
        {
            AdapterId = merged.SourceSnapshot.AdapterId,
            Kind = merged.SourceSnapshot.Kind,
            Status = !inMeeting ? "idle" : subscribedSourceCount > 0 ? "subscribed" : "joined",
            SubscribedSourceCount = subscribedSourceCount,
            LiveFrameCount = liveFrameCount,
            StaleFrameCount = merged.SourceSnapshot.StaleFrameCount,
            DroppedFrameCount = merged.SourceSnapshot.DroppedFrameCount,
            LowResolutionFrameCount = merged.SourceSnapshot.LowResolutionFrameCount,
            LastFrameTimestampMs = liveFrameCount > merged.SourceSnapshot.LiveFrameCount
                ? merged.Diagnostics.GeneratedAtMs
                : merged.SourceSnapshot.LastFrameTimestampMs,
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
                RawMediaActive = existing.RawMediaActive,
                ZoomSubscriptions = existing.ZoomSubscriptions
            };
        }
        // Ordinary session-state polls omit this engine fact. Retain it only in
        // the same meeting authority; a reconnect must not inherit old capture.
        if (incoming.RawMediaActive is null && existing is not null &&
            incoming.RosterEpoch == existing.RosterEpoch &&
            NormalizeMeetingState(incoming.MeetingState) == "in_meeting")
            incoming = incoming with { RawMediaActive = existing.RawMediaActive };
        if (incoming.ZoomSubscriptions.Count > 0 || existing is null || existing.ZoomSubscriptions.Count == 0)
        {
            return incoming;
        }
        var meetingState = NormalizeMeetingState(incoming.MeetingState ?? existing.MeetingState);
        if (!meetingState.Equals("in_meeting", StringComparison.Ordinal))
        {
            return incoming;
        }
        // #766: the Zoom source row (Settings "Feeds") is derived from the same spine
        // evidence, and the mapper's sync snapshot starts from the neutral idle row, so the
        // row flickered idle/subscribed on every poll. Carry it with the subscriptions.
        return incoming with
        {
            ZoomSubscriptions = existing.ZoomSubscriptions,
            SourceSnapshot = existing.SourceSnapshot,
            Diagnostics = incoming.Diagnostics with { SourceSnapshot = existing.SourceSnapshot }
        };
    }

    public static RawCaptureSnapshot ToCaptureSnapshot(
        ZoomMediaSpineNativeSnapshot spine,
        string? meetingStateOverride = null)
    {
        var meetingState = meetingStateOverride ?? NormalizeMeetingState(spine.MeetingState);
        return new RawCaptureSnapshot
        {
            RawMediaActive = spine.RawMediaActive,
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
                    SourceGeneration = participant.SourceGeneration,
                    Role = participant.Role,
                    Muted = participant.Muted,
                    VideoOn = participant.VideoOn,
                    Talking = participant.Talking,
                    SharingScreen = participant.SharingScreen,
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
