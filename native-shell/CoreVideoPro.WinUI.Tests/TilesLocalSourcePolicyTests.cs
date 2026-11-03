using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

// #1071: local sources (webcam, capture card, screen) as Tiles members.
public sealed class TilesLocalSourcePolicyTests
{
    internal static ShowInputSlot Slot(int number, ShowInputKind kind, string? capture = null, string? participant = null, bool inShow = true) =>
        new() { SlotNumber = number, Kind = kind, CaptureDeviceId = capture, ParticipantId = participant, InShow = inShow };

    internal static CaptureDevice Device(string id, string name) => new()
    {
        Id = id, NativeDeviceId = id, Vendor = "uvc", Name = name, Inputs = [], SelectedInputId = id
    };

    [Theory]
    [InlineData("capture:cam-1", true)]
    [InlineData("capture:screen:1", true)]
    [InlineData("media:clip-7", true)]
    [InlineData("zoom:42", false)]
    [InlineData("zoom-pid:abc", false)]
    [InlineData("zoom-name:Someone", false)]
    [InlineData("42", false)]
    [InlineData("", false)]
    [InlineData(null, false)]
    public void IsLocalSourceId_RecognisesOnlyCanonicalCaptureAndMediaIds(string? id, bool expected) =>
        Assert.Equal(expected, TilesLocalSourcePolicy.IsLocalSourceId(id));

    [Fact]
    public void LocalVideoSources_ListsInShowLocalSlotsInSlotOrderWithTheirPickerGroups()
    {
        var slots = new[]
        {
            Slot(4, ShowInputKind.Screen, capture: "screen:1"),
            Slot(1, ShowInputKind.ZoomParticipant, participant: "16778240"),
            Slot(2, ShowInputKind.UvcWebcam, capture: "cam-1"),
            Slot(5, ShowInputKind.Unassigned),
            Slot(3, ShowInputKind.Browser, capture: "browser:1"),
        };

        var sources = TilesLocalSourcePolicy.LocalVideoSources(slots, [Device("cam-1", "Logitech Brio")]);

        Assert.Equal(
            [
                new TilesLocalSource("capture:cam-1", "Logitech Brio", "Camera"),
                new TilesLocalSource("capture:browser:1", "Input 03", "Browser"),
                new TilesLocalSource("capture:screen:1", "Input 04", "Screen"),
            ],
            sources);
    }

    // Review of #1081, finding 3: a BOUND-but-not-in-show slot is not a candidate. It is not
    // brought online (QueueSelectedCaptureDevicesOnline connects InShow devices only), and a
    // Zoom-only wall must not gain a webcam tile on upgrade because a Camera scene's slot exists.
    [Fact]
    public void LocalVideoSources_SkipsASlotThatIsNotInTheShow()
    {
        var slots = new[]
        {
            Slot(1, ShowInputKind.UvcWebcam, capture: "cam-1", inShow: false),
            Slot(2, ShowInputKind.UvcWebcam, capture: "cam-2", inShow: true),
        };

        var sources = TilesLocalSourcePolicy.LocalVideoSources(slots, []);

        Assert.Equal("capture:cam-2", Assert.Single(sources).Id);
    }

    // Review of #1081, finding 1: the core opens media only for a scene ROUTE (it needs the
    // asset path, which a Tiles member id does not carry), so a Media tile would never get a
    // frame. Media slots are not candidates until the tiles wire carries the asset.
    [Fact]
    public void LocalVideoSources_DoesNotOfferAMediaSlotYet()
    {
        var sources = TilesLocalSourcePolicy.LocalVideoSources(
            [Slot(1, ShowInputKind.Media, participant: "media:clip-7"), Slot(2, ShowInputKind.UvcWebcam, capture: "cam-1")], []);

        Assert.Equal("capture:cam-1", Assert.Single(sources).Id);
    }

    [Fact]
    public void LocalVideoSources_DeduplicatesTheSameDeviceBoundTwiceAndKeepsTheFirstSlot()
    {
        var slots = new[]
        {
            Slot(1, ShowInputKind.UvcWebcam, capture: "cam-1"),
            Slot(2, ShowInputKind.Blackmagic, capture: "cam-1"),
        };

        var sources = TilesLocalSourcePolicy.LocalVideoSources(slots, [Device("cam-1", "Cam")]);

        Assert.Equal("Camera", Assert.Single(sources).Group);
    }

    [Fact]
    public void LocalVideoSources_AppliesTheOperatorDisplayNameOverride()
    {
        var sources = TilesLocalSourcePolicy.LocalVideoSources(
            [Slot(1, ShowInputKind.UvcWebcam, capture: "cam-1")],
            [Device("cam-1", "Logitech Brio")],
            (id, derived) => id == "capture:cam-1" ? "Wide shot" : derived);

        Assert.Equal("Wide shot", Assert.Single(sources).Name);
    }

    // The routed set is every IN-SHOW slot's canonical id, Zoom and local alike — the same
    // set BuildTilesLayerWire hands the builder and the workspace member states use.
    [Fact]
    public void RoutedSourceIds_AreTheInShowSlotIdsZoomAndLocalAlike()
    {
        var slots = new[]
        {
            Slot(1, ShowInputKind.ZoomParticipant, participant: "16778240"),
            Slot(2, ShowInputKind.UvcWebcam, capture: "cam-1"),
            Slot(3, ShowInputKind.Screen, capture: "screen:1", inShow: false),
            Slot(4, ShowInputKind.Media, participant: "media:clip-7"),
            Slot(5, ShowInputKind.Unassigned),
        };

        var routed = TilesLocalSourcePolicy.RoutedSourceIds(slots);

        Assert.Equal(new HashSet<string> { "zoom:16778240", "capture:cam-1", "media:clip-7" }, routed);
    }

    [Theory]
    [InlineData(ShowInputKind.UvcWebcam, "Camera")]
    [InlineData(ShowInputKind.Blackmagic, "Camera")]
    [InlineData(ShowInputKind.Aja, "Camera")]
    [InlineData(ShowInputKind.Screen, "Screen")]
    [InlineData(ShowInputKind.Browser, "Browser")]
    [InlineData(ShowInputKind.SrtIngest, "SRT")]
    [InlineData(ShowInputKind.RtmpIngest, "RTMP")]
    [InlineData(ShowInputKind.Ndi, "NDI")]
    [InlineData(ShowInputKind.Media, "Media")]
    public void GroupFor_MatchesTheShowInputPickerGroups(ShowInputKind kind, string group) =>
        Assert.Equal(group, TilesLocalSourcePolicy.GroupFor(kind));
}
