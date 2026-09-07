using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// The ordering seam pulled out of <c>StudioControlSurface</c>'s OHG host-command path, so the one
/// property that matters — a later command cannot start inside an earlier command's await — is
/// testable without a DispatcherQueue.
/// </summary>
public sealed class SequentialAsyncQueueTests
{
    [Fact]
    public async Task TwoEnqueuedTasksRunInOrder_EvenWhenTheFirstAwaits()
    {
        var queue = new SequentialAsyncQueue();
        var order = new List<string>();
        var gate = new TaskCompletionSource();

        var first = queue.Enqueue(async () =>
        {
            lock (order) order.Add("first-start");
            await gate.Task.ConfigureAwait(false);   // the take that used to let the next look in
            lock (order) order.Add("first-end");
        });

        var second = queue.Enqueue(() =>
        {
            lock (order) order.Add("second");
            return Task.CompletedTask;
        });

        // While the first is parked mid-await, the second must NOT have run.
        await Task.Delay(50);
        lock (order) Assert.Equal(new[] { "first-start" }, order);

        gate.SetResult();
        await Task.WhenAll(first, second);

        lock (order) Assert.Equal(new[] { "first-start", "first-end", "second" }, order);
    }

    [Fact]
    public async Task AThrowingLinkIsReported_AndTheChainKeepsDraining()
    {
        var errors = new List<Exception>();
        var queue = new SequentialAsyncQueue(ex => { lock (errors) errors.Add(ex); });
        var ran = false;

        var faulted = queue.Enqueue(() => throw new InvalidOperationException("boom"));
        var after = queue.Enqueue(() => { ran = true; return Task.CompletedTask; });

        await Task.WhenAll(faulted, after);

        // The queue's own task never faults — a faulted tail would deadlock every later link.
        Assert.False(faulted.IsFaulted);
        Assert.True(ran);
        lock (errors)
        {
            Assert.Single(errors);
            Assert.Equal("boom", errors[0].Message);
        }
    }

    [Fact]
    public async Task AnAsyncFaultAlsoLeavesTheChainIntact()
    {
        var errors = new List<Exception>();
        var queue = new SequentialAsyncQueue(ex => { lock (errors) errors.Add(ex); });
        var order = new List<string>();

        var faulted = queue.Enqueue(async () =>
        {
            await Task.Yield();
            throw new InvalidOperationException("late boom");
        });
        var after = queue.Enqueue(() => { lock (order) order.Add("after"); return Task.CompletedTask; });

        await Task.WhenAll(faulted, after);

        lock (errors) Assert.Single(errors);
        lock (order) Assert.Equal(new[] { "after" }, order);
    }
}
