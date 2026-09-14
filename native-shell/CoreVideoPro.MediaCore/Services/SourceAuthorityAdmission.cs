using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Shared admission for capture, spine and full snapshots. Pass the supervisor's
/// RestartCount captured before sending the request, not its value on completion.
/// Serialize publication with admission at the bridge; this class does not dispatch events.
/// </summary>
public sealed class SourceAuthorityAdmission
{
    private readonly object _gate = new();
    private readonly int _maxRetiredEpochs;
    private readonly HashSet<string> _retiredEpochs = new(StringComparer.Ordinal);
    private long? _restartCount;
    private NativeSourceAuthority? _accepted;

    public SourceAuthorityAdmission(int maxRetiredEpochs = 1024)
    {
        if (maxRetiredEpochs is <= 0 or > 65_536)
            throw new ArgumentOutOfRangeException(nameof(maxRetiredEpochs));
        _maxRetiredEpochs = maxRetiredEpochs;
    }

    /// <summary>
    /// Null remains null. Invalid/rejected evidence never exposes a partial catalog.
    /// A higher process generation resets epoch history; older generations cannot
    /// repopulate it. Epoch capacity exhaustion rejects new epochs without evicting fences.
    /// </summary>
    public NativeSourceAuthority? Admit(long capturedRestartCount, NativeSourceAuthority? authority)
    {
        TryAdmit(capturedRestartCount, authority, out var admitted);
        return admitted;
    }

    /// <summary>
    /// Returns false when the entire snapshot is older or conflicts with the
    /// accepted ordering fence. Current malformed evidence returns true with an
    /// invalid-empty catalog so unrelated state can still advance safely.
    /// </summary>
    public bool TryAdmit(long capturedRestartCount, NativeSourceAuthority? authority,
        out NativeSourceAuthority? admitted)
    {
        lock (_gate)
        {
            if (capturedRestartCount < 0 || (_restartCount is { } known && capturedRestartCount < known))
            {
                admitted = authority is null ? null : Invalid();
                return false;
            }

            if (_restartCount is null || capturedRestartCount > _restartCount.Value)
            {
                _restartCount = capturedRestartCount;
                _accepted = null;
                _retiredEpochs.Clear();
            }

            if (authority is null)
            {
                admitted = null;
                return true;
            }
            var candidate = authority.Validated();
            if (!candidate.Valid)
            {
                admitted = Invalid();
                return true;
            }
            if (_retiredEpochs.Contains(candidate.ProcessEpoch))
            {
                admitted = Invalid();
                return false;
            }

            if (_accepted is { } previous)
            {
                if (candidate.ProcessEpoch == previous.ProcessEpoch)
                {
                    if (candidate.Sequence < previous.Sequence)
                    {
                        admitted = Invalid();
                        return false;
                    }
                    if (candidate.Sequence == previous.Sequence)
                    {
                        admitted = SameEvidence(candidate, previous) ? previous : Invalid();
                        return SameEvidence(candidate, previous);
                    }
                }
                else
                {
                    if (_retiredEpochs.Count >= _maxRetiredEpochs)
                    {
                        admitted = Invalid();
                        return false;
                    }
                    _retiredEpochs.Add(previous.ProcessEpoch);
                }
            }

            _accepted = candidate;
            admitted = candidate;
            return true;
        }
    }

    private static bool SameEvidence(NativeSourceAuthority a, NativeSourceAuthority b) =>
        a.Version == b.Version && a.Valid == b.Valid && a.ProcessEpoch == b.ProcessEpoch &&
        a.Sequence == b.Sequence && a.Sources!.SequenceEqual(b.Sources!);

    private static NativeSourceAuthority Invalid() => new() { Valid = false, Sources = [] };
}
