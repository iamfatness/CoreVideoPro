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
    private volatile bool _engineStopped;
    private volatile bool _previewConsumerActive;
    private string? _retiredEpoch;
    private readonly Dictionary<string, IsoOutputLifecycleFact> _isoFacts = new(StringComparer.Ordinal);
    private IReadOnlyList<MultiviewTile> _tiles = [];
    private readonly object _previewGate = new();
    // At most one descriptor per currently assigned source (ten stable rows).
    private readonly Dictionary<string, VideoSurfaceState> _sourcePreviews = new(StringComparer.Ordinal);
    private readonly Dictionary<string, long> _previewGenerations = new(StringComparer.Ordinal);
    private readonly Dictionary<string, ulong> _previewEpochs = new(StringComparer.Ordinal);
    private readonly Dictionary<string, CoreVideoPro.MediaCore.Contracts.SourceInstanceIdentity> _previewInstances = new(StringComparer.Ordinal);
    private readonly HashSet<string> _previewBlockedIds = new(StringComparer.Ordinal);
    private readonly HashSet<string> _previewNeedsNewEpoch = new(StringComparer.Ordinal);
    private HashSet<string> _previewSourceIds = new(StringComparer.Ordinal);
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
        {
            lock (_previewGate)
            {
                _sourcePreviews.Clear();
                _previewEpochs.Clear();
                _previewGenerations.Clear();
                _previewInstances.Clear();
                _previewBlockedIds.Clear();
                _previewNeedsNewEpoch.Clear();
            }
            foreach (var row in Rows) ResetFrame(row);
        }
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
        lock (_previewGate)
        {
            _sourcePreviews.Clear();
            _previewEpochs.Clear();
            _previewGenerations.Clear();
            _previewInstances.Clear();
            _previewBlockedIds.Clear();
            _previewNeedsNewEpoch.Clear();
        }
        foreach (var row in Rows)
        {
            ResetFrame(row);
            row.StatusLabel = "IDLE";
            row.RecLabel = "OFF";
            row.HasPreviewTile = false;
            row.PreviewSurface = null;
            row.PreviewTally = "none";
            row.RosterEpoch = null;
            row.HighestObservationRevision = 0;
        }
    }

    public void EngineStarted()
    {
        if (!_engineStopped) return;
        _engineStopped = false;
        foreach (var row in Rows) PatchSlot(row.SlotNumber);
    }

    public void ApplyMultiviewFact(MultiviewSharedTexture multiview)
    {
        _tiles = multiview.Tiles;
        foreach (var row in Rows) PatchPreview(row);
    }

    /// <summary>Accept native per-source exports, never a crop of a mutable wall texture.
    /// Called on the bridge thread; only a new handle/format schedules UI work.</summary>
    public bool TryAcceptSourcePreview(ParticipantSharedTexture texture)
    {
        if (!_previewConsumerActive || !texture.IsValid) return false;
        var sourceId = texture.ParticipantId.StartsWith("capture:", StringComparison.Ordinal) ||
                       texture.ParticipantId.StartsWith("media:", StringComparison.Ordinal)
            ? texture.ParticipantId : $"zoom:{texture.ParticipantId}";
        var handle = new SharedTextureHandle
        {
            NtHandle = CoreProtocolParser.ParseSharedHandleHex(texture.SharedHandleHex),
            Width = texture.Width, Height = texture.Height,
            Format = texture.Format, FrameNumber = texture.FrameNumber
        };
        if (!handle.IsValid) return false;
        lock (_previewGate)
        {
            if (!_previewConsumerActive || !_previewSourceIds.Contains(sourceId) || _engineStopped) return false;
            var rosterGeneration = _previewInstances.TryGetValue(sourceId, out var instance) ? instance.Generation : 0;
            if (texture.SourceGeneration > 0 && texture.SourceGeneration < rosterGeneration) return false;
            // New pixels may outrun the roster lane. Stage their descriptor, but
            // PatchPreview cannot expose it until that exact incarnation is accepted.
            if (_previewBlockedIds.Contains(sourceId) &&
                !(texture.SourceGeneration > 0 && texture.SourceGeneration > rosterGeneration)) return false;
            _previewEpochs.TryGetValue(sourceId, out var previousEpoch);
            _previewGenerations.TryGetValue(sourceId, out var previousGeneration);
            if (texture.SourceEpoch < previousEpoch ||
                (_previewNeedsNewEpoch.Contains(sourceId) && previousEpoch > 0 && texture.SourceEpoch <= previousEpoch)) return false;
            _previewNeedsNewEpoch.Remove(sourceId);
            _previewEpochs[sourceId] = texture.SourceEpoch;
            _previewGenerations[sourceId] = texture.SourceGeneration;
            if (_sourcePreviews.TryGetValue(sourceId, out var existing) &&
                existing.PendingSharedHandle is { } previous &&
                previous.NtHandle == handle.NtHandle && previous.Width == handle.Width &&
                previous.Height == handle.Height && previous.Format == handle.Format &&
                previousEpoch == texture.SourceEpoch && previousGeneration == texture.SourceGeneration) return false;
            _sourcePreviews[sourceId] = VideoSurfaceState.Waiting(VideoSurfaceKind.Participant,
                $"source-thumbnail:{sourceId}", sourceId) with
            {
                PendingSharedHandle = handle,
                NaturalSourceWidth = texture.Width, NaturalSourceHeight = texture.Height
            };
            return true;
        }
    }

    public void SetPreviewConsumerActive(bool active)
    {
        if (_previewConsumerActive == active) return;
        _previewConsumerActive = active;
        if (!active)
            lock (_previewGate) _sourcePreviews.Clear();
        RefreshSourcePreviews();
    }

    public void RefreshSourcePreviews()
    {
        foreach (var row in Rows) PatchPreview(row);
    }

    private void PatchPreview(MultiviewInputRow row)
    {
        VideoSurfaceState? surface = null;
        if (row.SourceId is { } currentSourceId)
            lock (_previewGate)
            {
                var eligible = Rows.Any(item => item.SourceId == currentSourceId &&
                    _slots.Any(slot => slot.SlotNumber == item.SlotNumber && slot.InShow && slot.IsAssigned) &&
                    (currentSourceId.StartsWith("media:", StringComparison.Ordinal) ||
                     currentSourceId.StartsWith("capture:", StringComparison.Ordinal) && item.StatusLabel == "LIVE" ||
                     currentSourceId.StartsWith("zoom:", StringComparison.Ordinal) &&
                         item.StatusLabel is not ("VIDEO OFF" or "NO INCOMING" or "IDLE")));
                if (eligible) _previewBlockedIds.Remove(currentSourceId);
                else
                {
                    _previewBlockedIds.Add(currentSourceId);
                    if (!IsFuturePreview(currentSourceId)) _sourcePreviews.Remove(currentSourceId);
                }
            }
        if (_previewConsumerActive && !_engineStopped && row.SourceId is { } sourceId &&
            _slots.Any(slot => slot.SlotNumber == row.SlotNumber && slot.InShow && slot.IsAssigned) &&
            (sourceId.StartsWith("media:", StringComparison.Ordinal) ||
             sourceId.StartsWith("capture:", StringComparison.Ordinal) && row.StatusLabel == "LIVE" ||
             sourceId.StartsWith("zoom:", StringComparison.Ordinal) &&
                 row.StatusLabel is not ("VIDEO OFF" or "NO INCOMING" or "IDLE")))
            lock (_previewGate)
                if (!_previewGenerations.TryGetValue(sourceId, out var generation) || generation == 0 ||
                    _previewInstances.TryGetValue(sourceId, out var instance) && instance.Generation == generation)
                    _sourcePreviews.TryGetValue(sourceId, out surface);
        // Clear before exposing another identity; an unavailable export is a placeholder.
        row.PreviewSurface = surface;
        row.HasPreviewTile = surface is not null;
        var tile = _tiles.FirstOrDefault(item => item.Role == "source" &&
            item.Slot == row.SlotNumber - 1 && item.SourceId == row.SourceId);
        row.PreviewTally = row.StatusLabel == "VIDEO OFF" ? "none" :
            MultiviewTileCropPolicy.Tally(tile, row.FormatStale);
    }

    // Caller holds _previewGate. An early announcement is not a displayed image.
    private bool IsFuturePreview(string id) => _previewGenerations.TryGetValue(id, out var generation) && generation > 0 &&
        (!_previewInstances.TryGetValue(id, out var instance) || generation > instance.Generation);

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
        lock (_previewGate)
        {
            _previewSourceIds = Rows.Where(item => item.SourceId is not null)
                .Select(item => item.SourceId!).ToHashSet(StringComparer.Ordinal);
            foreach (var retired in _sourcePreviews.Keys.Concat(_previewGenerations.Keys).Concat(_previewEpochs.Keys).Concat(_previewBlockedIds).Concat(_previewInstances.Keys).Distinct()
                         .Where(id => !_previewSourceIds.Contains(id)).ToArray())
            {
                _sourcePreviews.Remove(retired);
                _previewEpochs.Remove(retired);
                _previewGenerations.Remove(retired);
                _previewInstances.Remove(retired);
                _previewBlockedIds.Remove(retired);
                _previewNeedsNewEpoch.Remove(retired);
            }
        }
        row.RosterEpoch = _lastRoster?.RosterEpoch;
        row.HighestObservationRevision = _lastRoster?.RosterRevision ?? 0;
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
            {
                ResetFrame(row);
                lock (_previewGate)
                {
                    if (!_previewInstances.TryGetValue(sourceId, out var applied) || applied != currentInstance)
                    {
                        // The publication may already belong to this new roster
                        // incarnation. Do not fence it behind an epoch that has
                        // already arrived: that would wait until another resize/rejoin.
                        var qualified = _previewGenerations.TryGetValue(sourceId, out var publishedGeneration) &&
                            publishedGeneration == currentInstance.Generation && publishedGeneration > 0;
                        if (qualified) _previewNeedsNewEpoch.Remove(sourceId);
                        else if (!IsFuturePreview(sourceId))
                        {
                            _sourcePreviews.Remove(sourceId);
                            if (_previewEpochs.ContainsKey(sourceId)) _previewNeedsNewEpoch.Add(sourceId);
                        }
                        _previewInstances[sourceId] = currentInstance;
                    }
                }
            }
            lock (_previewGate) _previewInstances[sourceId] = currentInstance;
            row.LastAppliedSourceInstance = currentInstance;
        }
        if (participant.VideoOn == false)
        {
            row.StatusLabel = "VIDEO OFF";
            lock (_previewGate)
            {
                if (!IsFuturePreview(sourceId))
                {
                    _sourcePreviews.Remove(sourceId);
                    if (_previewEpochs.ContainsKey(sourceId)) _previewNeedsNewEpoch.Add(sourceId);
                }
            }
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
