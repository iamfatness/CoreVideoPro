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

    private static SourceRoute MediaRoute(string assetId) =>
        new()
        {
            Id = $"route-{assetId}",
            Mode = SourceRouteMode.Fixed,
            ParticipantId = ShowInputRosterService.ToMediaSourceId(assetId)
        };
}
