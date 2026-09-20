using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class MediaRoutePlaybackServiceTests
{
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
    public void AStillGoingLiveIsNeverPromoted()
    {
        var stills = new HashSet<string>(StringComparer.Ordinal) { "logo" };
        bool SupportsPlayback(string id) => !stills.Contains(id);

        Assert.Null(MediaRoutePlaybackService.ChooseAssetToPromote(new[] { "logo" }, "logo", SupportsPlayback));
        Assert.Equal("clip", MediaRoutePlaybackService.ChooseAssetToPromote(new[] { "logo", "clip" }, "logo", SupportsPlayback));
    }

    // ---- Tap pauses/resumes a Program clip on its clock; it never restarts it (T1.2 task 2) ----

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

    // ---- AssetsEnteringProgram: the pure set diff that replaced the go-live ledger --------

    [Fact]
    public void AssetsEnteringProgram_NamesAClipThatEntersProgram()
    {
        var clip = MediaRoute("clip");
        Assert.Equal(
            new[] { "clip" },
            MediaRoutePlaybackService.AssetsEnteringProgram(Array.Empty<SourceRoute>(), new[] { clip }));
    }

    [Fact]
    public void AssetsEnteringProgram_IgnoresAClipThatStaysOnProgram()
    {
        var clip = MediaRoute("clip");
        Assert.Empty(MediaRoutePlaybackService.AssetsEnteringProgram(new[] { clip }, new[] { clip }));
    }

    [Fact]
    public void AssetsEnteringProgram_IgnoresAClipThatLeavesProgram()
    {
        var clip = MediaRoute("clip");
        Assert.Empty(MediaRoutePlaybackService.AssetsEnteringProgram(new[] { clip }, Array.Empty<SourceRoute>()));
    }

    // ---- IsPlayingOnAir reads the CORE's row, never a shell-side paused set ---------------

    [Fact]
    public void IsPlayingOnAir_ReadsTheCoreRowLive()
    {
        Assert.True(MediaRoutePlaybackService.IsPlayingOnAir("clip", [MediaSource("media:clip", "clip", "live")]));
    }

    [Fact]
    public void IsPlayingOnAir_ReadsTheCoreRowPausedAsNotPlaying()
    {
        Assert.False(MediaRoutePlaybackService.IsPlayingOnAir("clip", [MediaSource("media:clip", "clip", "paused")]));
        Assert.False(MediaRoutePlaybackService.IsPlayingOnAir("clip", [MediaSource("media:clip", "clip", "cued")]));
        Assert.False(MediaRoutePlaybackService.IsPlayingOnAir("clip", [MediaSource("media:clip", "clip", "ended")]));
    }

    [Fact]
    public void IsPlayingOnAir_ReadsTheCoreRowAbsentAsNotPlaying()
    {
        Assert.False(MediaRoutePlaybackService.IsPlayingOnAir("clip", []));
        // Another asset's row never answers for this one.
        Assert.False(MediaRoutePlaybackService.IsPlayingOnAir("clip", [MediaSource("media:other", "other", "live")]));
    }

    [Fact]
    public void IsPlayingOnAir_ALoopingBackgroundRowAnswersForItsAsset()
    {
        // A scene background is keyed background:<assetId>, not media:<assetId>.
        Assert.True(MediaRoutePlaybackService.IsPlayingOnAir("bg", [MediaSource("background:bg", "bg", "live")]));
    }

    // ---- ResolveTap now reads the core's transport state ---------------------------------

    [Fact]
    public void ResolveTap_ALiveProgramClipPauses()
    {
        Assert.Equal(
            MediaRoutePlaybackService.MediaTapAction.Pause,
            MediaRoutePlaybackService.ResolveTap(isOnProgram: true, isLooping: false, state: "live"));
    }

    [Fact]
    public void ResolveTap_APausedOrEndedProgramClipResumes()
    {
        Assert.Equal(
            MediaRoutePlaybackService.MediaTapAction.Resume,
            MediaRoutePlaybackService.ResolveTap(isOnProgram: true, isLooping: false, state: "paused"));
        Assert.Equal(
            MediaRoutePlaybackService.MediaTapAction.Resume,
            MediaRoutePlaybackService.ResolveTap(isOnProgram: true, isLooping: false, state: "ended"));
    }

    [Fact]
    public void ResolveTap_AClipNotOnProgramIsJustSelected()
    {
        Assert.Equal(
            MediaRoutePlaybackService.MediaTapAction.Select,
            MediaRoutePlaybackService.ResolveTap(isOnProgram: false, isLooping: false, state: "live"));
    }

    [Fact]
    public void ResolveTap_ALoopOnProgramIsJustSelectedNeverPaused()
    {
        Assert.Equal(
            MediaRoutePlaybackService.MediaTapAction.Select,
            MediaRoutePlaybackService.ResolveTap(isOnProgram: true, isLooping: true, state: "live"));
    }

    [Fact]
    public void ResolveTap_AnUnknownOrMissingStateIsJustSelected()
    {
        Assert.Equal(
            MediaRoutePlaybackService.MediaTapAction.Select,
            MediaRoutePlaybackService.ResolveTap(isOnProgram: true, isLooping: false, state: null));
        Assert.Equal(
            MediaRoutePlaybackService.MediaTapAction.Select,
            MediaRoutePlaybackService.ResolveTap(isOnProgram: true, isLooping: false, state: "unavailable"));
    }

    // ---- ResolveTransportState: the row lookup every caller shares -----------------------

    [Fact]
    public void ResolveTransportState_ReturnsTheRowsStateForTheAsset()
    {
        Assert.Equal("paused", MediaRoutePlaybackService.ResolveTransportState(
            "clip", [MediaSource("media:clip", "clip", "paused")]));
    }

    [Fact]
    public void ResolveTransportState_ReturnsNullWhenTheCoreHasNoRow()
    {
        // No row means "the core has no transport for this asset" (an off-Program audition),
        // never "paused" -- the difference is what keeps a snapshot from clobbering an audition.
        Assert.Null(MediaRoutePlaybackService.ResolveTransportState("clip", []));
        Assert.Null(MediaRoutePlaybackService.ResolveTransportState(null, [MediaSource("media:clip", "clip", "live")]));
    }

    [Fact]
    public void ResolveTransportState_TheRouteRowWinsWhenAnAssetIsBothARouteAndABackground()
    {
        // A background is a loop, so background:<id> is permanently "live". Letting it answer
        // for the ROUTE made the bin show a cued/paused clip as playing, made ResolveTap return
        // Pause, and sent the core a pause it correctly refuses for a loop — the UI flipped to
        // "paused" and nothing on air moved. The media:<id> row always wins.
        Assert.Equal("paused", MediaRoutePlaybackService.ResolveTransportState(
            "clip",
            [
                MediaSource("background:clip", "clip", "live"),
                MediaSource("media:clip", "clip", "paused")
            ]));
    }

    [Fact]
    public void ResolveTransportState_ABackgroundRowStillAnswersWhenTheAssetHasNoRoute()
    {
        // With no route row the background IS the asset's only source, so it is the honest answer.
        Assert.Equal("live", MediaRoutePlaybackService.ResolveTransportState(
            "bg", [MediaSource("background:bg", "bg", "live")]));
    }

    [Fact]
    public void ResolveTap_ALiveBackgroundRowCannotMakeAPausedClipLookPausable()
    {
        // The IsLoopingAsset guard does NOT cover this: it keys on MediaAsset.Kind, which is
        // "video"/"clip" for a clip that merely happens to be somebody's background.
        var state = MediaRoutePlaybackService.ResolveTransportState(
            "clip",
            [
                MediaSource("background:clip", "clip", "live"),
                MediaSource("media:clip", "clip", "paused")
            ]);

        Assert.Equal(
            MediaRoutePlaybackService.MediaTapAction.Resume,
            MediaRoutePlaybackService.ResolveTap(isOnProgram: true, isLooping: false, state));
    }

    // ---- the media bin row is an in-place scalar, not a rebuilt collection -----------------

    [Fact]
    public void MediaAssetIsPlayingIsAnObservableScalarSoTheBinIsNeverRebuiltAtSnapshotRate()
    {
        var asset = new MediaAsset { Id = "clip", Name = "Clip", Kind = "video", FilePath = "clip.mp4" };
        var changed = new List<string?>();
        asset.PropertyChanged += (_, e) => changed.Add(e.PropertyName);

        asset.IsPlaying = true;

        Assert.True(asset.IsPlaying);
        Assert.Contains(nameof(MediaAsset.IsPlaying), changed);
        Assert.Contains(nameof(MediaAsset.PlaybackLabel), changed);

        changed.Clear();
        asset.IsPlaying = true;                  // idempotent: no notification storm
        Assert.Empty(changed);
    }

    private static NativeMediaCoreMediaSource MediaSource(string sourceId, string assetId, string state) =>
        new()
        {
            SourceId = sourceId,
            MediaAssetId = assetId,
            State = state
        };

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
