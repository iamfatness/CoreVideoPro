using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
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
}
