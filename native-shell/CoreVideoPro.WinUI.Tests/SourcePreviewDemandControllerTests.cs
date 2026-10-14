using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class SourcePreviewDemandControllerTests
{
    [Fact]
    public async Task OnlyVisibleConsumerRenewsAndRepeatedVisibilityDoesNotChurnControls()
    {
        var sent = new List<bool>();
        var demand = new SourcePreviewDemandController(active => { sent.Add(active); return Task.CompletedTask; });
        Assert.Empty(sent);
        await demand.SetVisibleAsync(true);
        await demand.SetVisibleAsync(true);
        Assert.Equal(new[] { true }, sent);
        await demand.RefreshAsync();
        Assert.Equal(new[] { true, true }, sent);
        await demand.SetVisibleAsync(false);
        Assert.Equal(new[] { true, true, false }, sent);
    }

    [Fact]
    public async Task CoreStopClearsInterestAndRecoveryRenewsOnlyIfPageStillVisible()
    {
        var sent = new List<bool>();
        var demand = new SourcePreviewDemandController(active => { sent.Add(active); return Task.CompletedTask; });
        await demand.SetVisibleAsync(true);
        await demand.SetEngineAvailableAsync(false);
        await demand.SetEngineAvailableAsync(true);
        await demand.SetVisibleAsync(false);
        await demand.SetEngineAvailableAsync(false);
        await demand.SetEngineAvailableAsync(true);
        Assert.Equal(new[] { true, false, true, false, false, false }, sent);
    }

    [Fact]
    public async Task HideWhileControlIsInFlightCoalescesToReleaseWithoutAnotherTimerTick()
    {
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var sent = new List<bool>();
        var demand = new SourcePreviewDemandController(async active =>
        {
            sent.Add(active);
            if (active) { entered.SetResult(); await release.Task; }
        });
        var activating = demand.SetVisibleAsync(true);
        await entered.Task;
        await demand.SetVisibleAsync(false);
        release.SetResult();
        await activating;
        Assert.Equal(new[] { true, false }, sent);
    }
}
