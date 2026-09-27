using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private sealed record PendingAudioRoute(MediaCoreAudioRouteWire Draft, long Sequence, string AuthorityEpoch);
    private readonly Dictionary<(string SourceId, string BusId), PendingAudioRoute> _audioRouteDrafts = [];
    private readonly SemaphoreSlim _audioRouteLocalGate = new(1, 1);
    private long _audioRouteEditSequence;
    private string? _lastOwnAudioRouteEpoch;
    private long _lastOwnAudioRouteRevision;
    private DateTimeOffset _lastAudioRouteLocalEditAt = DateTimeOffset.MinValue;
    private string _audioRouteControlNotice = string.Empty;
    public string AudioRouteControlNotice
    {
        get => _audioRouteControlNotice;
        private set
        {
            if (_audioRouteControlNotice == value) return;
            _audioRouteControlNotice = value;
            OnPropertyChanged();
        }
    }

    private void OnAudioRoutingMatrixChanged(AudioRoutingCrosspointViewModel cell)
    {
        RefreshAudioProcessingTargets();
        var control = _bridge.LastSnapshot?.AudioRoutingMatrix.Control;
        if (control is null)
        {
            // Compatibility with an older native binary that has no route
            // control contract. The core remains authoritative once upgraded.
            _lastAudioRouteLocalEditAt = DateTimeOffset.UtcNow;
            _ = TrySyncMediaCoreAsync();
            return;
        }
        var sourceId = ResolveAudioRoutingMatrixSourceId(cell.SourceId);
        var draft = new MediaCoreAudioRouteWire(sourceId, cell.Bus.Id, cell.IsRouted, cell.GainDb);
        var key = (cell.SourceId, cell.Bus.Id);
        var sequence = Interlocked.Increment(ref _audioRouteEditSequence);
        _audioRouteDrafts[key] = new(draft, sequence, control.AuthorityEpoch);
        CommandStatus = $"Audio route edit pending core application: {cell.SourceLabel} → {cell.Bus.Label}";
        AudioRouteControlNotice = "Audio route edit pending core application";
        _ = SubmitLocalAudioRouteAsync(key, draft, sequence, control.AuthorityEpoch, control.Revision);
    }

    private async Task SubmitLocalAudioRouteAsync(
        (string SourceId, string BusId) key, MediaCoreAudioRouteWire draft,
        long sequence, string editEpoch, long editRevision)
    {
        await _audioRouteLocalGate.WaitAsync().ConfigureAwait(true);
        try
        {
            if (!_audioRouteDrafts.TryGetValue(key, out var pending) || pending.Sequence != sequence)
                return;
            // Multiple cells changed by one local gesture serialize behind our
            // own applied result. A concurrent API client still conflicts at core.
            var expectedRevision = _lastOwnAudioRouteEpoch == editEpoch &&
                _lastOwnAudioRouteRevision >= editRevision
                ? _lastOwnAudioRouteRevision : editRevision;
            var outcome = await RequestAudioRouteControlAsync(
                draft, editEpoch, expectedRevision).ConfigureAwait(true);
            if (!_audioRouteDrafts.TryGetValue(key, out pending) || pending.Sequence != sequence)
                return;
            if (outcome.Kind == AudioRouteControlOutcomeKind.Applied)
            {
                _lastOwnAudioRouteEpoch = outcome.Control?.AuthorityEpoch;
                _lastOwnAudioRouteRevision = outcome.Control?.Revision ?? expectedRevision + 1;
                _audioRouteDrafts.Remove(key);
                if (_bridge.LastSnapshot is { } snapshot) HydrateAudioRoutingMatrixFromSnapshot(snapshot);
                CommandStatus = $"Audio route applied at revision {_lastOwnAudioRouteRevision}";
                AudioRouteControlNotice = _audioRouteDrafts.Count == 0 ? string.Empty :
                    "Audio route edits pending core application";
            }
            else
            {
                CommandStatus = outcome.Kind == AudioRouteControlOutcomeKind.Conflict
                    ? $"Audio route conflict at revision {outcome.Control?.Revision}; draft retained"
                    : $"Audio route {outcome.Kind.ToString().ToLowerInvariant()}; draft retained";
                AudioRouteControlNotice = CommandStatus;
            }
        }
        finally
        {
            _audioRouteLocalGate.Release();
        }
    }

    /// <summary>The local Control API and the grid share native admission.</summary>
    public async Task<AudioRouteControlOutcome> RequestAudioRouteControlAsync(
        MediaCoreAudioRouteWire draft, string? expectedEpoch = null, long? expectedRevision = null,
        string? operationId = null)
    {
        try
        {
            await EnsureMediaCoreRunningAsync("Starting media core...").ConfigureAwait(true);
            return await _bridge.SetAudioRouteControlAsync(
                draft, expectedEpoch, expectedRevision, operationId).ConfigureAwait(true);
        }
        catch (Exception)
        {
            return new(AudioRouteControlOutcomeKind.Reconciling,
                operationId ?? string.Empty, draft, null, null);
        }
    }

    // Snapshot is applied state. A pending or conflicted local cell keeps its
    // draft; other cells follow core immediately, without a time-based blind spot.
    private void HydrateAudioRoutingMatrixFromSnapshot(NativeMediaCoreStateSnapshot snapshot)
    {
        var native = snapshot.AudioRoutingMatrix;
        if (native is null || AudioRoutingMatrix.Rows.Count == 0) return;
        if (native.Control is { } control && _audioRouteDrafts.Values.Any(draft =>
                draft.AuthorityEpoch != control.AuthorityEpoch))
            AudioRouteControlNotice = "Audio route core restarted; draft retained for review";
        if (native.Control is null && DateTimeOffset.UtcNow - _lastAudioRouteLocalEditAt < TimeSpan.FromSeconds(2))
            return;

        var uiIdByEngineId = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var row in AudioRoutingMatrix.Rows)
        {
            var engineId = ResolveAudioRoutingMatrixSourceId(row.SourceId);
            if (!string.IsNullOrWhiteSpace(engineId)) uiIdByEngineId.TryAdd(engineId, row.SourceId);
        }
        var sends = new List<(string SourceId, string BusId, double GainDb)>(native.Sends.Count);
        foreach (var send in native.Sends)
        {
            if (uiIdByEngineId.TryGetValue(send.SourceId, out var uiSourceId))
                sends.Add((uiSourceId, send.BusId, send.GainDb));
        }
        if (AudioRoutingMatrix.ApplyCoreSends(sends, _audioRouteDrafts.Keys.ToHashSet(),
                acceptEmpty: native.Control?.Revision > 0,
                preserveSelectedGain: native.Control is null))
            RefreshAudioProcessingTargets();
    }
}
