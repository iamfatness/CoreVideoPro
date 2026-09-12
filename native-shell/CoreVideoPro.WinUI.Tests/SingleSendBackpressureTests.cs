using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// T1.5 review fix. With Engine off, the core poll can now collide with a single-send sync. A
/// skipped Preview-scene pick or multiview layout used to be dropped on the assumption that "the
/// periodic sync reapplies", and nothing repeats them with Engine off.
/// </summary>
public sealed class SingleSendBackpressureTests
{
    // A media core whose single sync slot is taken by the poll for the first N sends.
    private sealed class FakeCore
    {
        public int InFlightFailures { get; set; }
        public List<string> Applied { get; } = [];
        public int Attempts { get; private set; }

        public Task SyncAsync(string payload)
        {
            Attempts++;
            if (InFlightFailures > 0)
            {
                InFlightFailures--;
                throw new MediaCoreSyncInFlightException();
            }
            Applied.Add(payload);
            return Task.CompletedTask;
        }
    }

    [Fact]
    public async Task APreviewScenePickThatCollidesWithThePollStillReachesTheCore()
    {
        var core = new FakeCore { InFlightFailures = 1 };
        Task<bool>? retry = null;

        // The preview-scene path: send once; a skip queues the production-sync retry worker,
        // whose loop is StudioViewModel.RetryDeferredProductionSyncAsync.
        var outcome = await SingleSendBackpressure.RunAsync(
            () => core.SyncAsync("preview=interview"),
            onSkipped: () => retry = StudioViewModel.RetryDeferredProductionSyncAsync(
                keepRunning: () => true,
                shouldDefer: () => false,
                syncAttempt: () => core.SyncAsync("preview=interview"),
                CancellationToken.None,
                retryDelayMs: 1),
            onFailed: ex => throw new Xunit.Sdk.XunitException($"not a failure: {ex.Message}"));

        Assert.Equal(SingleSendOutcome.SkippedForBackpressure, outcome);
        Assert.NotNull(retry);
        Assert.True(await retry!);
        Assert.Equal(new[] { "preview=interview" }, core.Applied);
    }

    [Fact]
    public async Task ASkippedMultiviewLayoutIsReArmedAndDelivered()
    {
        var core = new FakeCore { InFlightFailures = 2 };
        var outcomes = new List<SingleSendOutcome>();

        // The multiview-layout path: a skip re-arms the sender's own debounce, which re-runs
        // the same send. Modelled here as a loop that runs until nothing is re-armed.
        var armed = true;
        while (armed)
        {
            armed = false;
            outcomes.Add(await SingleSendBackpressure.RunAsync(
                () => core.SyncAsync("layout#3x3"),
                onSkipped: () => armed = true,
                onFailed: _ => { }));
        }

        Assert.Equal(
            new[] { SingleSendOutcome.SkippedForBackpressure, SingleSendOutcome.SkippedForBackpressure, SingleSendOutcome.Sent },
            outcomes);
        Assert.Equal(new[] { "layout#3x3" }, core.Applied);
    }

    [Fact]
    public async Task ARealFailureGoesToTheExistingHandlerAndIsNotRetriedAsBackpressure()
    {
        var skipped = false;
        Exception? failure = null;

        var outcome = await SingleSendBackpressure.RunAsync(
            () => throw new InvalidOperationException("media-core sync failed: rejected"),
            onSkipped: () => skipped = true,
            onFailed: ex => failure = ex);

        Assert.Equal(SingleSendOutcome.Failed, outcome);
        Assert.False(skipped);
        Assert.IsType<InvalidOperationException>(failure);
    }

    [Fact]
    public async Task ADeliveredSendNeedsNothingElse()
    {
        var core = new FakeCore();
        var skipped = false;

        var outcome = await SingleSendBackpressure.RunAsync(
            () => core.SyncAsync("preview=intro"), () => skipped = true, _ => { });

        Assert.Equal(SingleSendOutcome.Sent, outcome);
        Assert.False(skipped);
        Assert.Equal(new[] { "preview=intro" }, core.Applied);
    }
}
