using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class LatestUiDeliveryTests
{
    [Fact]
    public async Task ProducerDoesNotWaitForAStalledUiApply()
    {
        var callbacks = new System.Collections.Concurrent.ConcurrentQueue<Action>();
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var delivery = new LatestUiDelivery<string>(a => { callbacks.Enqueue(a); return true; }, _ =>
        {
            entered.Set();
            release.Wait();
        });
        delivery.Offer("first");
        Assert.True(callbacks.TryDequeue(out var callback));
        var ui = Task.Run(callback!);
        try
        {
            Assert.True(entered.Wait(TimeSpan.FromSeconds(2)));
            var producer = Task.Run(() => delivery.Offer("latest"));
            Assert.True(producer.Wait(TimeSpan.FromSeconds(2)), "Producer waited on UI work");
        }
        finally { release.Set(); await ui; }
        Assert.Single(callbacks);
    }

    [Fact]
    public void BlockedUiRetainsOnlyLatestStateAndOneCallback()
    {
        var callbacks = new Queue<Action>();
        var applied = new List<string>();
        var delivery = new LatestUiDelivery<string>(a => { callbacks.Enqueue(a); return true; }, applied.Add);
        for (var i = 0; i < 10000; i++) delivery.Offer(i.ToString());
        Assert.Single(callbacks);
        callbacks.Dequeue()();
        Assert.Equal(["9999"], applied);
        Assert.Empty(callbacks);
    }

    [Fact]
    public void UpdatesDuringApplyYieldAndDeliverLatestWithoutLosingWakeup()
    {
        var callbacks = new Queue<Action>();
        var applied = new List<string>();
        LatestUiDelivery<string> delivery = null!;
        delivery = new(a => { callbacks.Enqueue(a); return true; }, value =>
        {
            applied.Add(value);
            if (value == "first") { delivery.Offer("stale"); delivery.Offer("latest"); }
        });
        delivery.Offer("first");
        callbacks.Dequeue()();
        Assert.Equal(["first"], applied);
        Assert.Single(callbacks);
        callbacks.Dequeue()();
        Assert.Equal(["first", "latest"], applied);
    }

    [Fact]
    public void RejectedDispatchDoesNotLeaveDeliveryPermanentlyQueued()
    {
        var accept = false;
        Action? callback = null;
        string? applied = null;
        var delivery = new LatestUiDelivery<string>(a => { if (!accept) return false; callback = a; return true; }, s => applied = s);
        delivery.Offer("rejected"); accept = true; delivery.Offer("current"); callback!();
        Assert.Equal("current", applied);
    }
}
