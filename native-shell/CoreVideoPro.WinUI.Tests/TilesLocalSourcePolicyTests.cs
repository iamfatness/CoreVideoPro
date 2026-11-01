using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

// #1071: local sources (webcam, capture card, screen, media) as Tiles members.
public sealed class TilesLocalSourcePolicyTests
{
    private static ShowInputSlot Slot(int number, ShowInputKind kind, string? capture = null, string? participant = null, bool inShow = true) =>
        new() { SlotNumber = number, Kind = kind, CaptureDeviceId = capture, ParticipantId = participant, InShow = inShow };

    private static CaptureDevice Device(string id, string name) => new()
    {
        Id = id, NativeDeviceId = id, Vendor = "uvc", Name = name, Inputs = [], SelectedInputId = id
    };

    private static MediaAsset Asset(string id, string name, string kind = "video") =>
        new() { Id = id, Name = name, Kind = kind };

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
    public void LocalVideoSources_ListsBoundLocalSlotsInSlotOrderWithTheirPickerGroups()
    {
        var slots = new[]
        {
            Slot(3, ShowInputKind.Media, participant: "media:clip-7"),
            Slot(1, ShowInputKind.ZoomParticipant, participant: "16778240"),
            Slot(2, ShowInputKind.UvcWebcam, capture: "cam-1"),
            Slot(4, ShowInputKind.Screen, capture: "screen:1"),
            Slot(5, ShowInputKind.Unassigned),
        };

        var sources = TilesLocalSourcePolicy.LocalVideoSources(
            slots, [Device("cam-1", "Logitech Brio")], [Asset("clip-7", "Opening clip")]);

        Assert.Equal(
            [
                new TilesLocalSource("capture:cam-1", "Logitech Brio", "Camera"),
                new TilesLocalSource("media:clip-7", "Opening clip", "Media"),
                new TilesLocalSource("capture:screen:1", "Input 04", "Screen"),
            ],
            sources);
    }

    [Fact]
    public void LocalVideoSources_IgnoresInShowBecauseRoutedModeOwnsThatFilter()
    {
        // Eligible mode seats any bound local source; routed mode narrows to InShow slots
        // downstream (TilesMembershipPolicy), exactly as it does for Zoom participants.
        var sources = TilesLocalSourcePolicy.LocalVideoSources(
            [Slot(1, ShowInputKind.UvcWebcam, capture: "cam-1", inShow: false)], [], []);

        Assert.Equal("capture:cam-1", Assert.Single(sources).Id);
    }

    [Fact]
    public void LocalVideoSources_SkipsAMediaSlotWhoseAssetHasNoVideoOrLeftTheBin()
    {
        var slots = new[]
        {
            Slot(1, ShowInputKind.Media, participant: "media:song"),
            Slot(2, ShowInputKind.Media, participant: "media:gone"),
            Slot(3, ShowInputKind.Media, participant: "media:logo"),
        };
        // The caller passes VISUAL assets only (video + stills); "song" is not among them.
        var visual = new[] { Asset("logo", "Bug", "image") };

        var sources = TilesLocalSourcePolicy.LocalVideoSources(slots, [], visual);

        Assert.Equal(["media:logo"], sources.Select(s => s.Id));
    }

    [Fact]
    public void LocalVideoSources_DeduplicatesTheSameDeviceBoundTwiceAndKeepsTheFirstSlot()
    {
        var slots = new[]
        {
            Slot(1, ShowInputKind.UvcWebcam, capture: "cam-1"),
            Slot(2, ShowInputKind.Blackmagic, capture: "cam-1"),
        };

        var sources = TilesLocalSourcePolicy.LocalVideoSources(slots, [Device("cam-1", "Cam")], []);

        Assert.Equal("Camera", Assert.Single(sources).Group);
    }

    [Fact]
    public void LocalVideoSources_AppliesTheOperatorDisplayNameOverride()
    {
        var sources = TilesLocalSourcePolicy.LocalVideoSources(
            [Slot(1, ShowInputKind.UvcWebcam, capture: "cam-1")],
            [Device("cam-1", "Logitech Brio")],
            [],
            (id, derived) => id == "capture:cam-1" ? "Wide shot" : derived);

        Assert.Equal("Wide shot", Assert.Single(sources).Name);
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
