using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public class SingleFlightTimerWorkTests
{
    [Fact]
    public async Task ContendedApplicationMonitorDoesNotAccumulateTimerCallbacks()
    {
        var work = new SingleFlightTimerWork();
        var generation = work.Reset();
        var gate = new object();
        using var held = new ManualResetEventSlim();
        using var attempting = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        // Dedicated threads, not Task.Run: both participants below block on
        // purpose, and a saturated CI agent injects new pool threads only about
        // twice a second. Queueing them made pool scheduling, rather than the
        // admission logic under test, decide whether this test passed.
        var owner = new Thread(() => { lock (gate) { held.Set(); release.Wait(TimeSpan.FromSeconds(5)); } }) { IsBackground = true };
        owner.Start();
        Assert.True(held.Wait(TimeSpan.FromSeconds(2)));
        var count = 0;
        Task Tick() => work.RunAsync(generation, () =>
        {
            Interlocked.Increment(ref count);
            attempting.Set();
            lock (gate) { }
            return Task.CompletedTask;
        });
        var first = new Thread(() => Tick().GetAwaiter().GetResult()) { IsBackground = true };
        first.Start();
        try
        {
            Assert.True(attempting.Wait(TimeSpan.FromSeconds(2)));
            for (var i = 0; i < 500; i++) Assert.True(Tick().IsCompletedSuccessfully);
            Assert.Equal(1, Volatile.Read(ref count));
            var next = work.Reset();
            Assert.True(work.RunAsync(next, () => throw new Exception("Old work still owns admission")).IsCompletedSuccessfully);
        }
        finally { release.Set(); }
        Assert.True(owner.Join(TimeSpan.FromSeconds(5)));
        Assert.True(first.Join(TimeSpan.FromSeconds(5)));
        await Tick(); // retired generation must remain inert after old work finishes
        Assert.Equal(1, count);
        await work.RunAsync(work.Reset(), () => { count++; return Task.CompletedTask; });
        Assert.Equal(2, count);
    }

    [Fact]
    public async Task FailedTickReleasesAdmission()
    {
        var work = new SingleFlightTimerWork();
        var generation = work.Reset();
        await Assert.ThrowsAsync<InvalidOperationException>(() => work.RunAsync(generation, () => throw new InvalidOperationException()));
        var called = false;
        await work.RunAsync(generation, () => { called = true; return Task.CompletedTask; });
        Assert.True(called);
    }
}
