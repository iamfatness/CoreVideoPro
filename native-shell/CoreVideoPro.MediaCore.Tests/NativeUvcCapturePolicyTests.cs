using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class NativeUvcCapturePolicyTests
{
    [Theory]
    [InlineData("1", true)]
    [InlineData("true", true)]
    [InlineData("TRUE", true)]
    [InlineData("on", true)]
    [InlineData(" 1 ", true)]
    [InlineData("0", false)]
    [InlineData("false", false)]
    [InlineData("off", false)]
    [InlineData("", false)]     // opt-in: unset -> legacy WinUI bridge
    [InlineData(null, false)]
    public void IsEnabled_ParsesEnvironmentValues(string? value, bool expected)
    {
        Assert.Equal(expected, NativeUvcCapturePolicy.IsEnabled(value));
    }

    [Theory]
    [InlineData("uvc", true)]
    [InlineData("windows", true)]
    [InlineData("UVC", true)]
    [InlineData("blackmagic", false)]
    [InlineData("aja", false)]
    [InlineData("srt", false)]
    public void ShouldPreferNativeCapture_GatesOnCameraVendors(string vendor, bool expected)
    {
        Assert.Equal(expected, NativeUvcCapturePolicy.ShouldPreferNativeCapture("1", vendor));
        // Opt-in: unset/disabled env always declines regardless of vendor.
        Assert.False(NativeUvcCapturePolicy.ShouldPreferNativeCapture(null, vendor));
        Assert.False(NativeUvcCapturePolicy.ShouldPreferNativeCapture("0", vendor));
    }

    [Fact]
    public void FindConnectedDevice_MatchesStableIdOnlyWhenConnected()
    {
        var devices = new[]
        {
            new NativeCaptureDeviceStatus { Id = "aaaa", ConnectionState = "detected" },
            new NativeCaptureDeviceStatus { Id = "bbbb", ConnectionState = "connected", Width = 1920, Height = 1080 }
        };

        Assert.Null(NativeUvcCapturePolicy.FindConnectedDevice(devices, "aaaa", null));
        var match = NativeUvcCapturePolicy.FindConnectedDevice(devices, "bbbb", null);
        Assert.NotNull(match);
        Assert.Equal(1920, match!.Width);
    }

    [Fact]
    public void FindConnectedDevice_FallsBackToCaseInsensitiveSymbolicLink()
    {
        // WinRT and Media Foundation report the same device interface with
        // different casing; the hashed stable ids then disagree, but the OS
        // identity still correlates the device.
        var devices = new[]
        {
            new NativeCaptureDeviceStatus
            {
                Id = "core-hash",
                ConnectionState = "connected",
                NativeDeviceId = @"\\?\usb#vid_046d&pid_085e#{guid}\global"
            }
        };

        var match = NativeUvcCapturePolicy.FindConnectedDevice(
            devices,
            "shell-hash",
            @"\\?\USB#VID_046D&PID_085E#{GUID}\GLOBAL");
        Assert.NotNull(match);
        Assert.Equal("core-hash", match!.Id);

        Assert.Null(NativeUvcCapturePolicy.FindConnectedDevice(
            devices, "shell-hash", @"\\?\usb#vid_9999&pid_0000#{guid}\global"));
    }

    [Fact]
    public void TryParseCaptureDevices_ParsesCoreResponse()
    {
        const string json = """
        {
          "type": "capture-devices",
          "devices": [
            {
              "id": "98a3916224275371",
              "vendor": "uvc",
              "name": "Logitech BRIO",
              "inputs": [{ "id": "camera", "label": "Camera", "hasEmbeddedAudio": false }],
              "selectedInputId": "camera",
              "resolution": { "width": 1920, "height": 1080 },
              "frameRate": 60,
              "connectionState": "connected",
              "signalPresent": true,
              "droppedFrames": 0,
              "audioSyncOffsetMs": 0,
              "nativeDeviceId": "\\\\?\\usb#vid_046d&pid_085e#{guid}\\global"
            },
            {
              "id": "decklink-1",
              "vendor": "blackmagic",
              "name": "DeckLink Mini Recorder 4K",
              "inputs": [],
              "selectedInputId": "sdi-1",
              "resolution": { "width": 1920, "height": 1080 },
              "frameRate": 60,
              "connectionState": "detected",
              "signalPresent": false,
              "droppedFrames": 0,
              "audioSyncOffsetMs": 0,
              "warning": "Connect the input."
            }
          ],
          "id": "capture-1",
          "ok": true
        }
        """;

        using var response = JsonDocument.Parse(json);
        var devices = CoreProtocolParser.TryParseCaptureDevices(response);

        Assert.NotNull(devices);
        Assert.Equal(2, devices!.Count);

        var uvc = devices[0];
        Assert.Equal("98a3916224275371", uvc.Id);
        Assert.Equal("uvc", uvc.Vendor);
        Assert.Equal("Logitech BRIO", uvc.Name);
        Assert.Equal("connected", uvc.ConnectionState);
        Assert.True(uvc.SignalPresent);
        Assert.Equal(1920, uvc.Width);
        Assert.Equal(1080, uvc.Height);
        Assert.Equal(60, uvc.FrameRate);
        Assert.Null(uvc.Warning);
        Assert.Equal(@"\\?\usb#vid_046d&pid_085e#{guid}\global", uvc.NativeDeviceId);

        var deckLink = devices[1];
        Assert.Equal("Connect the input.", deckLink.Warning);
        Assert.Null(deckLink.NativeDeviceId);
        Assert.False(deckLink.SignalPresent);
    }

    [Theory]
    [InlineData("""{ "type": "ping", "id": "x", "ok": true }""")]
    [InlineData("""{ "type": "capture-devices", "id": "x", "ok": false, "devices": [] }""")]
    [InlineData("""{ "type": "capture-devices", "id": "x", "ok": true }""")]
    public void TryParseCaptureDevices_RejectsNonCaptureResponses(string json)
    {
        using var response = JsonDocument.Parse(json);
        Assert.Null(CoreProtocolParser.TryParseCaptureDevices(response));
    }
}
