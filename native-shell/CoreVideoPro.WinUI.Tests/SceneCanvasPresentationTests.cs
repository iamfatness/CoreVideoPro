using CoreVideoPro.WinUI.Models;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class SceneCanvasPresentationTests
{
    [Fact]
    public void OfflineOrThumbnailOnlySurfaceKeepsLayerFallback()
    {
        Assert.False(SceneCanvasPresentationRules.HasComposite(null));
        var waiting = VideoSurfaceState.Waiting(VideoSurfaceKind.Preview, "preview", "Preview");
        Assert.False(SceneCanvasPresentationRules.HasComposite(waiting));
        Assert.False(SceneCanvasPresentationRules.HasComposite(waiting with
        {
            PreviewBgra = new byte[16], PreviewWidth = 2, PreviewHeight = 2
        }));
    }

    [Theory]
    [InlineData("preview", VideoSurfaceKind.Preview, 1920, true)]
    [InlineData("preview", VideoSurfaceKind.Preview, 0, false)]
    [InlineData("program", VideoSurfaceKind.Program, 1920, false)]
    [InlineData("scene-layer-1:camera", VideoSurfaceKind.Multiview, 1920, false)]
    public void OnlyValidPreviewCompositeReplacesLayerFallback(string key, VideoSurfaceKind kind, int width, bool expected)
    {
        var surface = VideoSurfaceState.Waiting(kind, key, "Preview") with
        {
            PendingSharedHandle = new SharedTextureHandle { NtHandle = 0x1234, Width = width, Height = 1080 }
        };
        Assert.Equal(expected, SceneCanvasPresentationRules.HasComposite(surface));
    }

    [Fact]
    public void LosingCompositeRestoresOfflineFallbackWithoutEnablingLayerGpuHosts()
    {
        var composite = VideoSurfaceState.Waiting(VideoSurfaceKind.Preview, "preview", "Preview") with
        {
            PendingSharedHandle = new SharedTextureHandle { NtHandle = 0x1234, Width = 1920, Height = 1080 }
        };
        Assert.True(SceneCanvasPresentationRules.HasComposite(composite));
        Assert.False(SceneCanvasPresentationRules.HasComposite(composite with { PendingSharedHandle = null }));
        var layer = VideoSurfacePresentationRules.ToSceneLayerSurface(composite, 0, "Layer");
        Assert.False(VideoSurfacePresentationRules.UsesGpuSharedTexture(layer.SurfaceKey, layer.Kind));
    }
}
