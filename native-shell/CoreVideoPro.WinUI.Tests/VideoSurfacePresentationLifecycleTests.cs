using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class VideoSurfacePresentationLifecycleTests
{
    [Fact]
    public void LateGpuSourceAttachesOnceAndReloadGetsFreshPresenter()
    {
        var nextId = 0;
        int? active = null;
        var disposed = new List<int>();
        var lifecycle = new VideoSurfacePresentationLifecycle(
            () => { Assert.Null(active); active = ++nextId; return true; },
            () => { disposed.Add(active!.Value); active = null; });
        lifecycle.Load(false);
        Assert.Null(active);
        lifecycle.Refresh(true);
        Assert.Equal(1, active);
        lifecycle.Refresh(true);
        Assert.Equal(1, nextId);
        lifecycle.Unload();
        lifecycle.Unload();
        lifecycle.Refresh(true);
        Assert.Null(active);
        Assert.Equal(new[] { 1 }, disposed);
        lifecycle.Load(true);
        Assert.Equal(2, active);
        lifecycle.Refresh(false);
        Assert.Null(active);
        Assert.Equal(new[] { 1, 2 }, disposed);
    }

    [Fact]
    public void FailedAttachmentCanRetryWhenSourceBecomesAvailable()
    {
        var attempts = 0;
        var releases = 0;
        var lifecycle = new VideoSurfacePresentationLifecycle(() => ++attempts > 1, () => releases++);
        lifecycle.Load(true);
        lifecycle.Refresh(true);
        lifecycle.Refresh(true);
        Assert.Equal(2, attempts);
        lifecycle.Unload();
        Assert.Equal(1, releases);
    }
}
