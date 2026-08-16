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

    // Production participant ids are BARE Zoom SDK ids — LiveProductionSync.MapRawParticipants
    // sets Id = participant.UserId.Trim() with no scheme prefix (LiveProductionSync.cs:129),
    // and ParticipantMapper.ToParticipant carries that straight through to Participant.Id
    // (ParticipantMapper.cs:11). Every roster fixture here must use that shape, not a
    // pre-qualified "zoom:<id>" one — a pre-qualified fixture only proves an already-correct
    // input stays correct, never that the builder does the qualifying.
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
        Assert.Null(TilesLayerPayloadBuilder.Build(scene, [Guest("1")]));
    }

    [Fact]
    public void Build_CarriesEligibleMembersInRosterOrder()
    {
        var payload = TilesLayerPayloadBuilder.Build(
            GalleryScene(), [Guest("1"), Guest("p-2"), Guest("3")]);

        Assert.NotNull(payload);
        Assert.Equal(["zoom:1", "zoom:p-2", "zoom:3"], payload!.Members);
    }

    [Fact]
    public void Build_SkipsParticipantsWithVideoOff()
    {
        var payload = TilesLayerPayloadBuilder.Build(
            GalleryScene(), [Guest("1"), Guest("p-2", FeedHealth.VideoOff)]);

        Assert.Equal(["zoom:1"], payload!.Members);
    }

    [Fact]
    public void Build_TruncatesToMaxTiles()
    {
        var payload = TilesLayerPayloadBuilder.Build(
            GalleryScene(new DynamicGallerySettings { MaxTiles = 2 }),
            [Guest("1"), Guest("p-2"), Guest("3")]);

        Assert.Equal(["zoom:1", "zoom:p-2"], payload!.Members);
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
            [Guest("1")]);

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
            GalleryScene(), [Guest("1", FeedHealth.LowResolution)]);

        Assert.Equal(["zoom:1"], payload!.Members);
    }

    // Coordinator review finding (2026-08-15): a bare member id never matches a Zoom frame.
    // MediaCore.cpp's frame-age matcher (~line 4864) only pairs a Zoom frame's bare
    // participantId against a member spelled EXACTLY "zoom:"+pid; an unqualified member is
    // never admitted (admitTilesMembers stays empty), so the wall's background paints and no
    // guest ever appears on it -- silently, since warnUnmatchedCaptureLayer doesn't cover this
    // path. The builder must qualify bare ids with the "zoom:" scheme, mirroring the core's
    // own MediaCore::normalizeIsoSourceId rule (MediaCore.cpp:1936-1941).
    [Fact]
    public void Build_QualifiesBareIdsWithTheZoomScheme()
    {
        var payload = TilesLayerPayloadBuilder.Build(GalleryScene(), [Guest("42")]);

        Assert.Equal(["zoom:42"], payload!.Members);
    }

    // The qualify step must NOT blindly prepend, or a member that already carries a scheme
    // (capture:/media:) becomes "zoom:capture:..." and matches NOTHING. Mirrors
    // MediaCore::normalizeIsoSourceId's "already has a ':' -> keep as-is" guard.
    [Fact]
    public void Build_PassesThroughAnAlreadyQualifiedMemberUnchanged()
    {
        var payload = TilesLayerPayloadBuilder.Build(GalleryScene(), [Guest("capture:cam-1")]);

        Assert.Equal(["capture:cam-1"], payload!.Members);
    }
}
