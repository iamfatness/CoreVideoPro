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
    [InlineData("Logitech BRIO", "windows")]
    public void DetectVendor_ClassifiesFriendlyNames(string friendlyName, string expectedVendor)
    {
        Assert.Equal(expectedVendor, CaptureDeviceDiscoveryMapper.DetectVendor(friendlyName));
    }
}