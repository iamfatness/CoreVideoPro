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
        var baseSnapshot = existing ?? SyntheticMediaCore.SynthesizeSnapshot([], 0, 0);
        var inMeeting = capture.MeetingState.Equals("in_meeting", StringComparison.Ordinal);
        var participantCount = capture.Participants.Count;
        var sourceStatus = inMeeting ? "subscribed" : "idle";

        var sourceSnapshot = new NativeMediaCoreFrameSourceSnapshot
        {
            AdapterId = baseSnapshot.SourceSnapshot.AdapterId,
            Kind = "zoom-sdk",
            Status = sourceStatus,
            SubscribedSourceCount = inMeeting
                ? Math.Max(participantCount, baseSnapshot.SourceSnapshot.SubscribedSourceCount)
                : 0,
            LiveFrameCount = inMeeting
                ? Math.Max(participantCount, baseSnapshot.SourceSnapshot.LiveFrameCount)
                : 0,
            StaleFrameCount = baseSnapshot.SourceSnapshot.StaleFrameCount,
            DroppedFrameCount = baseSnapshot.SourceSnapshot.DroppedFrameCount,
            LowResolutionFrameCount = baseSnapshot.SourceSnapshot.LowResolutionFrameCount,
            LastFrameTimestampMs = inMeeting ? baseSnapshot.Diagnostics.GeneratedAtMs : null,
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
            MeetingState = capture.MeetingState,
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