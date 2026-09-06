namespace CoreVideoPro.WinUI.Services;

internal static class UiOwnedSnapshot
{
    // The queued callback owns all enumeration. Failures fault the awaiting task
    // instead of escaping into DispatcherQueue's native fail-fast boundary.
    public static Task<T> CaptureAsync<T>(Func<T> capture, bool hasThreadAccess, Func<Action, bool> enqueue, CancellationToken cancellationToken = default)
    {
        var completion = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        void Capture()
        {
            if (cancellationToken.IsCancellationRequested) { completion.TrySetCanceled(cancellationToken); return; }
            try { completion.TrySetResult(capture()); }
            catch (Exception error) { completion.TrySetException(error); }
        }
        if (hasThreadAccess) Capture();
        else if (!enqueue(Capture)) completion.TrySetException(new OperationCanceledException("UI dispatcher is shutting down."));
        return completion.Task.WaitAsync(cancellationToken);
    }
}
