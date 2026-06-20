using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ShowInputRosterServiceTests
{
    [Fact]
    public void CaptureDeviceFormatLabel_StaysPendingUntilRealFramesArrive()
    {
        var device = Device("cam-uvc", "USB Capture", "uvc", 0, 0, 0);

        Assert.Equal("Format pending", device.ResolutionLabel);
        Assert.Equal("Format pending", device.FormatLabel);
        Assert.False(device.SignalPresent);
        Assert.Equal(CaptureConnectionState.Detected, device.ConnectionState);

        device.ApplyFormatTelemetry(1920, 1080, 60);

        Assert.Equal("1920x1080", device.ResolutionLabel);
        Assert.Equal("1920x1080 · 60 fps", device.FormatLabel);
        Assert.False(device.SignalPresent);
        Assert.Equal(CaptureConnectionState.Detected, device.ConnectionState);

        device.ApplyFrameTelemetry(1920, 1080, 30);

        Assert.Equal("1920x1080", device.ResolutionLabel);
        Assert.Equal("1920x1080 · 60 fps", device.FormatLabel);
        Assert.True(device.SignalPresent);
        Assert.Equal(CaptureConnectionState.Connected, device.ConnectionState);
    }

    [Fact]
    public void BuildSourceOptions_IncludesWindowsAndUvcWebcams()
    {
        var options = ShowInputRosterService.BuildSourceOptions(
            ShowInputKind.UvcWebcam,
            [],
            [
                Device("cam-windows", "Integrated Camera", "windows", 1920, 1080, 30),
                Device("cam-uvc", "USB Capture", "uvc", 1280, 720, 60),
                Device("decklink", "DeckLink Mini", "blackmagic", 1920, 1080, 60)
            ]);

        Assert.Equal(["cam-windows", "cam-uvc"], options.Select(option => option.Value));
    }

    [Fact]
    public void BuildCaptureSourceOptions_IncludesFormatAndConnectionState()
    {
        var options = ShowInputRosterService.BuildCaptureSourceOptions(
            [
                Device("cam-uvc", "USB Capture", "uvc", 1920, 1080, 60, connected: true),
                Device("cam-windows", "Integrated Camera", "windows", 1280, 720, 30)
            ]);

        Assert.Equal(string.Empty, options[0].Value);
        Assert.Equal("Choose capture source", options[0].Label);
        Assert.Equal("cam-uvc", options[1].Value);
        Assert.Contains("USB Capture", options[1].Label, StringComparison.Ordinal);
        Assert.Contains("1920x1080", options[1].Label, StringComparison.Ordinal);
        Assert.Contains("60 fps", options[1].Label, StringComparison.Ordinal);
        Assert.Contains("connected", options[1].Label, StringComparison.Ordinal);
        Assert.Equal("cam-windows", options[2].Value);
    }

    [Fact]
    public void BuildMultiviewTiles_UsesSelectedCaptureDeviceForUvcSlot()
    {
        var slots = new[]
        {
            new ShowInputSlot
            {
                SlotNumber = 1,
                Kind = ShowInputKind.UvcWebcam,
                CaptureDeviceId = "cam-uvc",
                InShow = true
            }
        };
        var devices = new[]
        {
            Device("cam-uvc", "USB Capture", "uvc", 1920, 1080, 60, connected: true)
        };

        var tiles = ShowInputRosterService.BuildMultiviewTiles(slots, [], devices, []);

        var tile = Assert.Single(tiles);
        Assert.Equal("capture:cam-uvc", tile.Participant.Id);
        Assert.StartsWith("USB Capture", tile.Participant.Name, StringComparison.Ordinal);
        Assert.Contains("1920x1080", tile.Participant.Name, StringComparison.Ordinal);
        Assert.Equal("UVC webcam", tile.Participant.Title);
        Assert.Equal(1, tile.SourceIndex);
        Assert.Equal("capture:cam-uvc", tile.Surface.SurfaceKey);
        Assert.Contains("connected", tile.Surface.DetailLine, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("signal present", tile.Surface.DetailLine, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void BuildMultiviewTiles_UsesLiveCaptureSurfaceWhenFramesArrive()
    {
        var slots = new[]
        {
            new ShowInputSlot
            {
                SlotNumber = 1,
                Kind = ShowInputKind.UvcWebcam,
                CaptureDeviceId = "cam-uvc",
                InShow = true
            }
        };
        var devices = new[]
        {
            Device("cam-uvc", "USB Capture", "uvc", 1920, 1080, 60, connected: true)
        };
        var surface = VideoSurfaceState
            .Waiting(VideoSurfaceKind.Multiview, "capture:cam-uvc", "USB Capture")
            .WithPreviewPixels([0, 0, 0, 255], 1, 1);

        var tiles = ShowInputRosterService.BuildMultiviewTiles(
            slots,
            [],
            devices,
            [],
            new Dictionary<string, VideoSurfaceState>(StringComparer.Ordinal)
            {
                ["cam-uvc"] = surface
            });

        var tile = Assert.Single(tiles);
        Assert.True(tile.Surface.HasPreviewBitmap);
        Assert.Equal("capture:cam-uvc", tile.Surface.SurfaceKey);
        Assert.Equal("USB Capture - 1920x1080", tile.Surface.Title);
    }

    [Fact]
    public void SameMultiviewTileStructure_MatchesParticipantAndSlotOrderOnly()
    {
        var first = ShowInputRosterService.BuildMultiviewTiles(
            [new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.UvcWebcam, CaptureDeviceId = "cam-uvc", InShow = true }],
            [],
            [Device("cam-uvc", "USB Capture", "uvc", 1280, 720, 60, connected: true)],
            []);

        var second = ShowInputRosterService.BuildMultiviewTiles(
            [new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.UvcWebcam, CaptureDeviceId = "cam-uvc", InShow = true }],
            [],
            [Device("cam-uvc", "USB Capture", "uvc", 1920, 1080, 60, connected: false)],
            []);

        Assert.True(ShowInputRosterService.SameMultiviewTileStructure(first, second));
        Assert.NotEqual(first[0].Surface.DetailLine, second[0].Surface.DetailLine);
    }

    [Fact]
    public void SameMultiviewTileStructure_ReturnsFalseWhenRosterChanges()
    {
        var first = ShowInputRosterService.BuildMultiviewTiles(
            [new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.UvcWebcam, CaptureDeviceId = "cam-uvc", InShow = true }],
            [],
            [Device("cam-uvc", "USB Capture", "uvc", 1280, 720, 60)],
            []);

        var second = ShowInputRosterService.BuildMultiviewTiles(
            [new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.UvcWebcam, CaptureDeviceId = "cam-windows", InShow = true }],
            [],
            [
                Device("cam-uvc", "USB Capture", "uvc", 1280, 720, 60),
                Device("cam-windows", "Integrated Camera", "windows", 1920, 1080, 30)
            ],
            []);

        Assert.False(ShowInputRosterService.SameMultiviewTileStructure(first, second));
    }

    private static CaptureDevice Device(
        string id,
        string name,
        string vendor,
        int width,
        int height,
        int frameRate,
        bool connected = false) =>
        new()
        {
            Id = id,
            NativeDeviceId = $"native-{id}",
            Name = name,
            Vendor = vendor,
            Inputs = [new CaptureDeviceInput { Id = "input-1", Label = "Input 1" }],
            SelectedInputId = "input-1",
            Width = width,
            Height = height,
            FrameRate = frameRate,
            ConnectionState = connected ? CaptureConnectionState.Connected : CaptureConnectionState.Detected,
            SignalPresent = connected
        };
}
