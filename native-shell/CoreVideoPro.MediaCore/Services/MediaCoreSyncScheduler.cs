namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Serializes media-core syncs. Operator/production commands wait for the slot;
/// periodic empty polls are coalesced when a command is queued or a sync is busy.
/// A cancelled waiter never sends its command.
/// </summary>
internal sealed class MediaCoreSyncScheduler
{
    internal const int MaxWaitingCommands = 32;
    internal const int ReservedStopCommands = 4;
    private readonly SemaphoreSlim _slot = new(1, 1);
    private int _waitingCommands;
    private int _waitingNormalCommands;
    private int _waitingStopCommands;
    private long _overloadCount;
    private long _coalescedPollCount;

    public int WaitingCommands => Volatile.Read(ref _waitingCommands);
    public long OverloadCount => Interlocked.Read(ref _overloadCount);
    public long CoalescedPollCount => Interlocked.Read(ref _coalescedPollCount);

    public async Task<T> RunCommandAsync<T>(Func<Task<T>> send, CancellationToken cancellationToken,
        bool stopCommand = false)
    {
        ArgumentNullException.ThrowIfNull(send);
        var limit = stopCommand ? ReservedStopCommands : MaxWaitingCommands;
        var admitted = stopCommand
            ? Interlocked.Increment(ref _waitingStopCommands)
            : Interlocked.Increment(ref _waitingNormalCommands);
        if (admitted > limit)
        {
            DecrementLane(stopCommand);
            Interlocked.Increment(ref _overloadCount);
            throw new MediaCoreCommandOverloadedException();
        }
        Interlocked.Increment(ref _waitingCommands);
        try
        {
            await _slot.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            DecrementLane(stopCommand);
            Interlocked.Decrement(ref _waitingCommands);
        }

        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            return await send().ConfigureAwait(false);
        }
        finally
        {
            _slot.Release();
        }
    }

    private void DecrementLane(bool stopCommand)
    {
        if (stopCommand) Interlocked.Decrement(ref _waitingStopCommands);
        else Interlocked.Decrement(ref _waitingNormalCommands);
    }

    public async Task<T> TryPollAsync<T>(Func<Task<T>> send, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(send);
        cancellationToken.ThrowIfCancellationRequested();
        if (Volatile.Read(ref _waitingCommands) != 0 || !_slot.Wait(0))
        {
            Interlocked.Increment(ref _coalescedPollCount);
            throw new MediaCoreSyncInFlightException();
        }

        try
        {
            // A command may have queued between the first check and acquiring
            // the slot. Yield it the next sync rather than running another poll.
            if (Volatile.Read(ref _waitingCommands) != 0)
            {
                Interlocked.Increment(ref _coalescedPollCount);
                throw new MediaCoreSyncInFlightException();
            }
            cancellationToken.ThrowIfCancellationRequested();
            return await send().ConfigureAwait(false);
        }
        finally
        {
            _slot.Release();
        }
    }
}
