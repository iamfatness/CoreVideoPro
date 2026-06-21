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
    public void ShouldKeepReaderOnlineAfterFirstFrameTimeout_OnlyKeepsFinalCaptureCardFallback()
    {
        Assert.False(CaptureDeviceFormatSelector.ShouldKeepReaderOnlineAfterFirstFrameTimeout(
            allowsLateFirstFrame: true,
            candidateIndex: 0,
            candidateCount: 3));
        Assert.False(CaptureDeviceFormatSelector.ShouldKeepReaderOnlineAfterFirstFrameTimeout(
            allowsLateFirstFrame: false,
            candidateIndex: 2,
            candidateCount: 3));
        Assert.True(CaptureDeviceFormatSelector.ShouldKeepReaderOnlineAfterFirstFrameTimeout(
            allowsLateFirstFrame: true,
            candidateIndex: 2,
            candidateCount: 3));
    }

    [Fact]
    public void ShouldPreferRankedFormatsBeforeCurrent_OnlyForCaptureCardsAndUvc()
    {
        Assert.True(CaptureDeviceFormatSelector.ShouldPreferRankedFormatsBeforeCurrent(allowsLateFirstFrame: true));
        Assert.False(CaptureDeviceFormatSelector.ShouldPreferRankedFormatsBeforeCurrent(
            allowsLateFirstFrame: false,
            currentSubtype: "NV12",
            bestRankedSubtype: "YUY2"));
    }

    [Fact]
    public void ShouldPreferRankedFormatsBeforeCurrent_WhenCurrentFormatIsSyntheticBgra()
    {
        Assert.True(CaptureDeviceFormatSelector.ShouldPreferRankedFormatsBeforeCurrent(
            allowsLateFirstFrame: false,
            currentSubtype: "ARGB32",
            bestRankedSubtype: "YUY2"));
    }

    [Fact]
    public void Score_PenalizesNonWideAspectModesThatCauseStretchedUvcTiles()
    {
        var wide = new CaptureDeviceFormatCandidate(1920, 1080, 30, "NV12");
        var square = new CaptureDeviceFormatCandidate(1080, 1080, 30, "NV12");

        Assert.True(
            CaptureDeviceFormatSelector.Score(wide) > CaptureDeviceFormatSelector.Score(square));
    }

    [Fact]
    public void Score_PrefersFullHdCaptureModeOverUnstableFourKStartupMode()
    {
        var fullHd = new CaptureDeviceFormatCandidate(1920, 1080, 60, "YUY2");
        var fourK = new CaptureDeviceFormatCandidate(3840, 2160, 30, "MJPG");

        Assert.True(
            CaptureDeviceFormatSelector.Score(fullHd) > CaptureDeviceFormatSelector.Score(fourK));
    }
}
