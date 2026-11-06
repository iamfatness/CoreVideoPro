using CoreVideoPro.WinUI.Models;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class MultiviewEmptyStatePolicyTests
{
    [Fact]
    public void EmptySlateShowsAssignmentButPopulatedSlateShowsPreparation()
    {
        var slate = VideoSurfaceState.Slate(VideoSurfaceKind.Multiview, "multiview", "Multiview");
        Assert.True(MultiviewEmptyStatePolicy.ShowAssignmentGuidance(0, null));
        Assert.True(MultiviewEmptyStatePolicy.ShowAssignmentGuidance(0, slate));
        Assert.False(MultiviewEmptyStatePolicy.ShowAssignmentGuidance(2, slate));
    }

    [Fact]
    public void ErrorOrUnknownStatusRemainsVisibleWithoutTiles()
    {
        var slate = VideoSurfaceState.Slate(VideoSurfaceKind.Multiview, "multiview", "Multiview");
        Assert.False(MultiviewEmptyStatePolicy.ShowAssignmentGuidance(0, slate with { StatusLine = "Renderer failed" }));
        Assert.False(MultiviewEmptyStatePolicy.ShowAssignmentGuidance(0, slate with { StatusLine = "Capture paused" }));
    }

    [Fact]
    public void AvailablePictureIsNeverCoveredByAssignmentGuidance()
    {
        var picture = VideoSurfaceState.Slate(VideoSurfaceKind.Multiview, "multiview", "Multiview")
            .WithPreviewPixels(new byte[16], 2, 2);
        Assert.False(MultiviewEmptyStatePolicy.ShowAssignmentGuidance(0, picture));
    }

    [Fact]
    public void PublishedFrameOrPendingGpuHandleKeepsTheSurfaceVisible()
    {
        var slate = VideoSurfaceState.Slate(VideoSurfaceKind.Multiview, "multiview", "Multiview");
        Assert.False(MultiviewEmptyStatePolicy.ShowAssignmentGuidance(0,
            slate.WithFrame(new VideoFrameMetadata { FrameId = 1 }, "Live")));
        Assert.False(MultiviewEmptyStatePolicy.ShowAssignmentGuidance(0,
            slate.WithSharedHandle(new SharedTextureHandle { NtHandle = 1, Width = 1920, Height = 1080 })));
    }
}
