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
    [InlineData(" 0 ", false)]
    [InlineData("false", false)]
    [InlineData("FALSE", false)]
    [InlineData("off", false)]
    [InlineData("", true)]      // DEFAULT-ON: unset/blank -> native core capture
    [InlineData(null, true)]    // (opt out with COREVIDEO_NATIVE_UVC=0)
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
        // DEFAULT-ON: unset env behaves like enabled (camera vendors only).
        Assert.Equal(expected, NativeUvcCapturePolicy.ShouldPreferNativeCapture(null, vendor));
        // Explicit opt-out always declines regardless of vendor.
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

    [Fact]
    public void FindDevice_MatchesRegardlessOfConnectionState()
    {
        // Unlike FindConnectedDevice, the first-frame loop must see the device
        // even after the core downgrades it to "error" (the watchdog fired).
        var devices = new[]
        {
            new NativeCaptureDeviceStatus { Id = "cam", ConnectionState = "error", Warning = "no frames" }
        };

        Assert.Null(NativeUvcCapturePolicy.FindConnectedDevice(devices, "cam", null));
        var found = NativeUvcCapturePolicy.FindDevice(devices, "cam", null);
        Assert.NotNull(found);
        Assert.Equal("error", found!.ConnectionState);
    }

    [Fact]
    public void EvaluateFirstFrame_ConfirmsOnceSignalPresent()
    {
        var device = new NativeCaptureDeviceStatus { Id = "cam", ConnectionState = "connected", SignalPresent = true };
        Assert.Equal(
            NativeUvcCapturePolicy.FirstFrameOutcome.Confirmed,
            NativeUvcCapturePolicy.EvaluateFirstFrame(device, elapsedMs: 100));
    }

    [Fact]
    public void EvaluateFirstFrame_WaitsWhileConnectedWithinWindow()
    {
        // connect() reports "connected" the instant the reader starts — before
        // any frame. That is NOT a failure yet; keep polling.
        var device = new NativeCaptureDeviceStatus { Id = "cam", ConnectionState = "connected", SignalPresent = false };
        Assert.Equal(
            NativeUvcCapturePolicy.FirstFrameOutcome.Waiting,
            NativeUvcCapturePolicy.EvaluateFirstFrame(device, elapsedMs: 500));
    }

    [Fact]
    public void EvaluateFirstFrame_FallsBackWhenWatchdogErrorsTheDevice()
    {
        // The native no-first-frame watchdog downgraded "connected" → "error"
        // and released the device: fall back to the bridge immediately, even
        // well inside the wait window.
        var device = new NativeCaptureDeviceStatus { Id = "cam", ConnectionState = "error", SignalPresent = false };
        Assert.Equal(
            NativeUvcCapturePolicy.FirstFrameOutcome.FallBack,
            NativeUvcCapturePolicy.EvaluateFirstFrame(device, elapsedMs: 500));
    }

    [Fact]
    public void EvaluateFirstFrame_FallsBackWhenDeviceDisappears()
    {
        Assert.Equal(
            NativeUvcCapturePolicy.FirstFrameOutcome.FallBack,
            NativeUvcCapturePolicy.EvaluateFirstFrame(null, elapsedMs: 500));
    }

    [Fact]
    public void EvaluateFirstFrame_FallsBackWhenWindowElapsesWithNoFrame()
    {
        // Belt-and-braces: even if the device stays "connected" but never
        // delivers (e.g. the watchdog is disabled), the shell still gives up.
        var device = new NativeCaptureDeviceStatus { Id = "cam", ConnectionState = "connected", SignalPresent = false };
        Assert.Equal(
            NativeUvcCapturePolicy.FirstFrameOutcome.FallBack,
            NativeUvcCapturePolicy.EvaluateFirstFrame(device, NativeUvcCapturePolicy.FirstFrameTimeoutMs));
    }
}
