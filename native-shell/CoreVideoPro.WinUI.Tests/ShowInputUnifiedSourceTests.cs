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

        Assert.Contains(options, o => o.Value == "zoom:p-1" && o.Group == "Zoom");
        Assert.Contains(options, o => o.Value == "capture:cam-1" && o.Group == "Camera" && o.Label == "Elgato HD60");
        Assert.Contains(options, o => o.Value == "capture:screen:0" && o.Group == "Screen" && o.Label == "DISPLAY1");
        Assert.Contains(options, o => o.Value == "media:a-1" && o.Group == "Media");
        Assert.DoesNotContain(options, o => ShowInputRosterService.IsHintSourceId(o.Value));
    }

    [Fact]
    public void UnifiedSourceDisplayLabel_CarriesTheGroupSoNoTypeChipIsNeeded()
    {
        var options = ShowInputRosterService.BuildUnifiedSourceOptions(
            [], [Camera("cam-1", "Elgato HD60")], []);

        Assert.Equal("Camera · Elgato HD60",
            ShowInputRosterService.UnifiedSourceDisplayLabel(options, "capture:cam-1"));
        Assert.Null(ShowInputRosterService.UnifiedSourceDisplayLabel(options, null));

        var missing = ShowInputRosterService.BuildUnifiedSourceOptions(
            [], [], [], currentSourceId: "capture:gone", currentSourceLabel: "Steven Smith");
        Assert.Equal("Missing — was Steven Smith",
            ShowInputRosterService.UnifiedSourceDisplayLabel(missing, "capture:gone"));
    }

    [Fact]
    public void BuildUnifiedSourceOptions_EmptyGroupsRenderHintsNotSilence()
    {
        var options = ShowInputRosterService.BuildUnifiedSourceOptions([], [], []);

        Assert.Contains(options, o => o.Value == "hint:zoom" && o.Group == "Zoom" && o.Label.Contains("join a meeting"));
        Assert.Contains(options, o => o.Value == "hint:media" && o.Group == "Media" && o.Label.Contains("Media tab"));
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
    [InlineData("browser:1", "browser", ShowInputKind.Browser)]
    public void InferCaptureDeviceKind_MapsDeviceToSlotKind(string id, string vendor, ShowInputKind expected)
    {
        Assert.Equal(expected, ShowInputRosterService.InferCaptureDeviceKind(Device(id, "x", vendor)));
    }

    // ---- Browser sources (BR-1) -----------------------------------------------------

    [Fact]
    public void BrowserSource_GroupsUnderBrowserAndAssignsAsCaptureClass()
    {
        var options = ShowInputRosterService.BuildUnifiedSourceOptions(
            [], [Browser("browser:1", "Browser: https://overlays.example")], []);
        Assert.Contains(options, o =>
            o.Value == "capture:browser:1" && o.Group == "Browser" &&
            o.Label == "Browser: https://overlays.example");

        var editor = Editor(out var slot);
        editor.RefreshSourceOptions([], [Browser("browser:1", "Browser: https://overlays.example")], mediaAssets: []);
        editor.SelectedUnifiedSourceId = "capture:browser:1";
        Assert.Equal(ShowInputKind.Browser, slot.Kind);
        Assert.Equal("browser:1", slot.CaptureDeviceId);
        Assert.True(slot.IsAssigned);
    }

    [Fact]
    public void BrowserSource_RoutesAndMultiviewWiresLikeAnyCaptureDevice()
    {
        var slot = new ShowInputSlot { SlotNumber = 1 };
        slot.Kind = ShowInputKind.Browser;
        slot.CaptureDeviceId = "browser:1";
        slot.InShow = true;

        var route = new SourceRoute { Id = "route-1" };
        ShowInputRosterService.ApplySlotRoute(route, slot);
        Assert.Equal(SourceRouteMode.CaptureDevice, route.Mode);
        Assert.Equal("browser:1", route.CaptureDeviceId);

        var wires = ShowInputRosterService.BuildMultiviewLayoutSources(
            [slot], [], [Browser("browser:1", "Browser: https://overlays.example")]);
        var wire = Assert.Single(wires);
        Assert.Equal("capture:browser:1", wire.SourceId);
        Assert.Equal("capture", wire.Kind);
        Assert.Equal("browser:1", wire.CaptureDeviceId);
    }

    private static CaptureDevice Browser(string id, string name) => Device(id, name, "browser");

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
