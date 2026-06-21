using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class CaptureDeviceDiscoveryServiceTests
{
    [Fact]
    public void SortForOperatorSelection_PrefersPhysicalCaptureBeforeVirtualCamera()
    {
        var sorted = CaptureDeviceDiscoveryService.SortForOperatorSelection(
            [
                Device("virtual", "uvc", "Elgato Virtual Camera"),
                Device("hd60", "uvc", "Game Capture HD60 S+"),
                Device("webcam", "windows", "Logitech BRIO")
            ]);

        Assert.Equal("hd60", sorted[0].Id);
        Assert.Equal("webcam", sorted[1].Id);
        Assert.Equal("virtual", sorted[2].Id);
    }

    private static CaptureDevice Device(string id, string vendor, string name) =>
        new()
        {
            Id = id,
            NativeDeviceId = $"native:{id}",
            Vendor = vendor,
            Name = name,
            Inputs = [new CaptureDeviceInput { Id = "default", Label = "Default" }],
            SelectedInputId = "default"
        };
}
