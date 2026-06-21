using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class MediaRoutePlaybackServiceTests
{
    [Fact]
    public void ShouldPlaySceneMediaRoute_DoesNotAutoplayPreviewJustBecauseAssetIsOnProgram()
    {
        var programRoutes = new[]
        {
            MediaRoute("intro")
        };

        var shouldPlay = MediaRoutePlaybackService.ShouldPlaySceneMediaRoute(
            "intro",
            isProgramScene: false,
            programRoutes);

        Assert.False(shouldPlay);
    }

    [Fact]
    public void ShouldPlaySceneMediaRoute_AutoplaysWhenMediaRouteIsOnProgram()
    {
        var programRoutes = new[]
        {
            MediaRoute("intro")
        };

        var shouldPlay = MediaRoutePlaybackService.ShouldPlaySceneMediaRoute(
            "intro",
            isProgramScene: true,
            programRoutes);

        Assert.True(shouldPlay);
    }

    [Fact]
    public void ShouldPlaySceneMediaRoute_DoesNotPlaySelectedMediaInPreview()
    {
        var programRoutes = new[]
        {
            MediaRoute("intro")
        };

        var shouldPlay = MediaRoutePlaybackService.ShouldPlaySceneMediaRoute(
            "intro",
            isProgramScene: false,
            programRoutes);

        Assert.False(shouldPlay);
    }

    [Fact]
    public void ShouldPlaySceneMediaRoute_DoesNotPlayUnselectedPreviewCue()
    {
        var shouldPlay = MediaRoutePlaybackService.ShouldPlaySceneMediaRoute(
            "intro",
            isProgramScene: false,
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

    [Fact]
    public void ResolvePlaybackSelection_AutoplaysProgramMediaRouteOverPausedManualSelection()
    {
        var selection = MediaRoutePlaybackService.ResolvePlaybackSelection(
            selectedMediaAssetId: "bumper",
            selectedMediaAssetPlaying: false,
            programRoutes: [MediaRoute("intro")]);

        Assert.Equal("intro", selection.MediaAssetId);
        Assert.True(selection.Playing);
    }

    [Fact]
    public void ResolvePlaybackSelection_PreservesManualPlaybackWhenProgramHasNoMediaRoute()
    {
        var selection = MediaRoutePlaybackService.ResolvePlaybackSelection(
            selectedMediaAssetId: "bumper",
            selectedMediaAssetPlaying: true,
            programRoutes:
            [
                new SourceRoute
                {
                    Id = "route-guest",
                    Mode = SourceRouteMode.Fixed,
                    ParticipantId = "guest-1"
                }
            ]);

        Assert.Equal("bumper", selection.MediaAssetId);
        Assert.True(selection.Playing);
    }

    private static SourceRoute MediaRoute(string assetId) =>
        new()
        {
            Id = $"route-{assetId}",
            Mode = SourceRouteMode.Fixed,
            ParticipantId = ShowInputRosterService.ToMediaSourceId(assetId)
        };
}
