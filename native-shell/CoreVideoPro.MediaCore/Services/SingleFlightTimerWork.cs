namespace CoreVideoPro.MediaCore.Services;

// Admit a timer tick before it can block on application monitors. Reset retires
// queued callbacks without overlapping old and new in-flight generations.
internal sealed class SingleFlightTimerWork
{
    private int _inFlight;
    private long _generation;
    public long Reset() => Interlocked.Increment(ref _generation);
    public long CurrentGeneration => Volatile.Read(ref _generation);
    public async Task RunAsync(long generation, Func<Task> work)
    {
        if (generation != Volatile.Read(ref _generation) || Interlocked.CompareExchange(ref _inFlight, 1, 0) != 0) return;
        try { if (generation == Volatile.Read(ref _generation)) await work().ConfigureAwait(false); }
        finally { Volatile.Write(ref _inFlight, 0); }
    }
}
