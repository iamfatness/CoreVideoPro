using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class UiOwnedSnapshotTests
{
    [Fact]
    public async Task ShutdownCancelsWaitingCaptureAndQueuedCallbackCannotTouchDisposedState()
    {
        using var cancelled = new CancellationTokenSource();
        Action? queued = null;
        var touched = false;
        var capture = UiOwnedSnapshot.CaptureAsync(() => { touched = true; return 1; }, false,
            action => { queued = action; return true; }, cancelled.Token);
        cancelled.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => capture);
        queued!();
        Assert.False(touched);
    }

    [Fact]
    public async Task BackgroundCallerEnumeratesOnlyWhenOwnerExecutesCapture()
    {
        var values = new List<int> { 1 };
        Action? queued = null;
        var capture = UiOwnedSnapshot.CaptureAsync(() => values.ToArray(), false, action => { queued = action; return true; });
        Assert.False(capture.IsCompleted);
        values.Add(2); // UI finishes its roster update before running queued capture.
        queued!();
        values.Clear();
        Assert.Equal(new[] { 1, 2 }, await capture);
    }

    [Fact]
    public async Task CaptureFailureReachesAwaiterWithoutEscapingDispatcherCallback()
    {
        Action? queued = null;
        var capture = UiOwnedSnapshot.CaptureAsync<int>(() => throw new InvalidOperationException("capture failed"),
            false, action => { queued = action; return true; });
        queued!();
        var error = await Assert.ThrowsAsync<InvalidOperationException>(() => capture);
        Assert.Equal("capture failed", error.Message);
    }

    [Fact]
    public async Task RefusedDispatcherCannotFallBackToWorkerEnumeration()
    {
        var enumerated = false;
        var capture = UiOwnedSnapshot.CaptureAsync(() => { enumerated = true; return 1; }, false, _ => false);
        await Assert.ThrowsAsync<OperationCanceledException>(() => capture);
        Assert.False(enumerated);
    }
}
