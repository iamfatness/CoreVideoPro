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
    public void BuildSceneMediaPlaybackKey_ALoopHasNoGeneration()
    {
        Assert.Equal("media:bg", MediaRoutePlaybackService.BuildSceneMediaPlaybackKey("bg", loop: true, goLiveGeneration: 7));
    }

    [Fact]
    public void BuildSceneMediaPlaybackKey_AClipCarriesItsGoLiveGeneration()
    {
        Assert.Equal("media:clip:live:3", MediaRoutePlaybackService.BuildSceneMediaPlaybackKey("clip", loop: false, goLiveGeneration: 3));
    }

    [Theory]
    [InlineData("background", true)]
    [InlineData("Background", true)]
    [InlineData("video", false)]
    [InlineData("lower-third", false)]
    public void IsLoopingAsset_OnlySceneBackgroundsLoop(string kind, bool expected)
    {
        var asset = new MediaAsset { Id = "a", Name = "A", Kind = kind };
        Assert.Equal(expected, MediaRoutePlaybackService.IsLoopingAsset(asset));
    }

    [Fact]
    public void GoLiveLedger_AdvancesOnlyWhenAnAssetEntersProgram()
    {
        var ledger = new MediaGoLiveLedger();
        var clip = MediaRoute("clip");
        var none = Array.Empty<SourceRoute>();
        Assert.Equal(new[] { "clip" }, ledger.RecordTake(none, new[] { clip }));
        Assert.Equal(1, ledger.GenerationOf("clip"));
        // A second Take with the clip STILL on Program does not restart it.
        Assert.Empty(ledger.RecordTake(new[] { clip }, new[] { clip }));
        Assert.Equal(1, ledger.GenerationOf("clip"));
        // Leaving and re-entering Program rolls it again.
        Assert.Empty(ledger.RecordTake(new[] { clip }, none));
        Assert.Equal(new[] { "clip" }, ledger.RecordTake(none, new[] { clip }));
        Assert.Equal(2, ledger.GenerationOf("clip"));
    }

    [Fact]
    public void ChooseAssetToPromote_PrefersTheSelectedAssetWhenItWentLive()
    {
        Assert.Equal("outro", MediaRoutePlaybackService.ChooseAssetToPromote(new[] { "intro", "outro" }, "outro"));
    }

    [Fact]
    public void ChooseAssetToPromote_OtherwiseTakesTheFirstWentLiveAsset()
    {
        Assert.Equal("intro", MediaRoutePlaybackService.ChooseAssetToPromote(new[] { "intro", "outro" }, "bumper"));
        Assert.Equal("intro", MediaRoutePlaybackService.ChooseAssetToPromote(new[] { "intro", "outro" }, null));
    }

    [Fact]
    public void ChooseAssetToPromote_NothingWentLivePromotesNothing()
    {
        // A clip that STAYED on Program (paused or playing) is not in the went-live list, so a
        // Take must not touch it, even when it is the selected asset.
        Assert.Null(MediaRoutePlaybackService.ChooseAssetToPromote(Array.Empty<string>(), "intro"));
        Assert.Null(MediaRoutePlaybackService.ChooseAssetToPromote(Array.Empty<string>(), null));
    }

    [Fact]
    public void GoLiveLedger_OperatorRestartAdvancesTheGeneration()
    {
        var ledger = new MediaGoLiveLedger();
        ledger.RecordTake(Array.Empty<SourceRoute>(), new[] { MediaRoute("clip") });
        ledger.RecordRestart("clip");
        Assert.Equal(2, ledger.GenerationOf("clip"));
    }

    [Fact]
    public void ResolveSceneRoutePlayback_ALoopPlaysOnBothBusesWithOneKey()
    {
        var routes = new[] { MediaRoute("bg") };
        var program = MediaRoutePlaybackService.ResolveSceneRoutePlayback("bg", isProgramScene: true, loop: true, null, false, routes, 0);
        var preview = MediaRoutePlaybackService.ResolveSceneRoutePlayback("bg", isProgramScene: false, loop: true, null, false, routes, 0);
        Assert.True(program.Playing);
        Assert.True(preview.Playing);
        Assert.Equal(program.MediaPlaybackKey, preview.MediaPlaybackKey);
    }

    [Fact]
    public void ResolveSceneRoutePlayback_AClipIsPausedInPreviewAndRollsOnProgram()
    {
        var routes = new[] { MediaRoute("clip") };
        var preview = MediaRoutePlaybackService.ResolveSceneRoutePlayback("clip", isProgramScene: false, loop: false, null, false, routes, 1);
        var program = MediaRoutePlaybackService.ResolveSceneRoutePlayback("clip", isProgramScene: true, loop: false, null, false, routes, 1);
        Assert.False(preview.Playing);
        Assert.True(program.Playing);
        Assert.Equal("media:clip:live:1", program.MediaPlaybackKey);
        Assert.Equal("media:clip:live:1", preview.MediaPlaybackKey);
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
