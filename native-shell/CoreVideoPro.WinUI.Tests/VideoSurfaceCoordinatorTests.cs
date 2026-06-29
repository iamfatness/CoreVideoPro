using System.Threading;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.MediaCore.Models;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class VideoSurfaceCoordinatorTests
{
    [Fact]
    public void CaptureDeviceFrame_PreservesNaturalSourceDimensionsForSceneFraming()
    {
        var coordinator = new VideoSurfaceCoordinator();
        var accepted = coordinator.OnCaptureDeviceFrame(new CaptureDeviceFrame
        {
            DeviceId = "elgato-1",
            Bgra = new byte[640 * 360 * 4],
            Width = 640,
            Height = 360,
            NaturalSourceWidth = 1920,
            NaturalSourceHeight = 1080,
            Fps = 60,
            FrameId = 1
        });

        Assert.True(accepted);
        var surface = Assert.Single(coordinator.CaptureDeviceSurfaces).Value;
        Assert.Equal(640, surface.PreviewWidth);
        Assert.Equal(360, surface.PreviewHeight);
        Assert.Equal(1920, surface.FramingSourceWidth);
        Assert.Equal(1080, surface.FramingSourceHeight);
    }

    [Fact]
    public void CaptureDeviceFrame_FallsBackToPreviewDimensionsWhenNaturalSizeIsMissing()
    {
        var coordinator = new VideoSurfaceCoordinator();
        coordinator.OnCaptureDeviceFrame(new CaptureDeviceFrame
        {
            DeviceId = "uvc-1",
            Bgra = new byte[1280 * 720 * 4],
            Width = 1280,
            Height = 720,
            Fps = 30,
            FrameId = 1
        });

        var surface = Assert.Single(coordinator.CaptureDeviceSurfaces).Value;
        Assert.Equal(1280, surface.FramingSourceWidth);
        Assert.Equal(720, surface.FramingSourceHeight);
    }

    [Fact]
    public void MultiviewSharedTexture_StoresSingleSurfaceAndTileRects_AndFiresOnStructuralChangeOnly()
    {
        var coordinator = new VideoSurfaceCoordinator();
        var changes = 0;
        coordinator.SurfacesChanged += () => Interlocked.Increment(ref changes);

        var first = new MultiviewSharedTexture
        {
            Texture = new ProgramSharedTexture
            {
                SharedHandleHex = "0x1A2B3C",
                Width = 1920,
                Height = 1080,
                FrameNumber = 1
            },
            CanvasWidth = 1920,
            CanvasHeight = 1080,
            Tiles =
            [
                new MultiviewTile { SourceId = "zoom:p-1", ParticipantId = "p-1", Slot = 0, Label = "Host", X = 0, Y = 0, W = 0.5, H = 1 },
                new MultiviewTile { SourceId = "capture:cam-1", Slot = 1, Label = "Cam", X = 0.5, Y = 0, W = 0.5, H = 1 }
            ]
        };

        coordinator.OnMultiviewSharedTexture(first);

        Assert.Equal("multiview", coordinator.MultiviewSurface.SurfaceKey);
        Assert.Equal(VideoSurfaceKind.Multiview, coordinator.MultiviewSurface.Kind);
        Assert.True(coordinator.MultiviewSurface.PendingSharedHandle!.IsValid);
        Assert.Equal(1920, coordinator.MultiviewSurface.PendingSharedHandle!.Width);
        Assert.Equal(2, coordinator.MultiviewTileRects.Count);
        Assert.Equal(1, changes);

        // A new frame with the SAME handle + layout is NOT a structural change (the host's own
        // present loop shows new pixels) — no extra SurfacesChanged.
        coordinator.OnMultiviewSharedTexture(first);
        var sameFrame = new MultiviewSharedTexture
        {
            Texture = new ProgramSharedTexture { SharedHandleHex = "0x1A2B3C", Width = 1920, Height = 1080, FrameNumber = 2 },
            CanvasWidth = 1920,
            CanvasHeight = 1080,
            Tiles = first.Tiles
        };
        coordinator.OnMultiviewSharedTexture(sameFrame);
        Assert.Equal(1, changes);

        // A new tile layout IS structural.
        var relayout = new MultiviewSharedTexture
        {
            Texture = new ProgramSharedTexture { SharedHandleHex = "0x1A2B3C", Width = 1920, Height = 1080, FrameNumber = 3 },
            CanvasWidth = 1920,
            CanvasHeight = 1080,
            Tiles = [new MultiviewTile { SourceId = "zoom:p-1", ParticipantId = "p-1", Slot = 0, Label = "Host", X = 0, Y = 0, W = 1, H = 1 }]
        };
        coordinator.OnMultiviewSharedTexture(relayout);
        Assert.Equal(2, changes);
        Assert.Single(coordinator.MultiviewTileRects);
    }

    [Fact]
    public void MultiviewSharedTexture_IgnoresInvalidHandle()
    {
        var coordinator = new VideoSurfaceCoordinator();
        var changes = 0;
        coordinator.SurfacesChanged += () => Interlocked.Increment(ref changes);

        coordinator.OnMultiviewSharedTexture(new MultiviewSharedTexture
        {
            Texture = new ProgramSharedTexture { SharedHandleHex = "0x0", Width = 1920, Height = 1080 },
            CanvasWidth = 1920,
            CanvasHeight = 1080
        });

        Assert.Equal(0, changes);
        Assert.Null(coordinator.MultiviewSurface.PendingSharedHandle);
    }

    [Fact]
    public void ZoomParticipantFrame_PreservesRawFrameDimensionsForSceneFraming()
    {
        var coordinator = new VideoSurfaceCoordinator();
        coordinator.SetZoomCaptureSubscribed(true);

        coordinator.OnZoomVideoFrame(new ZoomVideoFrame
        {
            ParticipantId = "p1",
            Bgra = new byte[960 * 540 * 4],
            Width = 960,
            Height = 540,
            FrameId = 1
        });

        var tile = Assert.Single(coordinator.BuildMultiviewTiles(
            [new Participant { Id = "p1", Name = "Guest" }]));
        Assert.Equal(960, tile.Surface.PreviewWidth);
        Assert.Equal(540, tile.Surface.PreviewHeight);
        Assert.Equal(960, tile.Surface.FramingSourceWidth);
        Assert.Equal(540, tile.Surface.FramingSourceHeight);
    }
}
