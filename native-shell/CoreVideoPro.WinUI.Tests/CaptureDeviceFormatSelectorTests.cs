using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class CaptureDeviceFormatSelectorTests
{
    [Fact]
    public void Score_PrefersFullHdSixtyOverLowerResolutionModes()
    {
        var fullHd = new CaptureDeviceFormatCandidate(1920, 1080, 60, "YUY2");
        var lowResolution = new CaptureDeviceFormatCandidate(1280, 720, 60, "ARGB32");

        Assert.True(
            CaptureDeviceFormatSelector.Score(fullHd) > CaptureDeviceFormatSelector.Score(lowResolution));
    }

    [Fact]
    public void Score_PrefersStableUncompressedUvcTransportWhenResolutionAndFpsMatch()
    {
        var compressed = new CaptureDeviceFormatCandidate(1920, 1080, 60, "MJPG");
        var raw = new CaptureDeviceFormatCandidate(1920, 1080, 60, "YUY2");

        Assert.True(
            CaptureDeviceFormatSelector.Score(raw) > CaptureDeviceFormatSelector.Score(compressed));
    }

    [Fact]
    public void Score_PrefersNativeUvcTransportFormatsOverSyntheticBgraWhenResolutionAndFpsMatch()
    {
        var bgra = new CaptureDeviceFormatCandidate(1920, 1080, 60, "ARGB32");
        var nv12 = new CaptureDeviceFormatCandidate(1920, 1080, 60, "NV12");

        Assert.True(
            CaptureDeviceFormatSelector.Score(nv12) > CaptureDeviceFormatSelector.Score(bgra));
    }

    [Fact]
    public void NormalizeSubtype_MapsKnownMediaFoundationGuidFourCcValues()
    {
        Assert.Equal("I420", CaptureDeviceFormatSelector.NormalizeSubtype("{30323449-0000-0010-8000-00AA00389B71}"));
    }

    [Theory]
    [InlineData("Elgato USB-C Capture")]
    [InlineData("Cam Link 4K")]
    [InlineData("USB Video Capture")]
    [InlineData("UVC HDMI")]
    public void AllowsLateFirstFrame_KeepsCaptureCardsOnlineWhileHdmiSignalArrives(string deviceName)
    {
        Assert.True(CaptureDeviceFormatSelector.AllowsLateFirstFrame(deviceName));
    }

    [Fact]
    public void AllowsLateFirstFrame_DoesNotMaskNormalWebcamFormatFallback()
    {
        Assert.False(CaptureDeviceFormatSelector.AllowsLateFirstFrame("Integrated Webcam"));
    }

    [Fact]
    public void Score_PenalizesNonWideAspectModesThatCauseStretchedUvcTiles()
    {
        var wide = new CaptureDeviceFormatCandidate(1920, 1080, 30, "NV12");
        var square = new CaptureDeviceFormatCandidate(1080, 1080, 30, "NV12");

        Assert.True(
            CaptureDeviceFormatSelector.Score(wide) > CaptureDeviceFormatSelector.Score(square));
    }
}
