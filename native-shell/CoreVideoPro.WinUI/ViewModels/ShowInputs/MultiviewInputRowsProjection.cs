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
        if (_lastRoster?.RosterEpoch == snapshot.RosterEpoch &&
            snapshot.RosterRevision <= _lastRoster.RosterRevision) return;
        _lastRoster = snapshot;
        foreach (var row in Rows) PatchSlot(row.SlotNumber);
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
            row.FormatLabel = "—";
        }
        row.RosterEpoch = _lastRoster?.RosterEpoch;
        row.HighestObservationRevision = _lastRoster?.RosterRevision ?? 0;
        if (sourceId is null || slot?.InShow != true)
        {
            row.StatusLabel = "IDLE";
            row.FormatLabel = "—";
            return;
        }
        if (_lastRoster?.MeetingState != "in_meeting")
        {
            row.StatusLabel = "NO INCOMING";
            row.FormatLabel = "—";
            return;
        }
        var participant = _lastRoster.Participants.FirstOrDefault(item =>
            string.Equals(item.UserId, slot.ParticipantId, StringComparison.Ordinal));
        if (participant is null)
        {
            row.StatusLabel = "NO INCOMING";
            row.FormatLabel = "—";
            return;
        }
        if (participant.VideoOn == false)
        {
            row.StatusLabel = "VIDEO OFF";
            row.FormatLabel = "—";
            return;
        }
        var video = _lastRoster.ZoomSubscriptions.FirstOrDefault(item =>
            item.Kind == "participant-video" && item.ParticipantId == slot.ParticipantId &&
            item.DeliveredWidth > 0 && item.DeliveredHeight > 0 && item.DeliveredFps > 0);
        row.StatusLabel = video is null ? "CONNECTING" : "LIVE";
        row.FormatLabel = video is null ? "—" :
            $"{video.DeliveredWidth}×{video.DeliveredHeight}@{video.DeliveredFps}";
    }
}
