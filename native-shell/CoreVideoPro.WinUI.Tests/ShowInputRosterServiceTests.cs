using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ShowInputRosterServiceTests
{
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
