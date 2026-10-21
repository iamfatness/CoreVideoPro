using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class VideoSurfaceRenderWorkerTests
{
    [Fact]
    public void ProducerReleaseBetweenPollsIsConsumedBeforeOriginalNextSlot()
    {
        var cadence = new PresentationCadence();
        const long frequency = 6_000_000, period = 100_000, releaseDelay = 6_000;
        var commits = new List<int>();
        var pending = false;
        var attempts = 0;
        // The producer owns its texture at each 60 Hz tick, releases it 1 ms
        // later, and starts the next frame at the next tick. A periodic-only
        // consumer misses every image despite a perfect average polling rate.
        for (long now = 1; now < 600 * period; now += 600)
        {
            if (!cadence.TrySubmit(now, frequency, 60, pending, out var retry)) continue;
            attempts++;
            if (!retry) pending = true;
            var frame = (int)((now - 1) / period);
            if ((now - 1) % period < releaseDelay) continue;
            commits.Add(frame);
            pending = false; // A committed sibling must not be repumped this slot.
        }
        Assert.Equal(Enumerable.Range(0, 600), commits);
        Assert.InRange(attempts, 1200, 1800);
    }

    [Fact]
    public void ReadinessRetriesArePacedAndCannotExtendOrReplayMissedSlots()
    {
        var cadence = new PresentationCadence();
        const long frequency = 6_000_000;
        Assert.True(cadence.TrySubmit(1, frequency, 60, false, out var retry));
        Assert.False(retry);
        Assert.Equal(10_000, cadence.Delay100ns(1, frequency, true));
        Assert.False(cadence.TrySubmit(6000, frequency, 60, true, out _));
        Assert.True(cadence.TrySubmit(6001, frequency, 60, true, out retry));
        Assert.True(retry);
        Assert.False(cadence.TrySubmit(6002, frequency, 60, true, out _));
        Assert.False(cadence.TrySubmit(12001, frequency, 60, false, out _));
        Assert.True(cadence.TrySubmit(100001, frequency, 60, true, out retry));
        Assert.False(retry); // The old slot was not granted extra time.
        Assert.True(cadence.TrySubmit(6_000_001, frequency, 60, true, out retry));
        Assert.False(retry); // A long stall starts one fresh slot, without backlog.
        Assert.False(cadence.TrySubmit(6_000_002, frequency, 60, true, out _));
    }

    [Fact]
    public void RenderClockKeepsSixtyHzWithoutUiWakeupsAndSkipsMissedDeadlines()
    {
        var cadence = new PresentationCadence();
        const long frequency = 10_000_000;
        long now = 1;
        for (var frame = 0; frame < 600; frame++)
        {
            Assert.True(cadence.ShouldSubmit(now, frequency, 60));
            Assert.False(cadence.ShouldSubmit(now + 1, frequency, 60));
            now += cadence.Delay100ns(now, frequency);
        }
        Assert.InRange(now, 99_999_000, 100_001_000);
        now += frequency; // A driver stall must not create a catch-up burst.
        Assert.True(cadence.ShouldSubmit(now, frequency, 60));
        Assert.False(cadence.ShouldSubmit(now + 1, frequency, 60));
        Assert.InRange(cadence.Delay100ns(now, frequency), 166_665, 166_667);
    }

    [Theory]
    [InlineData(60)]
    [InlineData(75)]
    [InlineData(144)]
    public void SourceCadenceMaintainsThirtyHzAcrossDisplayRefreshRates(int displayHz)
    {
        var cadence = new PresentationCadence();
        const long frequency = 1_000_000;
        var submissions = Enumerable.Range(0, displayHz * 10)
            .Count(frame => cadence.ShouldSubmit((long)Math.Round(frame * (double)frequency / displayHz), frequency, 30));
        Assert.InRange(submissions, 299, 301);
    }

    [Fact]
    public void LateDisplayCallbackDoesNotReplayAnOldFrameBacklog()
    {
        var cadence = new PresentationCadence();
        Assert.True(cadence.ShouldSubmit(100, 1_000_000, 30));
        Assert.True(cadence.ShouldSubmit(10_000_000, 1_000_000, 30));
        Assert.False(cadence.ShouldSubmit(10_000_001, 1_000_000, 30));
    }

    [Fact]
    public void BusyRendererReceivesNewestFrameWithoutAnOldFrameQueue()
    {
        var requests = new LatestPresentationRequest<string>();
        Assert.True(requests.Offer("currently-rendering"));
        Assert.Equal("currently-rendering", requests.Take());
        for (var i = 0; i < 10000; i++) Assert.True(requests.Offer(i.ToString()));
        Assert.Equal("9999", requests.Take());
        Assert.Null(requests.Take());
    }

    [Fact]
    public async Task RetirementCannotResurrectWorkFromConcurrentPublisher()
    {
        var requests = new LatestPresentationRequest<object>();
        await Task.WhenAll(Task.Run(() => { for (var i = 0; i < 10000; i++) requests.Offer(new()); }),
            Task.Run(requests.Close));
        Assert.Null(requests.Take());
        Assert.False(requests.Offer(new()));
    }

    [Fact]
    public void PreviewAndInspectAliasesRetainOneConsumerUntilLastViewRetires()
    {
        var assignments = new SharedTextureRenderAssignments(4);
        var thumbnail = assignments.Acquire(101);
        var inspect = assignments.Acquire(101);
        var preview = assignments.Acquire(101);
        Assert.Equal(thumbnail, inspect); Assert.Equal(thumbnail, preview);
        assignments.Release(101, thumbnail);
        // Releasing the thumbnail must not move the still-active aliases.
        var reopened = assignments.Acquire(101);
        Assert.Equal(inspect, reopened);
        assignments.Release(101, reopened);
        assignments.Release(101, inspect);
        assignments.Release(101, preview);
        Assert.All(assignments.Loads, load => Assert.Equal(0, load));
    }

    [Fact]
    public async Task ConcurrentAliasesNeverAcquireTwoKeyedMutexConsumers()
    {
        var assignments = new SharedTextureRenderAssignments(4);
        var owners = await Task.WhenAll(Enumerable.Range(0, 100).Select(_ => Task.Run(() => assignments.Acquire(42))));
        Assert.Single(owners.Distinct());
        foreach (var owner in owners) assignments.Release(42, owner);
        Assert.All(assignments.Loads, load => Assert.Equal(0, load));
    }

    [Fact]
    public void DistinctSourcesUsePersistentGroupsAndRetirementReleasesCapacity()
    {
        var assignments = new SharedTextureRenderAssignments(4);
        var owners = Enumerable.Range(1, 10).Select(i => assignments.Acquire((ulong)i)).ToArray();
        Assert.All(assignments.Loads, load => Assert.InRange(load, 2, 3));
        for (var i = 0; i < owners.Length; i++) assignments.Release((ulong)i + 1, owners[i]);
        Assert.All(assignments.Loads, load => Assert.Equal(0, load));
    }
}
