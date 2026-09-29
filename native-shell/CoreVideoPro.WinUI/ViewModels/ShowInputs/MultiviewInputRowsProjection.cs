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
    private readonly ObservableCollection<CaptureDevice>? _captureDevices;
    private readonly HashSet<CaptureDevice> _observedCaptureDevices = [];
    private NativeMediaCoreStateSnapshot? _lastRoster;
    private bool _engineStopped;
    private string? _retiredEpoch;
    private readonly Dictionary<string, IsoOutputLifecycleFact> _isoFacts = new(StringComparer.Ordinal);
    private IReadOnlyList<MultiviewTile> _tiles = [];
    private string _cameraCap = ZoomCameraResolutionPreference.Default;
    private int _cameraFpsCap = ZoomCameraFrameRatePreference.Default;

    public ObservableCollection<MultiviewInputRow> Rows { get; } = new(
        Enumerable.Range(1, ShowInputRosterService.MaxMultiviewBoxes)
            .Select(number => new MultiviewInputRow(number)));

    public MultiviewInputRowsProjection(IReadOnlyList<ShowInputSlot> slots,
        ObservableCollection<CaptureDevice>? captureDevices = null)
    {
        _slots = slots;
        _captureDevices = captureDevices;
        if (captureDevices is not null)
        {
            captureDevices.CollectionChanged += (_, _) => RefreshCaptureSubscriptions();
            RefreshCaptureSubscriptions();
        }
        foreach (var slot in slots)
            slot.PropertyChanged += (_, _) => PatchSlot(slot.SlotNumber);
        for (var number = 1; number <= Rows.Count; number++) PatchSlot(number);
    }

    private void RefreshCaptureSubscriptions()
    {
        if (_captureDevices is null) return;
        foreach (var removed in _observedCaptureDevices.Except(_captureDevices).ToArray())
        {
            removed.PropertyChanged -= OnCaptureChanged;
            _observedCaptureDevices.Remove(removed);
        }
        foreach (var added in _captureDevices.Except(_observedCaptureDevices))
        {
            added.PropertyChanged += OnCaptureChanged;
            _observedCaptureDevices.Add(added);
        }
        foreach (var row in Rows) PatchSlot(row.SlotNumber);
    }

    private void OnCaptureChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs args)
    {
        if (sender is not CaptureDevice device) return;
        foreach (var row in Rows.Where(item => item.SourceId == $"capture:{device.Id}"))
            PatchSlot(row.SlotNumber);
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
        _engineStopped = false;
        _lastRoster = snapshot;
        foreach (var row in Rows) PatchSlot(row.SlotNumber);
    }

    public void ApplySourceFormatFact(ZoomSourceFormatFact fact)
    {
        if (_engineStopped || _lastRoster is null ||
            _lastRoster.MeetingState != "in_meeting" ||
            _lastRoster.RosterEpoch != fact.RosterEpoch) return;
        var participant = _lastRoster.Participants.FirstOrDefault(item =>
            string.Equals(item.UserId, fact.ParticipantId, StringComparison.Ordinal));
        if (participant is null || participant.VideoOn == false) return;
        if (participant.SourceGeneration > 0 && participant.SourceGeneration != fact.SourceGeneration) return;
        foreach (var row in Rows.Where(item => item.SourceId == $"zoom:{fact.ParticipantId}"))
        {
            if (fact.FrameAtMs <= row.LastFrameAtMs) continue;
            row.LastFrameAtMs = fact.FrameAtMs;
            row.HighestFrameId = fact.FrameId;
            row.FrameAgeMs = 0;
            row.FormatStale = false;
            row.StatusLabel = participant.Talking == true ? "TALKING" : "LIVE";
            row.FormatLabel = $"{fact.Width}×{fact.Height}@{fact.Fps}";
            ApplyCameraCap(row, fact.Width, fact.Height, fact.Fps);
            PatchPreview(row);
        }
    }

    /// <summary>The ceiling accepted at this meeting's join, never a pending preference.</summary>
    public void SetAppliedCameraCap(string cap)
    {
        _cameraCap = ZoomCameraResolutionPreference.Normalize(cap);
        foreach (var row in Rows.Where(item => item.SourceId?.StartsWith("zoom:", StringComparison.Ordinal) == true &&
                                               item.ObservedWidth > 0 && item.ObservedHeight > 0))
            ApplyCameraCap(row, row.ObservedWidth, row.ObservedHeight, row.ObservedFps);
    }

    public void SetAppliedCameraFps(int fps)
    {
        _cameraFpsCap = ZoomCameraFrameRatePreference.Normalize(fps);
        foreach (var row in Rows.Where(item => item.SourceId?.StartsWith("zoom:", StringComparison.Ordinal) == true &&
                                               item.ObservedWidth > 0 && item.ObservedHeight > 0))
            ApplyCameraCap(row, row.ObservedWidth, row.ObservedHeight, row.ObservedFps);
    }

    private void ApplyCameraCap(MultiviewInputRow row, int width, int height, int fps)
    {
        row.ObservedWidth = width;
        row.ObservedHeight = height;
        row.ObservedFps = fps;
        var (capWidth, capHeight) = ZoomCameraResolutionPreference.Dimensions(_cameraCap);
        var resolutionDiffers = width < capWidth || height < capHeight;
        var localFpsLimit = _cameraFpsCap < 60 && fps > _cameraFpsCap;
        row.ConfiguredCapLabel = string.Join(" · ", new[]
        {
            resolutionDiffers ? $"up to {_cameraCap} requested" : "",
            localFpsLimit ? $"local max {_cameraFpsCap} fps" : ""
        }.Where(text => text.Length > 0));
        row.CapDiffers = resolutionDiffers || localFpsLimit;
    }

    public void EngineStopped()
    {
        _retiredEpoch = _lastRoster?.RosterEpoch;
        _engineStopped = true;
        _lastRoster = null;
        _isoFacts.Clear();
        _tiles = [];
        foreach (var row in Rows)
        {
            ResetFrame(row);
            row.StatusLabel = "IDLE";
            row.RecLabel = "OFF";
            row.HasPreviewTile = false;
            row.PreviewTally = "none";
            row.MeterWidth = 0;
            row.RosterEpoch = null;
            row.HighestObservationRevision = 0;
        }
    }

    public void ApplyMultiviewFact(MultiviewSharedTexture multiview)
    {
        _tiles = multiview.Tiles;
        foreach (var row in Rows) PatchPreview(row);
    }

    private void PatchPreview(MultiviewInputRow row)
    {
        if (row.StatusLabel == "VIDEO OFF")
        {
            row.HasPreviewTile = false;
            row.PreviewTally = "none";
            return;
        }
        var tile = _tiles.FirstOrDefault(item => item.Role == "source" &&
            item.Slot == row.SlotNumber - 1 &&
            (string.IsNullOrEmpty(item.SourceId) || item.SourceId == row.SourceId));
        var crop = MultiviewTileCropPolicy.Resolve(tile);
        row.HasPreviewTile = crop.HasValue;
        if (crop.HasValue) row.PreviewCropRect = crop.Value;
        row.PreviewTally = MultiviewTileCropPolicy.Tally(tile, row.FormatStale);
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
        row.ObservedWidth = 0;
        row.ObservedHeight = 0;
        row.ObservedFps = 0;
    }

    private void PatchSlot(int slotNumber)
    {
        if (slotNumber < 1 || slotNumber > Rows.Count) return;
        var row = Rows[slotNumber - 1];
        var slot = _slots.FirstOrDefault(item => item.SlotNumber == slotNumber);
        var sourceId = slot is { IsAssigned: true } ? ShowInputRosterService.SlotSourceId(slot) : null;
        if (!string.Equals(row.SourceId, sourceId, StringComparison.Ordinal))
        {
            row.SourceId = sourceId;
            row.LastAppliedSourceInstance = null;
            ResetFrame(row);
            row.RecLabel = sourceId is not null && _isoFacts.TryGetValue(sourceId, out var iso)
                ? iso.State.ToUpperInvariant() : "OFF";
            row.HasPreviewTile = false;
            row.PreviewTally = "none";
        }
        row.RosterEpoch = _lastRoster?.RosterEpoch;
        row.HighestObservationRevision = _lastRoster?.RosterRevision ?? 0;
        row.MeterWidth = 0;
        if (_engineStopped || sourceId is null || slot?.InShow != true)
        {
            row.StatusLabel = "IDLE";
            ResetFrame(row);
            PatchPreview(row);
            return;
        }
        if (slot.Kind != ShowInputKind.ZoomParticipant)
        {
            var capture = _captureDevices?.FirstOrDefault(item => $"capture:{item.Id}" == sourceId);
            if (capture is not null && capture.ConnectionState == CaptureConnectionState.Connected && capture.SignalPresent)
            {
                row.StatusLabel = "LIVE";
                row.FormatLabel = capture.ObservedFrameWidth > 0 && capture.ObservedFrameHeight > 0 &&
                    capture.ObservedFrameRate > 0
                    ? $"{capture.ObservedFrameWidth}×{capture.ObservedFrameHeight}@{capture.ObservedFrameRate}" : "—";
            }
            else
            {
                row.StatusLabel = capture is null ? "NO INCOMING" : "CONNECTING";
                ResetFrame(row);
            }
            PatchPreview(row);
            return;
        }
        if (_lastRoster is null)
        {
            row.StatusLabel = "IDLE";
            ResetFrame(row);
            PatchPreview(row);
            return;
        }
        if (_lastRoster?.MeetingState != "in_meeting")
        {
            row.StatusLabel = "NO INCOMING";
            ResetFrame(row);
            PatchPreview(row);
            return;
        }
        var participant = _lastRoster.Participants.FirstOrDefault(item =>
            string.Equals(item.UserId, slot.ParticipantId, StringComparison.Ordinal));
        if (participant is null)
        {
            row.StatusLabel = "NO INCOMING";
            ResetFrame(row);
            PatchPreview(row);
            return;
        }
        if (participant.SourceGeneration > 0 && _lastRoster.RosterEpoch is { } epoch)
        {
            var currentInstance = new CoreVideoPro.MediaCore.Contracts.SourceInstanceIdentity
            {
                SourceId = sourceId,
                InstanceId = $"{epoch}:{participant.SourceGeneration}",
                ProcessEpoch = epoch,
                Generation = participant.SourceGeneration
            };
            if (row.LastAppliedSourceInstance is { } previousInstance &&
                previousInstance != currentInstance)
                ResetFrame(row);
            row.LastAppliedSourceInstance = currentInstance;
        }
        row.MeterWidth = Math.Clamp(participant.AudioLevel ?? 0, 0, 100) * 0.8;
        if (participant.VideoOn == false)
        {
            row.StatusLabel = "VIDEO OFF";
            ResetFrame(row);
            PatchPreview(row);
            return;
        }
        var video = _lastRoster.ZoomSubscriptions.FirstOrDefault(item =>
            item.Kind == "participant-video" && item.ParticipantId == slot.ParticipantId &&
            (participant.SourceGeneration <= 0 || item.SourceGeneration == participant.SourceGeneration) &&
            item.DeliveredWidth > 0 && item.DeliveredHeight > 0 && item.DeliveredFps > 0 &&
            item.LastFrameAtMs >= 0 && item.LastFrameAgeMs >= 0);
        if (video is null)
        {
            if (row.LastFrameAtMs < 0)
            {
                row.StatusLabel = "CONNECTING";
                ResetFrame(row);
            }
            PatchPreview(row);
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
        row.StatusLabel = row.FormatStale ? "STALLED" : participant.Talking == true ? "TALKING" : "LIVE";
        row.FormatLabel = $"{video.DeliveredWidth}×{video.DeliveredHeight}@{video.DeliveredFps}";
        ApplyCameraCap(row, video.DeliveredWidth, video.DeliveredHeight, video.DeliveredFps);
        PatchPreview(row);
    }
}
