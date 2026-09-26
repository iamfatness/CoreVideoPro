namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Serializes media-core syncs. Operator/production commands wait for the slot;
/// periodic empty polls are coalesced when a command is queued or a sync is busy.
/// A cancelled waiter never sends its command.
/// </summary>
internal sealed class MediaCoreSyncScheduler
{
    private readonly SemaphoreSlim _slot = new(1, 1);
    private int _waitingCommands;

    public async Task<T> RunCommandAsync<T>(Func<Task<T>> send, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(send);
        Interlocked.Increment(ref _waitingCommands);
        try
        {
            await _slot.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
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

    public async Task<T> TryPollAsync<T>(Func<Task<T>> send, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(send);
        cancellationToken.ThrowIfCancellationRequested();
        if (Volatile.Read(ref _waitingCommands) != 0 || !_slot.Wait(0))
        {
            throw new MediaCoreSyncInFlightException();
        }

        try
        {
            // A command may have queued between the first check and acquiring
            // the slot. Yield it the next sync rather than running another poll.
            if (Volatile.Read(ref _waitingCommands) != 0)
            {
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
