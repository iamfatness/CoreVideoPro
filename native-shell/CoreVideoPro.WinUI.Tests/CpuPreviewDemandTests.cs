using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class CpuPreviewDemandTests
{
    [Fact]
    public void GpuReadyDoesNotDisableAnotherVisibleCpuConsumer()
    {
        var consumers = new CpuPreviewConsumers();
        var first = new object(); var second = new object();
        var notifications = 0;
        consumers.Changed += () => notifications++;
        consumers.Set(first, true);
        consumers.Set(second, true);
        consumers.Set(first, false);
        Assert.True(consumers.Active);
        Assert.Equal(1, notifications);
        consumers.Set(second, false);
        Assert.False(consumers.Active);
        Assert.Equal(2, notifications);
    }

    [Fact]
    public void FallbackCanResumeAfterAllGpuMonitorsAreReady()
    {
        var consumers = new CpuPreviewConsumers(); var host = new object();
        consumers.Set(host, true);
        consumers.Set(host, false);
        Assert.False(consumers.Active);
        consumers.Set(host, true);
        Assert.True(consumers.Active);
        consumers.Set(host, false); // unload/hidden view
        Assert.False(consumers.Active);
    }
}
