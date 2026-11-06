using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ParticipantListRefreshSchedulerTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void UnloadInvalidatesEitherQueuedPhase(bool clearAlreadyRan)
    {
        var queue = new Queue<Action>();
        var touches = new List<string>();
        var refresh = new ParticipantListRefreshScheduler((action, _) => queue.Enqueue(action),
            () => touches.Add("clear"), () => touches.Add("bind"));
        refresh.Activate(); refresh.Schedule();
        if (clearAlreadyRan) queue.Dequeue()();
        refresh.Deactivate();
        touches.Clear();
        while (queue.TryDequeue(out var action)) action();
        Assert.Empty(touches);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void OldCallbacksCannotClearNewLifetimesPendingWork(bool clearAlreadyRan)
    {
        var queue = new Queue<Action>();
        var touches = new List<string>();
        var refresh = new ParticipantListRefreshScheduler((action, _) => queue.Enqueue(action),
            () => touches.Add("clear"), () => touches.Add("bind"));
        refresh.Activate(); refresh.Schedule();
        if (clearAlreadyRan) queue.Dequeue()();
        refresh.Deactivate(); refresh.Activate(); refresh.Schedule();
        touches.Clear();
        queue.Dequeue()(); // Old clear or rebind is ahead of the new clear.
        refresh.Schedule();
        Assert.Single(queue); // Stale callback did not reset the new coalescing flag.
        while (queue.TryDequeue(out var action)) action();
        Assert.Equal(new[] { "clear", "bind" }, touches);
    }

    [Fact]
    public void CoalescesBothTurnsAndBindsLatestRoster()
    {
        var queue = new Queue<Action>();
        var latest = "first";
        string? observed = null;
        var clears = 0;
        var refresh = new ParticipantListRefreshScheduler((action, _) => queue.Enqueue(action),
            () => clears++, () => observed = latest);
        refresh.Schedule(); Assert.Empty(queue);
        refresh.Activate(); refresh.Schedule(); refresh.Schedule();
        Assert.Single(queue);
        queue.Dequeue()();
        latest = "replacement"; refresh.Schedule(); refresh.Schedule();
        Assert.Single(queue);
        queue.Dequeue()();
        Assert.Equal(1, clears); Assert.Equal("replacement", observed); Assert.Empty(queue);
        refresh.Schedule(); Assert.Single(queue);
    }

    [Fact]
    public void ReentrantRosterChangeDuringBindingSchedulesOneFurtherRefresh()
    {
        var queue = new Queue<Action>();
        var bindings = 0;
        ParticipantListRefreshScheduler refresh = null!;
        refresh = new((action, _) => queue.Enqueue(action), () => { }, () =>
        {
            if (++bindings == 1) { refresh.Schedule(); refresh.Schedule(); }
        });
        refresh.Activate(); refresh.Schedule();
        while (queue.TryDequeue(out var action)) action();
        Assert.Equal(2, bindings);
    }

    [Fact]
    public void UnloadDuringClearDoesNotEnqueueBinding()
    {
        var queue = new Queue<Action>();
        ParticipantListRefreshScheduler refresh = null!;
        refresh = new((action, _) => queue.Enqueue(action), () => refresh.Deactivate(),
            () => throw new InvalidOperationException("Must not touch unloaded XAML"));
        refresh.Activate(); refresh.Schedule(); queue.Dequeue()();
        Assert.Empty(queue);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void FailedUiWorkRemainsVisibleAndDoesNotWedgeLaterRefresh(bool failBinding)
    {
        var queue = new Queue<Action>();
        var fail = true;
        void Work(bool binding) { if (fail && binding == failBinding) throw new InvalidOperationException("visible failure"); }
        var refresh = new ParticipantListRefreshScheduler((action, _) => queue.Enqueue(action),
            () => Work(false), () => Work(true));
        refresh.Activate(); refresh.Schedule();
        if (failBinding) queue.Dequeue()();
        Assert.Throws<InvalidOperationException>(() => queue.Dequeue()());
        fail = false; refresh.Schedule();
        Assert.Single(queue);
        while (queue.TryDequeue(out var action)) action();
    }

    [Fact]
    public void DispatcherShutdownDoesNotPreventANewLifetimeFromScheduling()
    {
        var queue = new Queue<Action>();
        var shuttingDown = true;
        var bindings = 0;
        var refresh = new ParticipantListRefreshScheduler((action, _) => { if (!shuttingDown) queue.Enqueue(action); },
            () => { }, () => bindings++);
        refresh.Activate(); refresh.Schedule(); Assert.Empty(queue);
        refresh.Deactivate(); shuttingDown = false; refresh.Activate(); refresh.Schedule();
        while (queue.TryDequeue(out var action)) action();
        Assert.Equal(1, bindings);
    }
}
