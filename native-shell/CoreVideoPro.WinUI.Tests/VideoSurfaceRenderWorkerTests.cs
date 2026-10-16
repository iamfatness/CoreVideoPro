using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class VideoSurfaceRenderWorkerTests
{
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
