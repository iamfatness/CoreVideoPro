using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>SRC-1 (sources-redesign-spec §A2): one unified source picker — kind inferred
/// from the pick, explicit empty-state hints, and NO silent source substitution.</summary>
public sealed class ShowInputUnifiedSourceTests
{
    [Fact]
    public void BuildUnifiedSourceOptions_GroupsAllSourceKindsUnderCanonicalIds()
    {
        var options = ShowInputRosterService.BuildUnifiedSourceOptions(
            [Participant("p-1", "Jane")],
            [Camera("cam-1", "Elgato HD60"), Screen("screen:0", "DISPLAY1")],
            [Asset("a-1", "intro.mp4")]);

        Assert.Contains(options, o => o.Value == "zoom:p-1" && o.Label.StartsWith("Zoom ·"));
        Assert.Contains(options, o => o.Value == "capture:cam-1" && o.Label == "Camera · Elgato HD60");
        Assert.Contains(options, o => o.Value == "capture:screen:0" && o.Label == "Screen · DISPLAY1");
        Assert.Contains(options, o => o.Value == "media:a-1" && o.Label.StartsWith("Media ·"));
        Assert.DoesNotContain(options, o => ShowInputRosterService.IsHintSourceId(o.Value));
    }

    [Fact]
    public void BuildUnifiedSourceOptions_EmptyGroupsRenderHintsNotSilence()
    {
        var options = ShowInputRosterService.BuildUnifiedSourceOptions([], [], []);

        Assert.Contains(options, o => o.Value == "hint:zoom" && o.Label.Contains("join a meeting"));
        Assert.Contains(options, o => o.Value == "hint:media" && o.Label.Contains("Media tab"));
        Assert.All(options, o => Assert.True(ShowInputRosterService.IsHintSourceId(o.Value)));
    }

    [Fact]
    public void BuildUnifiedSourceOptions_MissingCurrentSourceIsPrependedNeverDropped()
    {
        var options = ShowInputRosterService.BuildUnifiedSourceOptions(
            [], [Camera("cam-2", "Other Cam")], [],
            currentSourceId: "capture:cam-1", currentSourceLabel: "Steven Smith");

        Assert.Equal("capture:cam-1", options[0].Value);
        Assert.Equal("Missing — was Steven Smith", options[0].Label);
    }

    [Theory]
    [InlineData("screen:0", "uvc", ShowInputKind.Screen)]
    [InlineData("dev-1", "blackmagic", ShowInputKind.Blackmagic)]
    [InlineData("dev-2", "aja", ShowInputKind.Aja)]
    [InlineData("dev-3", "srt", ShowInputKind.SrtIngest)]
    [InlineData("dev-4", "uvc", ShowInputKind.UvcWebcam)]
    [InlineData("dev-5", "windows", ShowInputKind.UvcWebcam)]
    public void InferCaptureDeviceKind_MapsDeviceToSlotKind(string id, string vendor, ShowInputKind expected)
    {
        Assert.Equal(expected, ShowInputRosterService.InferCaptureDeviceKind(Device(id, "x", vendor)));
    }

    [Fact]
    public void SelectedUnifiedSourceId_SetInfersKindAndAssignsIdsTogether()
    {
        var editor = Editor(out var slot);
        editor.RefreshSourceOptions(
            [Participant("p-1", "Jane")],
            [Camera("cam-1", "Elgato"), Screen("screen:0", "DISPLAY1")],
            mediaAssets: [Asset("a-1", "intro.mp4")]);

        editor.SelectedUnifiedSourceId = "capture:cam-1";
        Assert.Equal(ShowInputKind.UvcWebcam, slot.Kind);
        Assert.Equal("cam-1", slot.CaptureDeviceId);

        editor.SelectedUnifiedSourceId = "capture:screen:0";
        Assert.Equal(ShowInputKind.Screen, slot.Kind);
        Assert.Equal("screen:0", slot.CaptureDeviceId);

        editor.SelectedUnifiedSourceId = "media:a-1";
        Assert.Equal(ShowInputKind.Media, slot.Kind);
        Assert.Equal("media:a-1", slot.ParticipantId);
        Assert.Null(slot.CaptureDeviceId);

        editor.SelectedUnifiedSourceId = "zoom:p-1";
        Assert.Equal(ShowInputKind.ZoomParticipant, slot.Kind);
        Assert.Equal("p-1", slot.ParticipantId);
    }

    [Fact]
    public void SelectedUnifiedSourceId_IgnoresHintRows()
    {
        var editor = Editor(out var slot);
        editor.RefreshSourceOptions([], [Camera("cam-1", "Elgato")], mediaAssets: []);
        editor.SelectedUnifiedSourceId = "capture:cam-1";

        editor.SelectedUnifiedSourceId = "hint:media";

        Assert.Equal(ShowInputKind.UvcWebcam, slot.Kind);
        Assert.Equal("cam-1", slot.CaptureDeviceId);
    }

    [Fact]
    public void GoneSourceIsMarkedMissingAndNeverSubstituted()
    {
        var editor = Editor(out var slot);
        editor.RefreshSourceOptions([], [Camera("cam-1", "Elgato"), Camera("cam-2", "Logi")], mediaAssets: []);
        editor.SelectedUnifiedSourceId = "capture:cam-1";

        // cam-1 unplugs; cam-2 is still there. The old behavior silently re-pointed the
        // slot at cam-2 — the exact live-show hazard SRC-1 removes.
        editor.RefreshSourceOptions([], [Camera("cam-2", "Logi")], mediaAssets: []);

        Assert.Equal("cam-1", slot.CaptureDeviceId);
        Assert.True(editor.IsSourceMissing);
        Assert.Contains(editor.UnifiedSourceOptions, o => o.Value == "capture:cam-1" && o.Label.StartsWith("Missing —"));

        // The device returns: binding re-attaches, missing clears.
        editor.RefreshSourceOptions([], [Camera("cam-1", "Elgato"), Camera("cam-2", "Logi")], mediaAssets: []);
        Assert.False(editor.IsSourceMissing);
        Assert.Equal("cam-1", slot.CaptureDeviceId);
    }

    private static ShowInputSlotViewModel Editor(out ShowInputSlot slot)
    {
        slot = new ShowInputSlot { SlotNumber = 1 };
        return new ShowInputSlotViewModel(slot, () => { });
    }

    private static Participant Participant(string id, string name) =>
        new() { Id = id, Name = name, Health = FeedHealth.Live };

    private static CaptureDevice Camera(string id, string name) => Device(id, name, "uvc");

    private static CaptureDevice Screen(string id, string name) => Device(id, name, "uvc");

    private static CaptureDevice Device(string id, string name, string vendor) =>
        new()
        {
            Id = id,
            NativeDeviceId = $"native-{id}",
            Name = name,
            Vendor = vendor,
            Inputs = [new CaptureDeviceInput { Id = "input-1", Label = "Input 1" }],
            SelectedInputId = "input-1"
        };

    private static MediaAsset Asset(string id, string name) =>
        new() { Id = id, Name = name, Kind = "video" };
}
