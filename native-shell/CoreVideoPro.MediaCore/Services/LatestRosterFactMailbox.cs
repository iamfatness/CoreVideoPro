using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// One pending full-roster observation plus one active delivery. Replaces older
/// observations before UI projection; an omitted revision is repaired by a
/// full snapshot barrier. No command or applied result enters this mailbox.
/// </summary>
internal sealed class LatestRosterFactMailbox(Action<RawCaptureSnapshot, long> deliver)
{
    private readonly object _gate = new();
    private (RawCaptureSnapshot Fact, long Generation)? _pending;
    private bool _draining;
    private long _coalesced;
    private long _discardedOlder;

    public long Coalesced { get { lock (_gate) return _coalesced; } }
    public long DiscardedOlder { get { lock (_gate) return _discardedOlder; } }
    public int PendingCount { get { lock (_gate) return _pending is null ? 0 : 1; } }

    public void Post(RawCaptureSnapshot fact, long generation)
    {
        lock (_gate)
        {
            if (_pending is { } pending)
            {
                if (pending.Generation == generation &&
                    !ZoomRosterSnapshotPolicy.IsNewerFact(pending.Fact, fact))
                {
                    _discardedOlder++;
                    return;
                }
                _coalesced++;
            }
            _pending = (fact, generation);
            if (_draining) return;
            _draining = true;
        }
        _ = Task.Run(DrainAsync);
    }

    public void Reset()
    {
        lock (_gate) _pending = null;
    }

    private async Task DrainAsync()
    {
        while (true)
        {
            (RawCaptureSnapshot Fact, long Generation)? item;
            lock (_gate)
            {
                item = _pending;
                _pending = null;
                if (item is null) { _draining = false; return; }
            }
            try { deliver(item.Value.Fact, item.Value.Generation); }
            catch (Exception error)
            {
                DiagnosticLog.WriteException("media-core.log", "roster fact projection failed", error);
            }
            // Bound roster/strip collection refresh to 30 Hz even if the core
            // publishes bursty talking and mute facts. Newer facts replace the slot.
            await Task.Delay(33).ConfigureAwait(false);
        }
    }
}
