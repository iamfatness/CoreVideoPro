using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Merges Zoom capture snapshots (join/leave/snapshot RPC responses) into the
/// immutable media-core state read model held by <see cref="MediaCoreBridgeService"/>.
/// </summary>
public static class ZoomCaptureSnapshotMerger
{
    public static NativeMediaCoreStateSnapshot Merge(
        NativeMediaCoreStateSnapshot? existing,
        RawCaptureSnapshot capture)
    {
        if (!ZoomRosterSnapshotPolicy.Accept(existing, capture.RosterEpoch, capture.RosterRevision))
            return existing!;
        var baseSnapshot = existing ?? new NativeMediaCoreStateSnapshot();
        var meetingState = ZoomMediaSpineSnapshotMerger.NormalizeMeetingState(capture.MeetingState);
        var inMeeting = meetingState.Equals("in_meeting", StringComparison.Ordinal);
        // #766: a roster is not video evidence. Subscription and frame counts come only from
        // the spine's per-subscription counters (ZoomMediaSpineSnapshotMerger); this merger
        // used to write the PARTICIPANT count into both, so one participant read as one
        // subscribed feed with one live frame before any raw media had started. In a meeting
        // the counts the spine already proved are kept; leaving clears them. Status says
        // "subscribed" only once a feed really is.
        var subscribedSourceCount = inMeeting ? baseSnapshot.SourceSnapshot.SubscribedSourceCount : 0;
        var liveFrameCount = inMeeting ? baseSnapshot.SourceSnapshot.LiveFrameCount : 0;
        var sourceStatus = !inMeeting ? "idle" : subscribedSourceCount > 0 ? "subscribed" : "joined";

        var sourceSnapshot = new NativeMediaCoreFrameSourceSnapshot
        {
            AdapterId = baseSnapshot.SourceSnapshot.AdapterId,
            Kind = "zoom-sdk",
            Status = sourceStatus,
            SubscribedSourceCount = subscribedSourceCount,
            LiveFrameCount = liveFrameCount,
            StaleFrameCount = baseSnapshot.SourceSnapshot.StaleFrameCount,
            DroppedFrameCount = baseSnapshot.SourceSnapshot.DroppedFrameCount,
            LowResolutionFrameCount = baseSnapshot.SourceSnapshot.LowResolutionFrameCount,
            LastFrameTimestampMs = inMeeting && liveFrameCount > 0
                ? baseSnapshot.SourceSnapshot.LastFrameTimestampMs
                : null,
            Warnings = baseSnapshot.SourceSnapshot.Warnings
        };

        var captionTrack = MergeCaptionTrack(baseSnapshot.CaptionTrack, capture.Caption);
        var diagnostics = baseSnapshot.Diagnostics with
        {
            SourceSnapshot = sourceSnapshot,
            CaptionTrack = captionTrack
        };

        return baseSnapshot with
        {
            MeetingState = meetingState,
            RawMediaActive = capture.RawMediaActive,
            RosterEpoch = capture.RosterEpoch,
            RosterRevision = capture.RosterRevision,
            ActiveSpeakerId = inMeeting ? capture.ActiveSpeakerId : null,
            Participants = inMeeting ? capture.Participants : [],
            SourceSnapshot = sourceSnapshot,
            CaptionTrack = captionTrack,
            Diagnostics = diagnostics
        };
    }

    private static NativeMediaCoreCaptionTrack MergeCaptionTrack(
        NativeMediaCoreCaptionTrack captionTrack,
        string? caption)
    {
        if (string.IsNullOrWhiteSpace(caption))
        {
            return captionTrack;
        }

        return new NativeMediaCoreCaptionTrack
        {
            Enabled = true,
            Status = "live",
            CurrentCue = new NativeMediaCoreCaptionCue
            {
                Text = caption.Trim(),
                AtMs = 0,
                Confidence = 95
            },
            LatencyMs = captionTrack.LatencyMs,
            Warnings = captionTrack.Warnings
        };
    }
}
