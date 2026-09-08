namespace CoreVideoPro.WinUI.Services;

// Reports an outstanding UI presentation stage from a separate timer. No log IO
// occurs in the per-frame path. This observes a blocked driver call; it cannot
// cancel CopyResource, resource creation, or a driver that ignores DoNotWait.
internal sealed class PresentationStageWatchdog : IDisposable
{
    private readonly object _gate = new();
    private readonly Func<long> _clock;
    private readonly Action<string, long> _report;
    private readonly Timer? _timer;
    private string? _stage;
    private long _started;
    private bool _reported;
    private bool _disposed;

    internal PresentationStageWatchdog(Action<string, long> report,
        Func<long>? clock = null, bool startTimer = true)
    {
        _report = report;
        _clock = clock ?? (() => Environment.TickCount64);
        if (startTimer) _timer = new Timer(_ => Check(), null, 1000, 1000);
    }

    internal void Mark(string? stage)
    {
        lock (_gate)
        {
            if (_disposed) return;
            _stage = stage;
            _started = _clock();
            _reported = false;
        }
    }

    internal void Check()
    {
        string? stage;
        long elapsed;
        lock (_gate)
        {
            elapsed = _clock() - _started;
            if (_disposed || _stage is null || _reported || elapsed < 250) return;
            stage = _stage;
            _reported = true;
        }
        try { _report(stage, elapsed); } catch { /* diagnostic must not crash the process */ }
    }

    public void Dispose()
    {
        lock (_gate) { _disposed = true; _stage = null; }
        _timer?.Dispose();
    }
}
