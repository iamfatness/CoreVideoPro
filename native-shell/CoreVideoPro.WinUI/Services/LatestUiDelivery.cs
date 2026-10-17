namespace CoreVideoPro.WinUI.Services;

/// <summary>At most one pending state and one UI callback. Never holds a lock while applying UI state.</summary>
internal sealed class LatestUiDelivery<T>(Func<Action, bool> enqueue, Action<T> apply) where T : class
{
    private readonly object _gate = new();
    private T? _latest;
    private bool _queued;

    internal void Offer(T value)
    {
        lock (_gate)
        {
            _latest = value;
            if (_queued) return;
            _queued = true;
        }
        Schedule();
    }

    private void Schedule()
    {
        if (enqueue(Drain)) return;
        lock (_gate) { _queued = false; _latest = null; }
    }

    private void Drain()
    {
        T? value;
        lock (_gate) { value = _latest; _latest = null; }
        try { if (value is not null) apply(value); }
        finally
        {
            bool again;
            lock (_gate) { again = _latest is not null; if (!again) _queued = false; }
            // Yield to other UI work even when the producer is faster than the UI.
            if (again) Schedule();
        }
    }
}
