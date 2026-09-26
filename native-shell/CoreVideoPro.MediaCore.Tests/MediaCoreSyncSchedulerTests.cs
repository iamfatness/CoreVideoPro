using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class MediaCoreSyncSchedulerTests
{
    [Fact]
    public async Task CommandPressureRejectsBeforeSendAndPreservesAdmittedOrder()
    {
        var scheduler = new MediaCoreSyncScheduler();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var first = scheduler.RunCommandAsync(async () =>
        {
            entered.SetResult();
            await release.Task;
            return -1;
        }, CancellationToken.None);
        await entered.Task;

        var sent = new List<int>();
        var admitted = Enumerable.Range(0, MediaCoreSyncScheduler.MaxWaitingCommands)
            .Select(index => scheduler.RunCommandAsync(() =>
            {
                sent.Add(index);
                return Task.FromResult(index);
            }, CancellationToken.None)).ToArray();
        Assert.Equal(MediaCoreSyncScheduler.MaxWaitingCommands, scheduler.WaitingCommands);
        await Assert.ThrowsAsync<MediaCoreCommandOverloadedException>(() =>
            scheduler.RunCommandAsync(() => Task.FromResult(999), CancellationToken.None));
        Assert.Equal(1, scheduler.OverloadCount);
        Assert.Empty(sent);

        release.SetResult();
        await first;
        await Task.WhenAll(admitted);
        Assert.Equal(Enumerable.Range(0, MediaCoreSyncScheduler.MaxWaitingCommands), sent);
        Assert.Equal(0, scheduler.WaitingCommands);
    }
    [Fact]
    public async Task OverlappingCommandsBothReachTheCoreWhileEmptyPollIsCoalesced()
    {
        var scheduler = new MediaCoreSyncScheduler();
        var firstEntered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirst = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var sent = new List<string>();

        var first = scheduler.RunCommandAsync(async () =>
        {
            sent.Add("preview=one");
            firstEntered.SetResult();
            await releaseFirst.Task;
            return 1;
        }, CancellationToken.None);
        await firstEntered.Task;

        var second = scheduler.RunCommandAsync(() =>
        {
            sent.Add("preview=two");
            return Task.FromResult(2);
        }, CancellationToken.None);

        await Assert.ThrowsAsync<MediaCoreSyncInFlightException>(() =>
            scheduler.TryPollAsync(() => Task.FromResult(0), CancellationToken.None));
        Assert.False(second.IsCompleted);
        Assert.Equal(["preview=one"], sent);

        releaseFirst.SetResult();
        Assert.Equal(1, await first);
        Assert.Equal(2, await second);
        Assert.Equal(["preview=one", "preview=two"], sent);
    }

    [Fact]
    public async Task CancelledCommandWaiterDoesNotSend()
    {
        var scheduler = new MediaCoreSyncScheduler();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var first = scheduler.RunCommandAsync(async () =>
        {
            entered.SetResult();
            await release.Task;
            return 1;
        }, CancellationToken.None);
        await entered.Task;

        using var cancellation = new CancellationTokenSource();
        var sent = false;
        var second = scheduler.RunCommandAsync(() =>
        {
            sent = true;
            return Task.FromResult(2);
        }, cancellation.Token);
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => second);
        release.SetResult();
        Assert.Equal(1, await first);
        Assert.False(sent);
    }
}
