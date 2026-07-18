using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
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
            selectedMediaAssetId: "intro",
            selectedMediaAssetPlaying: true,
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
            selectedMediaAssetId: "intro",
            selectedMediaAssetPlaying: true,
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
            selectedMediaAssetId: "intro",
            selectedMediaAssetPlaying: true,
            programRoutes);

        Assert.False(shouldPlay);
    }

    [Fact]
    public void ShouldPlaySceneMediaRoute_DoesNotPlayUnselectedPreviewCue()
    {
        var shouldPlay = MediaRoutePlaybackService.ShouldPlaySceneMediaRoute(
            "intro",
            isProgramScene: false,
            selectedMediaAssetId: "intro",
            selectedMediaAssetPlaying: true,
            programRoutes: []);

        Assert.False(shouldPlay);
    }

    [Fact]
    public void ShouldPlaySceneMediaRoute_RespectsPausedSelectedProgramMedia()
    {
        var programRoutes = new[]
        {
            MediaRoute("intro")
        };

        var shouldPlay = MediaRoutePlaybackService.ShouldPlaySceneMediaRoute(
            "intro",
            isProgramScene: true,
            selectedMediaAssetId: "intro",
            selectedMediaAssetPlaying: false,
            programRoutes);

        Assert.False(shouldPlay);
    }

    [Fact]
    public void ShouldPlaySceneMediaRoute_AutoplaysProgramMediaThatIsNotCurrentSelection()
    {
        var programRoutes = new[]
        {
            MediaRoute("intro")
        };

        var shouldPlay = MediaRoutePlaybackService.ShouldPlaySceneMediaRoute(
            "intro",
            isProgramScene: true,
            selectedMediaAssetId: "bumper",
            selectedMediaAssetPlaying: false,
            programRoutes);

        Assert.True(shouldPlay);
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
    public void ResolvePlaybackSelection_RespectsPausedSelectedProgramMediaRoute()
    {
        var selection = MediaRoutePlaybackService.ResolvePlaybackSelection(
            selectedMediaAssetId: "intro",
            selectedMediaAssetPlaying: false,
            programRoutes: [MediaRoute("intro")]);

        Assert.Equal("intro", selection.MediaAssetId);
        Assert.False(selection.Playing);
    }

    [Fact]
    public void ResolvePlaybackSelection_KeepsSelectedProgramMediaPlayingWhenActive()
    {
        var selection = MediaRoutePlaybackService.ResolvePlaybackSelection(
            selectedMediaAssetId: "intro",
            selectedMediaAssetPlaying: true,
            programRoutes: [MediaRoute("intro")]);

        Assert.Equal("intro", selection.MediaAssetId);
        Assert.True(selection.Playing);
    }

    [Fact]
    public void ResolvePlaybackSelection_CuesManualSelectionWhenProgramHasNoMediaRoute()
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
        Assert.False(selection.Playing);
    }

    [Fact]
    public void BuildSceneMediaPlaybackKey_KeepsPreviewKeyStableAndPaused()
    {
        var first = MediaRoutePlaybackService.BuildSceneMediaPlaybackKey("intro", isProgramScene: false, programTakeVersion: 1);
        var second = MediaRoutePlaybackService.BuildSceneMediaPlaybackKey("intro", isProgramScene: false, programTakeVersion: 2);

        Assert.Equal("preview:media:intro", first);
        Assert.Equal(first, second);
    }

    [Fact]
    public void BuildSceneMediaPlaybackKey_ChangesProgramKeyPerTake()
    {
        var first = MediaRoutePlaybackService.BuildSceneMediaPlaybackKey("intro", isProgramScene: true, programTakeVersion: 1);
        var second = MediaRoutePlaybackService.BuildSceneMediaPlaybackKey("intro", isProgramScene: true, programTakeVersion: 2);

        Assert.Equal("program-take:1:media:intro", first);
        Assert.Equal("program-take:2:media:intro", second);
        Assert.NotEqual(first, second);
    }

    [Fact]
    public void BuildSceneMediaPlaybackKey_UsesProgramTakeKeyWhenProgramMediaIsPaused()
    {
        var shouldPlay = MediaRoutePlaybackService.ShouldPlaySceneMediaRoute(
            "intro",
            isProgramScene: true,
            selectedMediaAssetId: "intro",
            selectedMediaAssetPlaying: false,
            programRoutes: [MediaRoute("intro")]);

        var key = MediaRoutePlaybackService.BuildSceneMediaPlaybackKey(
            "intro",
            isProgramScene: true,
            programTakeVersion: 7);

        Assert.False(shouldPlay);
        Assert.Equal("program-take:7:media:intro", key);
    }

    [Theory]
    [InlineData("intro", true, true)]
    [InlineData("intro", false, false)]
    [InlineData("bumper", true, false)]
    [InlineData("", true, false)]
    public void ShouldAdvanceProgramPlaybackKey_OnlyAdvancesWhenStartingProgramRoutedMedia(
        string mediaAssetId,
        bool startingPlayback,
        bool expected)
    {
        var shouldAdvance = MediaRoutePlaybackService.ShouldAdvanceProgramPlaybackKey(
            mediaAssetId,
            startingPlayback,
            [MediaRoute("intro")]);

        Assert.Equal(expected, shouldAdvance);
    }

    [Fact]
    public void ResolveSceneRoutePlayback_KeepsPreviewMediaPausedWithStablePreviewKey()
    {
        var playback = MediaRoutePlaybackService.ResolveSceneRoutePlayback(
            "intro",
            isProgramScene: false,
            selectedMediaAssetId: "intro",
            selectedMediaAssetPlaying: true,
            programRoutes: [MediaRoute("intro")],
            programTakeVersion: 4);

        Assert.False(playback.Playing);
        Assert.Equal("preview:media:intro", playback.MediaPlaybackKey);
    }

    [Fact]
    public void ResolveSceneRoutePlayback_AutoplaysProgramMediaWithTakeVersionKey()
    {
        var playback = MediaRoutePlaybackService.ResolveSceneRoutePlayback(
            "intro",
            isProgramScene: true,
            selectedMediaAssetId: "bumper",
            selectedMediaAssetPlaying: false,
            programRoutes: [MediaRoute("intro")],
            programTakeVersion: 4);

        Assert.True(playback.Playing);
        Assert.Equal("program-take:4:media:intro", playback.MediaPlaybackKey);
    }

    [Fact]
    public void ResolveSceneRoutePlayback_KeepsPausedSelectedProgramMediaOnProgramKey()
    {
        var playback = MediaRoutePlaybackService.ResolveSceneRoutePlayback(
            "intro",
            isProgramScene: true,
            selectedMediaAssetId: "intro",
            selectedMediaAssetPlaying: false,
            programRoutes: [MediaRoute("intro")],
            programTakeVersion: 4);

        Assert.False(playback.Playing);
        Assert.Equal("program-take:4:media:intro", playback.MediaPlaybackKey);
    }

    [Fact]
    public void BuildProgramMediaRouteSignature_TracksOnlyMediaRouteIdentity()
    {
        var signature = StudioViewModel.BuildProgramMediaRouteSignature(
        [
            MediaRoute("intro", "route-b"),
            new SourceRoute
            {
                Id = "route-guest",
                Mode = SourceRouteMode.Fixed,
                ParticipantId = "guest-1"
            },
            MediaRoute("outro", "route-a")
        ]);

        Assert.Equal("intro:1|outro:1", signature);
    }

    [Fact]
    public void HasPendingProgramMediaCue_DetectsMediaAddedToLiveSceneDraft()
    {
        var programRoutes = new List<SourceRoute>
        {
            new()
            {
                Id = "route-guest",
                Mode = SourceRouteMode.Fixed,
                ParticipantId = "guest-1"
            }
        };
        var previewRoutes = programRoutes.Select(route => route.Clone()).ToList();
        previewRoutes.Add(MediaRoute("intro"));

        Assert.True(StudioViewModel.HasPendingProgramMediaCue(programRoutes, previewRoutes));
        Assert.False(StudioViewModel.HasPendingProgramMediaCue(previewRoutes, previewRoutes));
    }

    [Fact]
    public void BuildProgramMediaRouteSignature_IgnoresFramingChangesForSameMediaRoute()
    {
        var before = MediaRoute("intro");
        before.SourceScale = 1;
        before.SourceOffsetX = 0;

        var after = MediaRoute("intro");
        after.SourceScale = 1.4;
        after.SourceOffsetX = 0.25;

        Assert.Equal(
            StudioViewModel.BuildProgramMediaRouteSignature([before]),
            StudioViewModel.BuildProgramMediaRouteSignature([after]));
    }

    [Fact]
    public void BuildProgramMediaRouteSignature_IgnoresRouteIdChurnForSameMediaAsset()
    {
        var before = MediaRoute("intro", "route-old");
        var after = MediaRoute("intro", "route-new");

        Assert.Equal(
            StudioViewModel.BuildProgramMediaRouteSignature([before]),
            StudioViewModel.BuildProgramMediaRouteSignature([after]));
    }

    [Fact]
    public void BuildProgramMediaRouteSignature_TracksDuplicateMediaAssetCount()
    {
        var single = StudioViewModel.BuildProgramMediaRouteSignature([MediaRoute("intro", "route-a")]);
        var duplicate = StudioViewModel.BuildProgramMediaRouteSignature(
        [
            MediaRoute("intro", "route-a"),
            MediaRoute("intro", "route-b")
        ]);

        Assert.Equal("intro:1", single);
        Assert.Equal("intro:2", duplicate);
        Assert.NotEqual(single, duplicate);
    }

    private static SourceRoute MediaRoute(string assetId) =>
        MediaRoute(assetId, $"route-{assetId}");

    private static SourceRoute MediaRoute(string assetId, string routeId) =>
        new()
        {
            Id = routeId,
            Mode = SourceRouteMode.Fixed,
            ParticipantId = ShowInputRosterService.ToMediaSourceId(assetId)
        };
}
