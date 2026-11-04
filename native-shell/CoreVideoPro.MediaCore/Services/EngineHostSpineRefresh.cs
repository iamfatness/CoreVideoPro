using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed record EngineHostSpineIntent(ZoomMediaSpinePayloadBuilder.BuildInput Input, string RosterEpoch,
    long RosterRevision, IReadOnlyDictionary<string, long> Sources)
{
    public const string CommandKey = "hostSpineIntent";

    public static EngineHostSpineIntent? From(ZoomMediaSpinePayloadBuilder.BuildInput input, NativeMediaCoreStateSnapshot? snapshot)
    {
        if (!input.StartCapture || snapshot?.MeetingState != "in_meeting" ||
            snapshot.RosterEpoch is not { Length: > 0 } epoch || snapshot.RosterRevision <= 0)
            return null;

        var sources = snapshot.Participants.Where(source => source.SourceGeneration > 0)
            .GroupBy(source => source.UserId, StringComparer.Ordinal)
            .Where(group => group.Count() == 1)
            .ToDictionary(group => group.Key, group => group.Single().SourceGeneration, StringComparer.Ordinal);
        return new(input with { Participants = [] }, epoch, snapshot.RosterRevision, sources);
    }
}

/// <summary>A continuation for a capture that native accepted with matching roster facts.</summary>
public sealed class EngineHostSpineRefresh
{
    private readonly EngineHostSpineIntent _intent;
    private readonly string _captureGeneration;

    private EngineHostSpineRefresh(EngineHostSpineIntent intent, string captureGeneration) =>
        (_intent, _captureGeneration) = (intent, captureGeneration);

    public static EngineHostSpineRefresh? Accept(EngineHostSpineIntent? intent, ZoomMediaSpineNativeSnapshot? accepted)
    {
        if (intent is not { Input.StartCapture: true, Sources: not null } || accepted is null ||
            accepted.MeetingState is not ("in-meeting" or "in_meeting") ||
            accepted.RosterEpoch != intent.RosterEpoch || accepted.RosterRevision != intent.RosterRevision ||
            accepted.CaptureGeneration is not { Length: > 0 } capture)
            return null;
        return new(intent, capture);
    }

    public Dictionary<string, object?>? Build(RawCaptureSnapshot current)
    {
        var input = _intent.Input;
        if (current.MeetingState != "in_meeting" ||
            current.RosterEpoch is not { Length: > 0 } epoch || epoch != _intent.RosterEpoch ||
            current.CaptureGeneration is not { Length: > 0 } capture || capture != _captureGeneration ||
            current.RosterRevision < _intent.RosterRevision)
            return null;

        var roster = current.Participants.Where(source =>
            _intent.Sources.TryGetValue(source.UserId, out var generation) && generation == source.SourceGeneration).ToArray();
        var sourceIds = roster.Select(source => source.UserId).ToHashSet(StringComparer.Ordinal);
        var participants = ZoomMediaSpinePayloadBuilder.ProjectNativeRoster(new()
        {
            MeetingState = current.MeetingState,
            ActiveSpeakerId = current.ActiveSpeakerId,
            Participants = roster
        }, input.SelectedBreakoutRoomId, "");
        var payload = ZoomMediaSpinePayloadBuilder.Build(input with
        {
            Participants = participants,
            StickyAudioParticipantIds = input.StickyAudioParticipantIds.Where(sourceIds.Contains).ToArray()
        });
        payload.Remove("startCapture");
        payload.Remove("multiview");
        payload.Remove("previewScene");
        payload.Remove("recording");
        payload["captureRefresh"] = new { rosterEpoch = epoch, rosterRevision = current.RosterRevision, captureGeneration = capture };
        return payload;
    }
}
