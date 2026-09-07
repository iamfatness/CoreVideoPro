using System.Collections.Concurrent;

namespace CoreVideoPro.ShowEngine.Tests;

/// <summary>
/// The injected <c>Func&lt;TimeSpan, CancellationToken, Task&gt;</c> for tests. Every requested
/// duration is recorded in <see cref="Recorded"/>. A duration in <see cref="SetImmediate"/> (and
/// <see cref="TimeSpan.Zero"/>, always) completes at once — that is how a test makes the request
/// timeout, the handshake fallback, or a backoff fire deterministically. Every other duration PARKS
/// until the test calls <see cref="Release"/> or the caller's token is cancelled, so nothing in the
/// supervisor ever fires on its own.
/// </summary>
public sealed class DelayController
{
    private readonly ConcurrentQueue<TimeSpan> _recorded = new();
    private readonly HashSet<TimeSpan> _immediate = new();
    private readonly List<(TimeSpan Duration, TaskCompletionSource Tcs)> _parked = new();
    private readonly object _gate = new();

    public IReadOnlyList<TimeSpan> Recorded => _recorded.ToArray();

    public void SetImmediate(params TimeSpan[] durations)
    {
        lock (_gate)
        {
            foreach (var d in durations) _immediate.Add(d);
        }
    }

    public void ClearImmediate(TimeSpan duration)
    {
        lock (_gate) { _immediate.Remove(duration); }
    }

    /// <summary>Release every waiter currently parked on <paramref name="duration"/>. Returns how many.</summary>
    public int Release(TimeSpan duration)
    {
        List<TaskCompletionSource> release = new();
        lock (_gate)
        {
            for (var i = _parked.Count - 1; i >= 0; i--)
            {
                if (_parked[i].Duration != duration) continue;
                release.Add(_parked[i].Tcs);
                _parked.RemoveAt(i);
            }
        }

        foreach (var tcs in release) tcs.TrySetResult();
        return release.Count;
    }

    public int ParkedCount(TimeSpan duration)
    {
        lock (_gate) { return _parked.Count(p => p.Duration == duration); }
    }

    public Task DelayAsync(TimeSpan duration, CancellationToken ct)
    {
        _recorded.Enqueue(duration);
        if (ct.IsCancellationRequested) return Task.FromCanceled(ct);

        TaskCompletionSource tcs;
        lock (_gate)
        {
            if (duration <= TimeSpan.Zero || _immediate.Contains(duration)) return Task.CompletedTask;
            tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            _parked.Add((duration, tcs));
        }

        var registration = ct.Register(static state => ((TaskCompletionSource)state!).TrySetCanceled(), tcs);
        return tcs.Task.ContinueWith(t =>
        {
            registration.Dispose();
            return t;
        }, TaskContinuationOptions.ExecuteSynchronously).Unwrap();
    }
}
