using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class MediaRoutePlaybackServiceTests
{
    [Fact]
    public void ShouldPlayMediaRoute_DoesNotAutoplayPreviewJustBecauseAssetIsOnProgram()
    {
        var programRoutes = new[]
        {
            MediaRoute("intro")
        };

        var shouldPlay = MediaRoutePlaybackService.ShouldPlayMediaRoute(
            "intro",
            isProgramScene: false,
            selectedMediaAssetId: null,
            selectedMediaAssetPlaying: false,
            programRoutes);

        Assert.False(shouldPlay);
    }

    [Fact]
    public void ShouldPlayMediaRoute_AutoplaysWhenMediaRouteIsOnProgram()
    {
        var programRoutes = new[]
        {
            MediaRoute("intro")
        };

        var shouldPlay = MediaRoutePlaybackService.ShouldPlayMediaRoute(
            "intro",
            isProgramScene: true,
            selectedMediaAssetId: null,
            selectedMediaAssetPlaying: false,
            programRoutes);

        Assert.True(shouldPlay);
    }

    [Fact]
    public void ShouldPlayMediaRoute_AllowsExplicitSelectedPlaybackInPreview()
    {
        var shouldPlay = MediaRoutePlaybackService.ShouldPlayMediaRoute(
            "intro",
            isProgramScene: false,
            selectedMediaAssetId: "intro",
            selectedMediaAssetPlaying: true,
            programRoutes: []);

        Assert.True(shouldPlay);
    }

    [Fact]
    public void ShouldPlayMediaRoute_DoesNotPlayUnselectedPreviewCue()
    {
        var shouldPlay = MediaRoutePlaybackService.ShouldPlayMediaRoute(
            "intro",
            isProgramScene: false,
            selectedMediaAssetId: "outro",
            selectedMediaAssetPlaying: true,
            programRoutes: []);

        Assert.False(shouldPlay);
    }

    [Fact]
    public void ResolveProgramAutoplayAssetId_PreservesSelectedAssetWhenItIsOnProgram()
    {
        var programRoutes = new[]
        {
            MediaRoute("intro"),
            MediaRoute("outro")
        };

        var assetId = MediaRoutePlaybackService.ResolveProgramAutoplayAssetId("outro", programRoutes);

        Assert.Equal("outro", assetId);
    }

    [Fact]
    public void ResolveProgramAutoplayAssetId_UsesFirstProgramMediaRouteWhenSelectionIsElsewhere()
    {
        var programRoutes = new[]
        {
            MediaRoute("intro"),
            MediaRoute("outro")
        };

        var assetId = MediaRoutePlaybackService.ResolveProgramAutoplayAssetId("bumper", programRoutes);

        Assert.Equal("intro", assetId);
    }

    [Fact]
    public void ResolveProgramAutoplayAssetId_ReturnsNullWhenProgramHasNoMediaRoute()
    {
        var programRoutes = new[]
        {
            new SourceRoute
            {
                Id = "route-guest",
                Mode = SourceRouteMode.Fixed,
                ParticipantId = "guest-1"
            }
        };

        var assetId = MediaRoutePlaybackService.ResolveProgramAutoplayAssetId("intro", programRoutes);

        Assert.Null(assetId);
    }

    private static SourceRoute MediaRoute(string assetId) =>
        new()
        {
            Id = $"route-{assetId}",
            Mode = SourceRouteMode.Fixed,
            ParticipantId = ShowInputRosterService.ToMediaSourceId(assetId)
        };
}
