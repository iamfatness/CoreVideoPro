using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public enum AudioRouteControlOutcomeKind { Applied, Conflict, Reconciling, Incompatible }

public sealed record AudioRouteControlOutcome(
    AudioRouteControlOutcomeKind Kind,
    string OperationId,
    MediaCoreAudioRouteWire Draft,
    NativeMediaCoreAudioRoutingMatrix? Applied,
    NativeMediaCoreAudioRouteControl? Control);

/// <summary>
/// Both the Windows editor and local Control API use this one-shot admission
/// path. A lost reply is resolved from the core snapshot, never by blindly
/// replaying a route mutation; a conflicting draft stays separate from applied.
/// </summary>
public sealed class AudioRouteControlCoordinator(
    Func<IReadOnlyList<NativeMediaCoreCommand>, Task<NativeMediaCoreStateSnapshot>> send,
    Func<Task<NativeMediaCoreStateSnapshot>> readSnapshot)
{
    private readonly SemaphoreSlim _gate = new(1, 1);
    private NativeMediaCoreStateSnapshot? _observed;

    public void Reset() => _observed = null;

    public void Observe(NativeMediaCoreStateSnapshot snapshot)
    {
        var incoming = snapshot.AudioRoutingMatrix.Control;
        var current = _observed?.AudioRoutingMatrix.Control;
        if (incoming is null) return;
        if (current is not null && current.AuthorityEpoch == incoming.AuthorityEpoch &&
            incoming.Revision < current.Revision) return;
        _observed = snapshot;
    }

    public async Task<AudioRouteControlOutcome> SubmitAsync(
        MediaCoreAudioRouteWire draft, string? expectedEpoch = null, long? expectedRevision = null,
        string? operationId = null, CancellationToken cancellationToken = default)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (_observed?.AudioRoutingMatrix.Control is null)
                Observe(await readSnapshot().ConfigureAwait(false));
            var control = _observed?.AudioRoutingMatrix.Control;
            operationId ??= Guid.NewGuid().ToString("N");
            if (control is null || string.IsNullOrWhiteSpace(control.AuthorityEpoch))
                return new(AudioRouteControlOutcomeKind.Incompatible, operationId, draft,
                    _observed?.AudioRoutingMatrix, control);

            var identity = new ControlOperationIdentity
            {
                OperationId = operationId,
                AuthorityEpoch = expectedEpoch ?? control.AuthorityEpoch,
                ExpectedRevision = expectedRevision ?? control.Revision
            };
            NativeMediaCoreStateSnapshot? response = null;
            try
            {
                response = await send([MediaCoreCommandBuilder.BuildAudioRouteControlCommand(draft, identity)])
                    .ConfigureAwait(false);
                Observe(response);
            }
            catch (Exception) when (!cancellationToken.IsCancellationRequested)
            {
                // Native may have applied the edit before its reply was lost.
            }
            if (FindResult(response?.AudioRoutingMatrix.Control, operationId) is null)
            {
                try
                {
                    response = await readSnapshot().ConfigureAwait(false);
                    Observe(response);
                }
                catch (Exception) when (!cancellationToken.IsCancellationRequested) { }
            }
            var result = FindResult(response?.AudioRoutingMatrix.Control, operationId);
            var kind = result is null ? AudioRouteControlOutcomeKind.Reconciling : result.Status switch
            {
                "applied" => AudioRouteControlOutcomeKind.Applied,
                "conflict" => AudioRouteControlOutcomeKind.Conflict,
                _ => AudioRouteControlOutcomeKind.Incompatible
            };
            return new(kind, operationId, draft, response?.AudioRoutingMatrix,
                response?.AudioRoutingMatrix.Control);
        }
        finally
        {
            _gate.Release();
        }
    }

    private static NativeMediaCoreAudioRouteResult? FindResult(
        NativeMediaCoreAudioRouteControl? control, string operationId) =>
        control?.LastResult?.OperationId == operationId
            ? control.LastResult
            : control?.RecentResults.FirstOrDefault(result => result.OperationId == operationId);
}
