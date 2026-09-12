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
            operatorPausedAssetIds: NoPaused,
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
            operatorPausedAssetIds: NoPaused,
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
            operatorPausedAssetIds: NoPaused,
            programRoutes);

        Assert.False(shouldPlay);
    }

    [Fact]
    public void ShouldPlaySceneMediaRoute_DoesNotPlayUnselectedPreviewCue()
    {
        var shouldPlay = MediaRoutePlaybackService.ShouldPlaySceneMediaRoute(
            "intro",
            isProgramScene: false,
            operatorPausedAssetIds: NoPaused,
            programRoutes: []);

        Assert.False(shouldPlay);
    }

    [Fact]
    public void ShouldPlaySceneMediaRoute_RespectsAnOperatorPausedProgramClip()
    {
        var programRoutes = new[]
        {
            MediaRoute("intro")
        };

        var shouldPlay = MediaRoutePlaybackService.ShouldPlaySceneMediaRoute(
            "intro",
            isProgramScene: true,
            operatorPausedAssetIds: new[] { "intro" },
            programRoutes);

        Assert.False(shouldPlay);
    }

    [Fact]
    public void ShouldPlaySceneMediaRoute_AutoplaysProgramMediaThatWasNotPaused()
    {
        var programRoutes = new[]
        {
            MediaRoute("intro")
        };

        var shouldPlay = MediaRoutePlaybackService.ShouldPlaySceneMediaRoute(
            "intro",
            isProgramScene: true,
            operatorPausedAssetIds: new[] { "bumper" },
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
    public void ResolveSceneRoutePlayback_ALoopPlaysOnBothBusesWithOneKey()
    {
        var routes = new[] { MediaRoute("bg") };
        var program = MediaRoutePlaybackService.ResolveSceneRoutePlayback("bg", isProgramScene: true, loop: true, NoPaused, routes, 0);
        var preview = MediaRoutePlaybackService.ResolveSceneRoutePlayback("bg", isProgramScene: false, loop: true, NoPaused, routes, 0);
        Assert.True(program.Playing);
        Assert.True(preview.Playing);
        Assert.Equal(program.MediaPlaybackKey, preview.MediaPlaybackKey);
    }

    [Fact]
    public void ResolveSceneRoutePlayback_AClipIsPausedInPreviewAndRollsOnProgram()
    {
        var routes = new[] { MediaRoute("clip") };
        var preview = MediaRoutePlaybackService.ResolveSceneRoutePlayback("clip", isProgramScene: false, loop: false, NoPaused, routes, 1);
        var program = MediaRoutePlaybackService.ResolveSceneRoutePlayback("clip", isProgramScene: true, loop: false, NoPaused, routes, 1);
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

    // ---- Operator pause is per-asset state (final review FR3) -------------------------------

    [Fact]
    public void APausedProgramClipStaysPausedWhenAnotherAssetGoesLiveAndIsPromoted()
    {
        var ledger = new MediaGoLiveLedger();
        var x = MediaRoute("x");
        var y = MediaRoute("y");
        ledger.RecordTake(Array.Empty<SourceRoute>(), new[] { x });
        ledger.RecordPause("x");
        var before = MediaRoutePlaybackService.ResolveSceneRoutePlayback(
            "x", isProgramScene: true, loop: false, ledger.OperatorPausedAssetIds, new[] { x }, ledger.GenerationOf("x"));
        Assert.False(before.Playing);

        // Y goes live on a Take that leaves X on Program; the shell promotes Y.
        var wentLive = ledger.RecordTake(new[] { x }, new[] { x, y });
        Assert.Equal(new[] { "y" }, wentLive);
        Assert.Equal("y", MediaRoutePlaybackService.ChooseAssetToPromote(wentLive, selectedMediaAssetId: "x"));

        var after = MediaRoutePlaybackService.ResolveSceneRoutePlayback(
            "x", isProgramScene: true, loop: false, ledger.OperatorPausedAssetIds, new[] { x, y }, ledger.GenerationOf("x"));
        Assert.False(after.Playing);
        // Same key AND same playing flag: the core's request key is unchanged, so no cold restart.
        Assert.Equal(before.MediaPlaybackKey, after.MediaPlaybackKey);
        var yPlayback = MediaRoutePlaybackService.ResolveSceneRoutePlayback(
            "y", isProgramScene: true, loop: false, ledger.OperatorPausedAssetIds, new[] { x, y }, ledger.GenerationOf("y"));
        Assert.True(yPlayback.Playing);
    }

    [Fact]
    public void APausedProgramClipResumesWhenTheOperatorPlaysIt()
    {
        var ledger = new MediaGoLiveLedger();
        var x = MediaRoute("x");
        ledger.RecordTake(Array.Empty<SourceRoute>(), new[] { x });
        ledger.RecordPause("x");
        Assert.True(ledger.IsOperatorPaused("x"));

        ledger.RecordPlay("x");

        Assert.False(ledger.IsOperatorPaused("x"));
        Assert.True(MediaRoutePlaybackService.ShouldPlaySceneMediaRoute(
            "x", isProgramScene: true, ledger.OperatorPausedAssetIds, new[] { x }));
    }

    [Fact]
    public void APausedClipGoingLiveAgainAfterLeavingProgramClearsThePauseAndRolls()
    {
        var ledger = new MediaGoLiveLedger();
        var x = MediaRoute("x");
        var none = Array.Empty<SourceRoute>();
        ledger.RecordTake(none, new[] { x });
        ledger.RecordPause("x");
        ledger.RecordTake(new[] { x }, none);  // X leaves Program, still paused
        Assert.True(ledger.IsOperatorPaused("x"));
        var generationBefore = ledger.GenerationOf("x");

        Assert.Equal(new[] { "x" }, ledger.RecordTake(none, new[] { x }));  // X goes live again

        Assert.False(ledger.IsOperatorPaused("x"));
        var playback = MediaRoutePlaybackService.ResolveSceneRoutePlayback(
            "x", isProgramScene: true, loop: false, ledger.OperatorPausedAssetIds, new[] { x }, ledger.GenerationOf("x"));
        Assert.True(playback.Playing);
        Assert.Equal(generationBefore + 1, ledger.GenerationOf("x"));  // rolls from frame 0
    }

    [Fact]
    public void AStillGoingLiveIsNeverPromoted()
    {
        var stills = new HashSet<string>(StringComparer.Ordinal) { "logo" };
        bool SupportsPlayback(string id) => !stills.Contains(id);

        Assert.Null(MediaRoutePlaybackService.ChooseAssetToPromote(new[] { "logo" }, "logo", SupportsPlayback));
        Assert.Equal("clip", MediaRoutePlaybackService.ChooseAssetToPromote(new[] { "logo", "clip" }, "logo", SupportsPlayback));
    }

    [Fact]
    public void ALoopPlaysOnProgramEvenIfItsIdIsInThePausedSet()
    {
        var routes = new[] { MediaRoute("bg") };
        var playback = MediaRoutePlaybackService.ResolveSceneRoutePlayback(
            "bg", isProgramScene: true, loop: true, new[] { "bg" }, routes, 0);
        Assert.True(playback.Playing);
    }

    // ---- Tap pauses/resumes a Program clip on its clock; it never restarts it (T1.2 task 2) ----

    [Fact]
    public void ResolveTap_ARollingUnselectedProgramClipPauses()
    {
        var action = MediaRoutePlaybackService.ResolveTap(isOnProgram: true, isLooping: false, isOperatorPaused: false);
        Assert.Equal(MediaRoutePlaybackService.MediaTapAction.Pause, action);
    }

    [Fact]
    public void ResolveTap_APausedProgramClipResumes()
    {
        var action = MediaRoutePlaybackService.ResolveTap(isOnProgram: true, isLooping: false, isOperatorPaused: true);
        Assert.Equal(MediaRoutePlaybackService.MediaTapAction.Resume, action);
    }

    [Fact]
    public void ResolveTap_AClipNotOnProgramIsJustSelected()
    {
        var action = MediaRoutePlaybackService.ResolveTap(isOnProgram: false, isLooping: false, isOperatorPaused: false);
        Assert.Equal(MediaRoutePlaybackService.MediaTapAction.Select, action);
    }

    [Fact]
    public void ResolveTap_ALoopOnProgramIsJustSelectedNeverPaused()
    {
        var action = MediaRoutePlaybackService.ResolveTap(isOnProgram: true, isLooping: true, isOperatorPaused: false);
        Assert.Equal(MediaRoutePlaybackService.MediaTapAction.Select, action);
    }

    [Fact]
    public void IsPlayingOnAir_TrueForARollingUnselectedProgramClip()
    {
        Assert.True(MediaRoutePlaybackService.IsPlayingOnAir("clip", isOnProgram: true, isLooping: false, NoPaused));
    }

    [Fact]
    public void IsPlayingOnAir_FalseForAPausedProgramClip()
    {
        Assert.False(MediaRoutePlaybackService.IsPlayingOnAir("clip", isOnProgram: true, isLooping: false, new[] { "clip" }));
    }

    [Fact]
    public void IsPlayingOnAir_TrueForALoopingAssetEvenIfPaused()
    {
        Assert.True(MediaRoutePlaybackService.IsPlayingOnAir("bg", isOnProgram: true, isLooping: true, new[] { "bg" }));
    }

    [Fact]
    public void IsPlayingOnAir_FalseWhenNotOnProgram()
    {
        Assert.False(MediaRoutePlaybackService.IsPlayingOnAir("clip", isOnProgram: false, isLooping: false, NoPaused));
    }

    // ---- SelectedAssetLeftProgram (T1.2 task 3, controller ruling) -----------------------

    [Fact]
    public void SelectedAssetLeftProgram_TrueWhenTheSelectedClipWasOnProgramAndIsNotNow()
    {
        var before = new[] { MediaRoute("clip") };
        var after = Array.Empty<SourceRoute>();
        Assert.True(MediaRoutePlaybackService.SelectedAssetLeftProgram("clip", before, after));
    }

    [Fact]
    public void SelectedAssetLeftProgram_FalseWhenTheSelectedClipStaysOnProgram()
    {
        var routes = new[] { MediaRoute("clip") };
        Assert.False(MediaRoutePlaybackService.SelectedAssetLeftProgram("clip", routes, routes));
    }

    [Fact]
    public void SelectedAssetLeftProgram_FalseWhenTheSelectedAssetWasNeverOnProgram()
    {
        var before = Array.Empty<SourceRoute>();
        var after = Array.Empty<SourceRoute>();
        Assert.False(MediaRoutePlaybackService.SelectedAssetLeftProgram("clip", before, after));
    }

    [Fact]
    public void SelectedAssetLeftProgram_FalseWhenADifferentClipLeftProgram()
    {
        // Only the SELECTED asset's departure matters here -- an unselected clip leaving
        // Program is handled by the bin-row refresh, not this per-selection clearing check.
        var before = new[] { MediaRoute("clip"), MediaRoute("other") };
        var after = new[] { MediaRoute("clip") };
        Assert.False(MediaRoutePlaybackService.SelectedAssetLeftProgram("clip", before, after));
    }

    [Fact]
    public void SelectedAssetLeftProgram_FalseWhenNoAssetIsSelected()
    {
        var before = new[] { MediaRoute("clip") };
        var after = Array.Empty<SourceRoute>();
        Assert.False(MediaRoutePlaybackService.SelectedAssetLeftProgram(null, before, after));
    }

    [Fact]
    public void TappingARollingProgramClipPausesItWithoutTouchingTheGeneration()
    {
        var ledger = new MediaGoLiveLedger();
        var clip = MediaRoute("clip");
        ledger.RecordTake(Array.Empty<SourceRoute>(), new[] { clip });
        var generationBefore = ledger.GenerationOf("clip");

        var action = MediaRoutePlaybackService.ResolveTap(isOnProgram: true, isLooping: false, ledger.IsOperatorPaused("clip"));
        Assert.Equal(MediaRoutePlaybackService.MediaTapAction.Pause, action);
        ledger.RecordPause("clip");

        Assert.Equal(generationBefore, ledger.GenerationOf("clip"));
        Assert.Contains("clip", ledger.OperatorPausedAssetIds);
    }

    [Fact]
    public void TappingAPausedProgramClipResumesItWithoutTouchingTheGeneration()
    {
        var ledger = new MediaGoLiveLedger();
        var clip = MediaRoute("clip");
        ledger.RecordTake(Array.Empty<SourceRoute>(), new[] { clip });
        ledger.RecordPause("clip");
        var generationBefore = ledger.GenerationOf("clip");

        var action = MediaRoutePlaybackService.ResolveTap(isOnProgram: true, isLooping: false, ledger.IsOperatorPaused("clip"));
        Assert.Equal(MediaRoutePlaybackService.MediaTapAction.Resume, action);
        ledger.RecordPlay("clip");

        Assert.Equal(generationBefore, ledger.GenerationOf("clip"));
        Assert.DoesNotContain("clip", ledger.OperatorPausedAssetIds);
    }

    private static readonly IReadOnlyCollection<string> NoPaused = Array.Empty<string>();

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
