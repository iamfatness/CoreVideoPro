using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// WHY THE SCENE CANVAS EDITOR SHOWS LIVE VIDEO "INCONSISTENTLY" (investigated 2026-08-15).
///
/// The core exports a per-source keyed-mutex GPU texture for EVERY source with content
/// (<c>D3D11CompositorAdapter::exportParticipantTextures</c>) precisely so an intermittent
/// consumer — "the preview host, when it shows this source" — can present real pixels.
/// The shell receives it as <c>participant-shared-texture</c> and parks it on the
/// participant surface (<c>VideoSurfaceCoordinator.OnParticipantSharedTexture</c>).
///
/// <c>VideoSurfaceHost</c> only ATTACHES a SwapChainPanel (and only then hooks its
/// per-vsync present loop) when <see cref="VideoSurfacePresentationRules.UsesGpuSharedTexture"/>
/// is true — i.e. for the keys "program"/"preview"/"multiview" or kinds Program/Preview.
///
/// The PREVIEW monitor gets live video off that very tile because
/// <c>StudioViewModel.ResolvePreviewPrimarySurface</c> rewrites the tile surface to
/// key "preview" / kind Preview. The scene canvas editor rewrites the SAME tile surface to
/// key "scene-layer-N:&lt;tileKey&gt;" / kind Multiview
/// (<c>StudioViewModel.ResolveLayerSurface</c>) — which matches NO clause, so the layer's
/// host never attaches a swap chain and the GPU handle is silently dropped. Those layers
/// can only ever render CPU BGRA pixels, which exist for some source classes and not others.
///
/// These are CHARACTERIZATION tests of the defect, not of the desired behaviour.
/// </summary>
public sealed class SceneCanvasLayerSurfaceTests
{
    private static ParticipantSharedTexture GpuTexture(string participantId) =>
        new()
        {
            ParticipantId = participantId,
            SharedHandleHex = "0x1A2B3C4D",
            Width = 1920,
            Height = 1080,
            Format = "B8G8R8A8_UNORM",
            FrameNumber = 42
        };

    [Fact]
    public void SceneCanvasLayer_DropsTheCoreExportedGpuTexture_SoTheLayerHasNoPixelsAtAll()
    {
        // The core published a per-source GPU texture for a Zoom guest and NO CPU
        // thumbnail has landed yet (thumbnails are ~2/s: kThumbnailEmitIntervalMs = 500).
        var coordinator = new VideoSurfaceCoordinator();
        coordinator.OnParticipantSharedTexture(GpuTexture("p1"));

        var tile = Assert.Single(coordinator.BuildMultiviewTiles([new Participant { Id = "p1", Name = "Guest" }]));

        // The tile really does carry a presentable GPU handle...
        Assert.NotNull(tile.Surface.PendingSharedHandle);
        Assert.True(tile.Surface.PendingSharedHandle!.IsValid);

        var layerSurface = VideoSurfacePresentationRules.ToSceneLayerSurface(tile.Surface, layerIndex: 0, "1. Guest");

        // ...and the scene-layer rewrite preserves it...
        Assert.True(layerSurface.PendingSharedHandle!.IsValid);

        // ...but the layer's VideoSurfaceHost will never attach a swap chain for it,
        // and there are no CPU pixels either => the operator sees the placeholder.
        // PROOF OF THE DEFECT: flipping this to Assert.True (the DESIRED behaviour —
        // the layer should present the handle it is holding) fails against current code.
        Assert.False(VideoSurfacePresentationRules.UsesGpuSharedTexture(layerSurface.SurfaceKey, layerSurface.Kind));
        Assert.False(layerSurface.HasPreviewBitmap);
    }

    [Fact]
    public void PreviewMonitorRewriteOfTheSameTile_DoesPresentTheGpuTexture()
    {
        // The A/B: StudioViewModel.ResolvePreviewPrimarySurface rewrites the identical
        // tile surface to key "preview"/kind Preview, which DOES take the GPU path.
        var coordinator = new VideoSurfaceCoordinator();
        coordinator.OnParticipantSharedTexture(GpuTexture("p1"));
        var tile = Assert.Single(coordinator.BuildMultiviewTiles([new Participant { Id = "p1", Name = "Guest" }]));

        var previewSurface = tile.Surface with
        {
            SurfaceKey = VideoSurfacePresentationRules.PreviewSurfaceKey,
            Kind = VideoSurfaceKind.Preview,
            Title = "Preview"
        };

        Assert.True(VideoSurfacePresentationRules.UsesGpuSharedTexture(previewSurface.SurfaceKey, previewSurface.Kind));
        Assert.True(previewSurface.PendingSharedHandle!.IsValid);
    }

    [Fact]
    public void SceneCanvasLayer_ShowsLiveVideoOnlyWhenTheSurfaceCarriesCpuBgraPixels()
    {
        // The one path that DOES work for a Zoom guest: the 640x360 CPU thumbnail the
        // engine emits at ~2/s. That is the entire reason some layers look "live".
        var coordinator = new VideoSurfaceCoordinator();
        coordinator.SetZoomCaptureSubscribed(true);
        coordinator.OnZoomVideoFrame(new ZoomVideoFrame
        {
            ParticipantId = "p1",
            Bgra = new byte[640 * 360 * 4],
            Width = 640,
            Height = 360,
            FrameId = 1
        });

        var tile = Assert.Single(coordinator.BuildMultiviewTiles([new Participant { Id = "p1", Name = "Guest" }]));
        var layerSurface = VideoSurfacePresentationRules.ToSceneLayerSurface(tile.Surface, layerIndex: 0, "1. Guest");

        Assert.True(layerSurface.HasPreviewBitmap);
        Assert.False(VideoSurfacePresentationRules.UsesGpuSharedTexture(layerSurface.SurfaceKey, layerSurface.Kind));
    }

    [Fact]
    public void SceneCanvasLayer_LosesEveryZoomPixelWhenCaptureIsUnsubscribed()
    {
        // SetZoomCaptureSubscribed(false) CLEARS _participantSurfaces, so every Zoom
        // layer in the editor reverts to the placeholder even though the core keeps
        // compositing. Another face of the "sometimes live, sometimes not" report.
        var coordinator = new VideoSurfaceCoordinator();
        coordinator.SetZoomCaptureSubscribed(true);
        coordinator.OnZoomVideoFrame(new ZoomVideoFrame
        {
            ParticipantId = "p1",
            Bgra = new byte[640 * 360 * 4],
            Width = 640,
            Height = 360,
            FrameId = 1
        });
        coordinator.SetZoomCaptureSubscribed(false);

        var tile = Assert.Single(coordinator.BuildMultiviewTiles([new Participant { Id = "p1", Name = "Guest" }]));
        var layerSurface = VideoSurfacePresentationRules.ToSceneLayerSurface(tile.Surface, layerIndex: 0, "1. Guest");

        Assert.False(layerSurface.HasPreviewBitmap);
        Assert.False(VideoSurfacePresentationRules.UsesGpuSharedTexture(layerSurface.SurfaceKey, layerSurface.Kind));
    }

    [Fact]
    public void CoreOnlyCaptureSources_NeverReachTheEditorsCpuPreviewMap()
    {
        // Screen (WGC), browser hosts and SRT ingest are decoded IN THE CORE; only the
        // managed MediaCapture bridge (CaptureDeviceFrameReaderService ->
        // CaptureDeviceFrameRouter.Publish) ever fills CaptureDeviceSurfaces. So a scene
        // layer bound to one of those devices has neither CPU pixels nor a GPU path.
        var coordinator = new VideoSurfaceCoordinator();
        coordinator.OnCaptureDeviceFrame(new CaptureDeviceFrame
        {
            DeviceId = "uvc-1",
            Bgra = new byte[640 * 360 * 4],
            Width = 640,
            Height = 360,
            FrameId = 1
        });

        Assert.True(coordinator.CaptureDeviceSurfaces.ContainsKey("uvc-1"));
        Assert.False(coordinator.CaptureDeviceSurfaces.ContainsKey("browser:1"));
        Assert.False(coordinator.CaptureDeviceSurfaces.ContainsKey("srt-ingest-1"));
    }

    [Fact]
    public void SceneLayerKeys_AreNeverOneOfTheThreeGpuSurfaceKeys()
    {
        for (var index = 0; index < 8; index++)
        {
            Assert.False(VideoSurfacePresentationRules.UsesGpuSharedTexture(
                VideoSurfacePresentationRules.SceneLayerPlaceholderKey(index),
                VideoSurfaceKind.Multiview));
            Assert.False(VideoSurfacePresentationRules.UsesGpuSharedTexture(
                VideoSurfacePresentationRules.SceneLayerSurfaceKey(index, "multiview"),
                VideoSurfaceKind.Multiview));
        }

        // The three surfaces that legitimately own a stable swap chain still do.
        Assert.True(VideoSurfacePresentationRules.UsesGpuSharedTexture("program", VideoSurfaceKind.Program));
        Assert.True(VideoSurfacePresentationRules.UsesGpuSharedTexture("preview", VideoSurfaceKind.Preview));
        Assert.True(VideoSurfacePresentationRules.UsesGpuSharedTexture("multiview", VideoSurfaceKind.Multiview));
    }
}
