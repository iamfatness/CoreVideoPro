using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public class TilesLayerPayloadBuilderTests
{
    private static Scene GalleryScene(DynamicGallerySettings? settings = null) => new()
    {
        Id = "scene-1",
        Name = "CoreVideo Tiles",
        Layout = "dynamic-gallery",
        DynamicGallery = settings ?? new DynamicGallerySettings()
    };

    private static Participant Guest(string id, FeedHealth health = FeedHealth.Live) =>
        new() { Id = id, Name = $"Guest {id}", Health = health };

    [Fact]
    public void Build_ReturnsNullForANonGalleryScene()
    {
        // Scene.DynamicGallery is init-only, so build the non-gallery scene
        // directly rather than mutating one after construction.
        var scene = new Scene
        {
            Id = "scene-1",
            Name = "CoreVideo Tiles",
            Layout = "dynamic-gallery",
            DynamicGallery = null
        };
        Assert.Null(TilesLayerPayloadBuilder.Build(scene, [Guest("zoom:1")]));
    }

    [Fact]
    public void Build_CarriesEligibleMembersInRosterOrder()
    {
        var payload = TilesLayerPayloadBuilder.Build(
            GalleryScene(), [Guest("zoom:1"), Guest("zoom:2"), Guest("zoom:3")]);

        Assert.NotNull(payload);
        Assert.Equal(["zoom:1", "zoom:2", "zoom:3"], payload!.Members);
    }

    [Fact]
    public void Build_SkipsParticipantsWithVideoOff()
    {
        var payload = TilesLayerPayloadBuilder.Build(
            GalleryScene(), [Guest("zoom:1"), Guest("zoom:2", FeedHealth.VideoOff)]);

        Assert.Equal(["zoom:1"], payload!.Members);
    }

    [Fact]
    public void Build_TruncatesToMaxTiles()
    {
        var payload = TilesLayerPayloadBuilder.Build(
            GalleryScene(new DynamicGallerySettings { MaxTiles = 2 }),
            [Guest("zoom:1"), Guest("zoom:2"), Guest("zoom:3")]);

        Assert.Equal(["zoom:1", "zoom:2"], payload!.Members);
    }

    [Fact]
    public void Build_CarriesTheOperatorsStyleVerbatim()
    {
        var payload = TilesLayerPayloadBuilder.Build(
            GalleryScene(new DynamicGallerySettings
            {
                TileAspect = "1:1",
                GutterPercent = 2.5,
                MarginPercent = 3.0
            }),
            [Guest("zoom:1")]);

        Assert.Equal("1:1", payload!.Style.TileAspect);
        Assert.Equal(2.5, payload.Style.GutterPercent);
        Assert.Equal(3.0, payload.Style.MarginPercent);
    }

    // The shell must NOT pre-filter on its own idea of liveness beyond video-off.
    // Deciding who actually has frames is the core's job; duplicating it here is
    // how the two ends drift.
    [Fact]
    public void Build_DoesNotDropAParticipantMerelyBecauseItLooksUnhealthy()
    {
        var payload = TilesLayerPayloadBuilder.Build(
            GalleryScene(), [Guest("zoom:1", FeedHealth.LowResolution)]);

        Assert.Equal(["zoom:1"], payload!.Members);
    }
}
