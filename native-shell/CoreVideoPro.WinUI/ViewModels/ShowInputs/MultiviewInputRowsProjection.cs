using System.Collections.ObjectModel;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels.ShowInputs;

/// <summary>
/// Projects versioned Zoom roster facts onto ten stable operator rows. The
/// bridge callback calls this directly; scene synchronization is not involved.
/// </summary>
public sealed class MultiviewInputRowsProjection
{
    private readonly IReadOnlyList<ShowInputSlot> _slots;
    private NativeMediaCoreStateSnapshot? _lastRoster;
    private string? _retiredEpoch;
    private readonly Dictionary<string, IsoOutputLifecycleFact> _isoFacts = new(StringComparer.Ordinal);

    public ObservableCollection<MultiviewInputRow> Rows { get; } = new(
        Enumerable.Range(1, ShowInputRosterService.MaxMultiviewBoxes)
            .Select(number => new MultiviewInputRow(number)));

    public MultiviewInputRowsProjection(IReadOnlyList<ShowInputSlot> slots)
    {
        _slots = slots;
        foreach (var slot in slots)
            slot.PropertyChanged += (_, _) => PatchSlot(slot.SlotNumber);
        for (var number = 1; number <= Rows.Count; number++) PatchSlot(number);
    }

    public void ApplyRosterFact(NativeMediaCoreStateSnapshot snapshot)
    {
        if (snapshot.RosterRevision <= 0 || string.IsNullOrWhiteSpace(snapshot.RosterEpoch) ||
            !ZoomRosterSnapshotPolicy.Accept(_lastRoster, snapshot.RosterEpoch, snapshot.RosterRevision))
            return;
        if (_retiredEpoch == snapshot.RosterEpoch) return;
        var sameEpoch = _lastRoster?.RosterEpoch == snapshot.RosterEpoch;
        if (sameEpoch && snapshot.RosterRevision < _lastRoster!.RosterRevision) return;
        if (sameEpoch && snapshot.RosterRevision == _lastRoster!.RosterRevision &&
            snapshot.ZoomSubscriptions.Count == 0) return;
        if (!sameEpoch)
            foreach (var row in Rows) ResetFrame(row);
        _lastRoster = snapshot;
        foreach (var row in Rows) PatchSlot(row.SlotNumber);
    }

    public void EngineStopped()
    {
        _retiredEpoch = _lastRoster?.RosterEpoch;
        _lastRoster = null;
        _isoFacts.Clear();
        foreach (var row in Rows)
        {
            ResetFrame(row);
            row.StatusLabel = "IDLE";
            row.RecLabel = "OFF";
            row.RosterEpoch = null;
            row.HighestObservationRevision = 0;
        }
    }

    public void ApplyOutputLifecycleFact(IsoOutputLifecycleFact fact)
    {
        if (_isoFacts.TryGetValue(fact.SourceId, out var current) && fact.Revision <= current.Revision)
            return;
        _isoFacts[fact.SourceId] = fact;
        foreach (var row in Rows.Where(item => item.SourceId == fact.SourceId))
            row.RecLabel = fact.State.ToUpperInvariant();
    }

    private static void ResetFrame(MultiviewInputRow row)
    {
        row.FormatLabel = "—";
        row.ConfiguredCapLabel = "";
        row.CapDiffers = false;
        row.FormatStale = false;
        row.FrameAgeMs = -1;
        row.HighestFrameId = 0;
        row.LastFrameAtMs = -1;
    }

    private void PatchSlot(int slotNumber)
    {
        if (slotNumber < 1 || slotNumber > Rows.Count) return;
        var row = Rows[slotNumber - 1];
        var slot = _slots.FirstOrDefault(item => item.SlotNumber == slotNumber);
        var sourceId = slot is { IsAssigned: true, Kind: ShowInputKind.ZoomParticipant,
            ParticipantId: { Length: > 0 } participantId } ? $"zoom:{participantId}" : null;
        if (!string.Equals(row.SourceId, sourceId, StringComparison.Ordinal))
        {
            row.SourceId = sourceId;
            row.LastAppliedSourceInstance = null;
            ResetFrame(row);
            row.RecLabel = sourceId is not null && _isoFacts.TryGetValue(sourceId, out var iso)
                ? iso.State.ToUpperInvariant() : "OFF";
        }
        row.RosterEpoch = _lastRoster?.RosterEpoch;
        row.HighestObservationRevision = _lastRoster?.RosterRevision ?? 0;
        if (_lastRoster is null || sourceId is null || slot?.InShow != true)
        {
            row.StatusLabel = "IDLE";
            ResetFrame(row);
            return;
        }
        if (_lastRoster?.MeetingState != "in_meeting")
        {
            row.StatusLabel = "NO INCOMING";
            ResetFrame(row);
            return;
        }
        var participant = _lastRoster.Participants.FirstOrDefault(item =>
            string.Equals(item.UserId, slot.ParticipantId, StringComparison.Ordinal));
        if (participant is null)
        {
            row.StatusLabel = "NO INCOMING";
            ResetFrame(row);
            return;
        }
        if (participant.VideoOn == false)
        {
            row.StatusLabel = "VIDEO OFF";
            ResetFrame(row);
            return;
        }
        var video = _lastRoster.ZoomSubscriptions.FirstOrDefault(item =>
            item.Kind == "participant-video" && item.ParticipantId == slot.ParticipantId &&
            item.DeliveredWidth > 0 && item.DeliveredHeight > 0 && item.DeliveredFps > 0 &&
            item.LastFrameAtMs >= 0 && item.LastFrameAgeMs >= 0);
        if (video is null)
        {
            row.StatusLabel = "CONNECTING";
            ResetFrame(row);
            return;
        }
        // The subscription's frame clock is monotonic within this meeting.
        // An older response cannot resurrect a fresh frame or lower its age.
        if (video.LastFrameAtMs >= 0 && row.LastFrameAtMs > video.LastFrameAtMs ||
            video.LastFrameAtMs == row.LastFrameAtMs &&
            video.LastFrameAgeMs >= 0 && row.FrameAgeMs > video.LastFrameAgeMs)
            return;
        row.HighestFrameId = video.LastFrameId;
        row.LastFrameAtMs = video.LastFrameAtMs;
        row.FrameAgeMs = video.LastFrameAgeMs;
        row.FormatStale = video.LastFrameAgeMs > 1500;
        row.StatusLabel = row.FormatStale ? "STALLED" : "LIVE";
        row.FormatLabel = $"{video.DeliveredWidth}×{video.DeliveredHeight}@{video.DeliveredFps}";
        row.CapDiffers = video.DeliveredWidth != 1920 || video.DeliveredHeight != 1080 || video.DeliveredFps != 60;
        row.ConfiguredCapLabel = row.CapDiffers ? "cap 1080p60" : "";
    }
}
