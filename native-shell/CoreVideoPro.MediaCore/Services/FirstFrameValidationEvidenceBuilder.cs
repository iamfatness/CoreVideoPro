using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Derives first-frame validation state from existing media-core snapshots and
/// unsolicited frame events. This intentionally does not join Zoom; tests can
/// prove the validation path with synthetic snapshots and parsed event payloads.
/// </summary>
public static class FirstFrameValidationEvidenceBuilder
{
    public static FirstFrameValidationEvidence From(
        FirstFrameValidationEvidence? existing = null,
        NativeMediaCoreStateSnapshot? snapshot = null,
        ZoomMediaSpineNativeSnapshot? spineSnapshot = null,
        ZoomVideoFrame? zoomVideoFrame = null,
        ProgramFramePreview? programPreviewFrame = null,
        double? zoomFrameObservedElapsedMs = null)
    {
        var evidence = existing ?? new FirstFrameValidationEvidence();

        if (snapshot is not null)
        {
            evidence = MergeSnapshot(evidence, snapshot);
        }

        if (spineSnapshot is not null)
        {
            evidence = MergeSpineSnapshot(evidence, spineSnapshot);
        }

        if (zoomVideoFrame is not null)
        {
            evidence = MergeZoomVideoFrame(evidence, zoomVideoFrame, zoomFrameObservedElapsedMs);
        }

        if (programPreviewFrame is not null)
        {
            evidence = MergeProgramPreviewFrame(evidence, programPreviewFrame);
        }

        return FinalizeEvidence(evidence);
    }

    public static FirstFrameValidationEvidence MergeZoomVideoFrame(
        FirstFrameValidationEvidence existing,
        ZoomVideoFrame frame,
        double? observedElapsedMs = null)
    {
        var observed = frame.Width > 0 &&
                       frame.Height > 0 &&
                       frame.FrameId > 0 &&
                       frame.Bgra.Length == frame.Width * frame.Height * 4;

        if (!observed)
        {
            return existing;
        }

        var firstZoomFrameElapsedMs = existing.FirstZoomFrameElapsedMs;
        if (firstZoomFrameElapsedMs is null && observedElapsedMs is >= 0)
        {
            firstZoomFrameElapsedMs = observedElapsedMs;
        }

        return existing with
        {
            ZoomVideoFrameObserved = true,
            LiveZoomFrameCount = Math.Max(existing.LiveZoomFrameCount, frame.FrameId),
            LastZoomParticipantId = frame.ParticipantId,
            LastZoomFrameId = frame.FrameId,
            LastZoomFrameWidth = frame.Width,
            LastZoomFrameHeight = frame.Height,
            FirstZoomFrameElapsedMs = firstZoomFrameElapsedMs
        };
    }

    public static FirstFrameValidationEvidence MergeProgramPreviewFrame(
        FirstFrameValidationEvidence existing,
        ProgramFramePreview preview)
    {
        var hasBgra = preview.Width > 0 &&
                      preview.Height > 0 &&
                      preview.Bgra.Length == preview.Width * preview.Height * 4;
        var hasSharedTexture = preview.SharedTexture?.IsValid == true;
        if (preview.FrameNumber <= 0 || (!hasBgra && !hasSharedTexture))
        {
            return existing;
        }

        return existing with
        {
            ProgramPreviewFrameObserved = true,
            ProgramFrameCount = Math.Max(existing.ProgramFrameCount, preview.FrameNumber),
            LastProgramFrameNumber = Math.Max(existing.LastProgramFrameNumber ?? 0, preview.FrameNumber)
        };
    }

    public static FirstFrameValidationEvidence MergeSnapshot(
        FirstFrameValidationEvidence existing,
        NativeMediaCoreStateSnapshot snapshot)
    {
        var meetingState = ZoomMediaSpineSnapshotMerger.NormalizeMeetingState(snapshot.MeetingState);
        var participantCount = Math.Max(existing.ParticipantCount, snapshot.Participants.Count);
        var liveFrameCount = Math.Max(existing.LiveZoomFrameCount, snapshot.SourceSnapshot.LiveFrameCount);
        var programFrameCount = Math.Max(existing.ProgramFrameCount, snapshot.ProgramFrameCount);
        var audioPacketCount = Math.Max(existing.AudioPacketCount, snapshot.AudioMixSession.MixedFrameCount);
        var audioActiveParticipantCount = Math.Max(
            existing.AudioActiveParticipantCount,
            snapshot.AudioMixSession.Participants.Count(participant =>
                participant.InputLevel > 0 ||
                participant.OutputLevel > 0 ||
                participant.Status.Equals("balanced", StringComparison.Ordinal) ||
                participant.Status.Equals("live", StringComparison.Ordinal)));

        var preview = snapshot.ProgramFramePreview;
        var previewObserved = existing.ProgramPreviewFrameObserved ||
                              IsNativePreviewPresent(preview) ||
                              snapshot.ProgramTransport.FrameNumber > 0;

        return existing with
        {
            MeetingState = meetingState,
            ParticipantCount = participantCount,
            LiveZoomFrameCount = liveFrameCount,
            ProgramFrameCount = programFrameCount,
            AudioPacketCount = audioPacketCount,
            AudioActiveParticipantCount = audioActiveParticipantCount,
            ZoomVideoFrameObserved = existing.ZoomVideoFrameObserved || liveFrameCount > 0,
            ProgramPreviewFrameObserved = previewObserved,
            ParticipantFreshnessObserved = existing.ParticipantFreshnessObserved ||
                                           participantCount > 0 ||
                                           snapshot.SourceSnapshot.SubscribedSourceCount > 0,
            AudioFreshnessObserved = existing.AudioFreshnessObserved ||
                                     audioPacketCount > 0 ||
                                     audioActiveParticipantCount > 0,
            LastProgramFrameNumber = MaxNullable(existing.LastProgramFrameNumber, preview?.FrameNumber ?? snapshot.ProgramTransport.FrameNumber)
        };
    }

    public static FirstFrameValidationEvidence MergeSpineSnapshot(
        FirstFrameValidationEvidence existing,
        ZoomMediaSpineNativeSnapshot spine)
    {
        var meetingState = ZoomMediaSpineSnapshotMerger.NormalizeMeetingState(spine.MeetingState);
        var participantCount = Math.Max(
            existing.ParticipantCount,
            Math.Max(spine.ParticipantCount, spine.Participants.Count));
        var frameCount = Math.Max(
            existing.LiveZoomFrameCount,
            spine.Subscriptions.Sum(subscription => subscription.FramesReceived));
        var firstSpineFrameMs = spine.Subscriptions
            .Where(subscription =>
                subscription.Kind == "participant-video" &&
                subscription.FramesReceived > 0 &&
                subscription.FirstFrameAtMs >= 0)
            .Select(subscription => subscription.FirstFrameAtMs)
            .DefaultIfEmpty(-1)
            .Min();
        var audioPacketCount = Math.Max(
            existing.AudioPacketCount,
            spine.Subscriptions.Sum(subscription => subscription.AudioPacketsReceived));
        var audioActiveParticipantCount = Math.Max(
            existing.AudioActiveParticipantCount,
            spine.Participants.Count(participant =>
                participant.AudioLevel > 0 ||
                participant.Talking ||
                !participant.Muted));

        return existing with
        {
            MeetingState = meetingState,
            ParticipantCount = participantCount,
            LiveZoomFrameCount = frameCount,
            AudioPacketCount = audioPacketCount,
            AudioActiveParticipantCount = audioActiveParticipantCount,
            ZoomVideoFrameObserved = existing.ZoomVideoFrameObserved || frameCount > 0,
            ParticipantFreshnessObserved = existing.ParticipantFreshnessObserved || participantCount > 0,
            AudioFreshnessObserved = existing.AudioFreshnessObserved ||
                                     audioPacketCount > 0 ||
                                     audioActiveParticipantCount > 0,
            FirstZoomFrameElapsedMs = existing.FirstZoomFrameElapsedMs ??
                                      (firstSpineFrameMs >= 0 ? firstSpineFrameMs : null)
        };
    }

    private static FirstFrameValidationEvidence FinalizeEvidence(FirstFrameValidationEvidence evidence)
    {
        var observed = new List<string>();
        var missing = new List<string>();
        AddResult(
            evidence.ZoomVideoFrameObserved,
            "zoom-video-frame observed",
            "first Zoom video frame",
            observed,
            missing);
        AddResult(
            evidence.ProgramPreviewFrameObserved,
            "program preview frame observed",
            "first program preview frame",
            observed,
            missing);
        AddResult(
            evidence.ParticipantFreshnessObserved,
            "participant roster fresh",
            "fresh participant roster",
            observed,
            missing);
        AddResult(
            evidence.AudioFreshnessObserved,
            "audio freshness observed",
            "fresh participant audio",
            observed,
            missing);

        return evidence with
        {
            Evidence = observed,
            Missing = missing
        };
    }

    private static bool IsNativePreviewPresent(NativeMediaCoreProgramFramePreview? preview)
    {
        if (preview is null || preview.FrameNumber <= 0)
        {
            return false;
        }

        var hasBgra = preview.Width > 0 &&
                      preview.Height > 0 &&
                      preview.Bgra is { Length: > 0 } &&
                      preview.Bgra.Length == preview.Width * preview.Height * 4;
        var hasSharedTexture = preview.SharedTexture is
        {
            SharedHandleHex.Length: > 0,
            Width: > 0,
            Height: > 0
        };

        return hasBgra || hasSharedTexture;
    }

    private static int? MaxNullable(int? left, int? right)
    {
        if (left is null)
        {
            return right;
        }

        if (right is null)
        {
            return left;
        }

        return Math.Max(left.Value, right.Value);
    }

    private static void AddResult(
        bool observed,
        string evidence,
        string missing,
        ICollection<string> observedItems,
        ICollection<string> missingItems)
    {
        if (observed)
        {
            observedItems.Add(evidence);
        }
        else
        {
            missingItems.Add(missing);
        }
    }
}
