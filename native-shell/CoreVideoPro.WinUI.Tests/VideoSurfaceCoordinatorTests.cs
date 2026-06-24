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
