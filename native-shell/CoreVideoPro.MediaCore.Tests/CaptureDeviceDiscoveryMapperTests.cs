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
    [InlineData("test-device", "2fe90d9c33ad85a1")]
    [InlineData(
        @"\\?\usb#vid_046d&pid_085e&mi_00#6&158f56b0&0&0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\global",
        "98a3916224275371")]
    public void CreateStableDeviceId_MatchesNativeCoreSha256Convention(string symbolicLink, string expected)
    {
        // These vectors are mirrored in the native core's UvcCaptureSupportTest
        // (stableCaptureDeviceIdFromSymbolicLink). The shell and the C++ UVC
        // adapter MUST derive the identical stable id from the same symbolic
        // link so scene routes resolve to the same "capture:<id>" source on
        // both capture paths.
        Assert.Equal(expected, CaptureDeviceDiscoveryMapper.CreateStableDeviceId(symbolicLink));
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
