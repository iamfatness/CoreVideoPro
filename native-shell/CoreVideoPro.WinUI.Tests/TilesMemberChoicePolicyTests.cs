using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

// #1071: the Tiles member picker list (StudioViewModel.GalleryMemberChoices) — order, labels,
// and the membership check every Tiles setter applies.
public sealed class TilesMemberChoicePolicyTests
{
    private static Participant Guest(string id, string name) => new() { Id = id, Name = name, Health = FeedHealth.Live };

    [Fact]
    public void Build_ListsZoomInRosterOrderThenLocalsInPickerGroupOrderThenSlotOrder()
    {
        var locals = new[]
        {
            new TilesLocalSource("capture:screen:1", "Input 02", "Screen"),
            new TilesLocalSource("capture:ndi-1", "Studio NDI", "NDI"),
            new TilesLocalSource("capture:cam-b", "Cam B", "Camera"),
            new TilesLocalSource("capture:cam-a", "Cam A", "Camera"),
        };

        var choices = TilesMemberChoicePolicy.Build([Guest("2", "Zed"), Guest("1", "Amy")], locals);

        Assert.Equal(
            ["zoom:2", "zoom:1", "capture:cam-b", "capture:cam-a", "capture:screen:1", "capture:ndi-1"],
            choices.Select(c => c.Id));
        Assert.Equal(["Zoom", "Zoom", "Camera", "Camera", "Screen", "NDI"], choices.Select(c => c.Group));
    }

    [Fact]
    public void Build_LabelsALocalSourceAsGroupDotNameAndAZoomGuestByNameAlone()
    {
        var choices = TilesMemberChoicePolicy.Build(
            [Guest("1", "Amy")], [new TilesLocalSource("capture:cam-1", "Logitech Brio", "Camera")]);

        Assert.Equal("Amy", choices[0].Label);
        Assert.Equal("Camera · Logitech Brio", choices[1].Label);
        Assert.Equal("Logitech Brio", choices[1].Name);
    }

    [Fact]
    public void Build_QualifiesBareZoomIdsAndDeduplicatesById()
    {
        var choices = TilesMemberChoicePolicy.Build(
            [Guest("1", "Amy"), Guest("zoom:1", "Amy again")],
            [new TilesLocalSource("capture:cam-1", "Cam", "Camera"), new TilesLocalSource("capture:cam-1", "Cam dup", "Camera")]);

        Assert.Equal(["zoom:1", "capture:cam-1"], choices.Select(c => c.Id));
    }

    // The four Tiles setters (AssignTilesSlot, SetTilesExclusion, SetTilesBackground,
    // SetTilesOverride) accept a source id only when it is a current choice. A local id is
    // valid exactly while its slot is in the show; a bare or unknown id never is.
    [Fact]
    public void Contains_AcceptsOnlyCurrentChoicesSoSettersTakeInShowLocalIds()
    {
        var choices = TilesMemberChoicePolicy.Build(
            [Guest("1", "Amy")], [new TilesLocalSource("capture:cam-1", "Cam", "Camera")]);

        Assert.True(TilesMemberChoicePolicy.Contains(choices, "zoom:1"));
        Assert.True(TilesMemberChoicePolicy.Contains(choices, "capture:cam-1"));
        Assert.False(TilesMemberChoicePolicy.Contains(choices, "capture:cam-2"));
        Assert.False(TilesMemberChoicePolicy.Contains(choices, "cam-1"));
        Assert.False(TilesMemberChoicePolicy.Contains(choices, "media:clip-7"));
        Assert.False(TilesMemberChoicePolicy.Contains(choices, ""));
        Assert.False(TilesMemberChoicePolicy.Contains(choices, null));
    }
}
