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

    [Fact]
    public void DisplayName_DefaultsToDerivedNameAndPersistsOverrideKeyedByCanonicalSourceId()
    {
        var overrides = new Dictionary<string, string>(StringComparer.Ordinal);
        var slot = new ShowInputSlot { SlotNumber = 1 };
        var editor = new ShowInputSlotViewModel(
            slot,
            () => { },
            resolveDisplayName: (sourceId, derived) =>
                sourceId is not null && overrides.TryGetValue(sourceId, out var name) ? name : derived,
            setDisplayName: (sourceId, name) =>
            {
                if (sourceId is null)
                {
                    return;
                }

                if (string.IsNullOrWhiteSpace(name))
                {
                    overrides.Remove(sourceId);
                }
                else
                {
                    overrides[sourceId] = name;
                }
            });

        editor.Kind = ShowInputKind.UvcWebcam;
        editor.RefreshSourceOptions([], [Device("cam-uvc", "USB Capture", "uvc")]);

        // Defaults to the derived device name.
        Assert.Equal("capture:cam-uvc", editor.SourceId);
        Assert.Equal("USB Capture", editor.DisplayName);
        Assert.True(editor.IsDisplayNameEditable);

        // Setting a name stores an override keyed by the canonical source id.
        editor.DisplayName = "Main Camera";
        Assert.Equal("Main Camera", overrides["capture:cam-uvc"]);
        Assert.Equal("Main Camera", editor.DisplayName);

        // Clearing it (or matching the derived name) resets to the derived name.
        editor.DisplayName = "  ";
        Assert.False(overrides.ContainsKey("capture:cam-uvc"));
        Assert.Equal("USB Capture", editor.DisplayName);
    }

    [Fact]
    public void DisplayName_IsNotEditableWhenUnassigned()
    {
        var editor = new ShowInputSlotViewModel(new ShowInputSlot { SlotNumber = 2 }, () => { });
        Assert.Null(editor.SourceId);
        Assert.False(editor.IsDisplayNameEditable);
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
