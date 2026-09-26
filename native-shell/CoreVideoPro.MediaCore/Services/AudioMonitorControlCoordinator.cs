using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public enum AudioMonitorControlOutcomeKind { Applied, Conflict, Reconciling, Incompatible }

public sealed record AudioMonitorControlOutcome(
    AudioMonitorControlOutcomeKind Kind,
    string OperationId,
    MediaCoreAudioMonitorWire Draft,
    NativeMediaCoreAudioMixSession? Applied,
    NativeMediaCoreMonitorControl? Control);

/// <summary>
/// The monitor editor and local Control API submit the same revisioned command.
/// A property edit remains a draft until the native core reports its operation
/// result; a missing acknowledgement triggers a snapshot read, never a blind
/// second mutation. PCM and endpoint work remain in the native adapter.
/// </summary>
public sealed class AudioMonitorControlCoordinator(
    Func<IReadOnlyList<NativeMediaCoreCommand>, Task<NativeMediaCoreStateSnapshot>> send,
    Func<Task<NativeMediaCoreStateSnapshot>> readSnapshot)
{
    private readonly SemaphoreSlim _gate = new(1, 1);
    private NativeMediaCoreStateSnapshot? _observed;

    public void Reset() => _observed = null;

    public void Observe(NativeMediaCoreStateSnapshot snapshot)
    {
        var incoming = snapshot.AudioMixSession.MonitorControl;
        var current = _observed?.AudioMixSession.MonitorControl;
        if (incoming is null) return;
        if (current is not null && current.AuthorityEpoch == incoming.AuthorityEpoch &&
            incoming.Revision < current.Revision) return;
        _observed = snapshot;
    }

    public async Task<AudioMonitorControlOutcome> SubmitAsync(
        MediaCoreAudioMonitorWire draft, string? expectedEpoch = null, long? expectedRevision = null,
        string? operationId = null, CancellationToken cancellationToken = default)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var observed = _observed;
            if (observed?.AudioMixSession.MonitorControl is null)
            {
                observed = await readSnapshot().ConfigureAwait(false);
                Observe(observed);
            }
            var control = _observed?.AudioMixSession.MonitorControl;
            operationId ??= Guid.NewGuid().ToString("N");
            if (control is null || string.IsNullOrWhiteSpace(control.AuthorityEpoch))
                return new(AudioMonitorControlOutcomeKind.Incompatible, operationId, draft,
                    _observed?.AudioMixSession, control);

            var identity = new ControlOperationIdentity
            {
                OperationId = operationId,
                AuthorityEpoch = expectedEpoch ?? control.AuthorityEpoch,
                ExpectedRevision = expectedRevision ?? control.Revision
            };
            var command = MediaCoreCommandBuilder.BuildAudioMonitorControlCommand(draft, identity);
            NativeMediaCoreStateSnapshot? response = null;
            try
            {
                response = await send([command]).ConfigureAwait(false);
                Observe(response);
            }
            catch (Exception) when (!cancellationToken.IsCancellationRequested)
            {
                // The command might have applied before its reply was lost.
            }

            if (FindResult(response?.AudioMixSession.MonitorControl, operationId) is null)
            {
                try
                {
                    response = await readSnapshot().ConfigureAwait(false);
                    Observe(response);
                }
                catch (Exception) when (!cancellationToken.IsCancellationRequested)
                {
                    // Keep the draft and show reconciling until a later snapshot.
                }
            }

            var result = FindResult(response?.AudioMixSession.MonitorControl, operationId);
            var kind = result is null
                ? AudioMonitorControlOutcomeKind.Reconciling
                : result.Status switch
                {
                    "applied" => AudioMonitorControlOutcomeKind.Applied,
                    "conflict" => AudioMonitorControlOutcomeKind.Conflict,
                    _ => AudioMonitorControlOutcomeKind.Incompatible
                };
            return new(kind, operationId, draft, response?.AudioMixSession,
                response?.AudioMixSession.MonitorControl);
        }
        finally
        {
            _gate.Release();
        }
    }

    private static NativeMediaCoreMonitorControlResult? FindResult(
        NativeMediaCoreMonitorControl? control, string operationId) =>
        control?.LastResult?.OperationId == operationId
            ? control.LastResult
            : control?.RecentResults.FirstOrDefault(result => result.OperationId == operationId);
}
