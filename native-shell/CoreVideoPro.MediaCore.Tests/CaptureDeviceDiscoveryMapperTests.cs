using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class CaptureDeviceDiscoveryMapperTests
{
    [Fact]
    public void CreateStableDeviceId_IsDeterministicForSymbolicLink()
    {
        var first = CaptureDeviceDiscoveryMapper.CreateStableDeviceId(@"\\?\usb#vid_046d&pid_082d");
        var second = CaptureDeviceDiscoveryMapper.CreateStableDeviceId(@"\\?\usb#vid_046d&pid_082d");

        Assert.Equal(first, second);
        Assert.Equal(16, first.Length);
    }

    [Theory]
    [InlineData("DeckLink Mini Recorder 4K", "blackmagic")]
    [InlineData("Blackmagic Web Presenter HD", "blackmagic")]
    [InlineData("AJA Io 4K Plus", "aja")]
    [InlineData("Elgato Cam Link 4K", "uvc")]
    [InlineData("Game Capture HD60 S+", "uvc")]
    [InlineData("USB Capture HDMI", "uvc")]
    [InlineData("Logitech BRIO", "windows")]
    public void DetectVendor_ClassifiesFriendlyNames(string friendlyName, string expectedVendor)
    {
        Assert.Equal(expectedVendor, CaptureDeviceDiscoveryMapper.DetectVendor(friendlyName));
    }

    [Theory]
    [InlineData("Elgato Virtual Camera", true)]
    [InlineData("OBS Virtual Camera", true)]
    [InlineData("Game Capture HD60 S+", false)]
    [InlineData("Cam Link 4K", false)]
    public void IsVirtualCameraName_DetectsSoftwareCameraSources(string friendlyName, bool expected)
    {
        Assert.Equal(expected, CaptureDeviceDiscoveryMapper.IsVirtualCameraName(friendlyName));
    }

    [Fact]
    public void OperatorSelectionPriority_PrefersPhysicalCaptureBeforeVirtualCamera()
    {
        var physicalPriority = CaptureDeviceDiscoveryMapper.OperatorSelectionPriority(
            "Game Capture HD60 S+",
            CaptureDeviceDiscoveryMapper.DetectVendor("Game Capture HD60 S+"));
        var virtualPriority = CaptureDeviceDiscoveryMapper.OperatorSelectionPriority(
            "Elgato Virtual Camera",
            CaptureDeviceDiscoveryMapper.DetectVendor("Elgato Virtual Camera"));

        Assert.True(physicalPriority < virtualPriority);
    }
}
