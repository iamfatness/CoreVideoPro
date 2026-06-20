using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ShowInputSlotViewModelTests
{
    [Fact]
    public void ChangingKindRefreshesSourceOptionsForSelectedInputType()
    {
        var slot = new ShowInputSlot { SlotNumber = 1 };
        var editor = new ShowInputSlotViewModel(slot, () => { });
        var devices = new[]
        {
            Device("cam-uvc", "USB Capture", "uvc"),
            Device("srt-ingest-01", "SRT 1", "srt"),
            Device("decklink", "DeckLink", "blackmagic")
        };

        editor.RefreshSourceOptions([], devices);

        editor.Kind = ShowInputKind.UvcWebcam;
        Assert.Equal(["cam-uvc"], editor.SourceOptions.Select(option => option.Value));
        Assert.Equal("cam-uvc", editor.SelectedSourceId);

        editor.Kind = ShowInputKind.SrtIngest;
        Assert.Equal(["srt-ingest-01"], editor.SourceOptions.Select(option => option.Value));
        Assert.Equal("srt-ingest-01", editor.SelectedSourceId);
    }

    private static CaptureDevice Device(string id, string name, string vendor) =>
        new()
        {
            Id = id,
            NativeDeviceId = $"native-{id}",
            Name = name,
            Vendor = vendor,
            Inputs = [new CaptureDeviceInput { Id = "input-1", Label = "Input 1" }],
            SelectedInputId = "input-1",
            Width = 1920,
            Height = 1080,
            FrameRate = 60,
            ConnectionState = CaptureConnectionState.Detected,
            SignalPresent = false
        };
}
