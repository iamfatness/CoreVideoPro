using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// The WHOLE snapshot-apply decision for the media bin (#535 slice 3b review round 1). These
/// exist because the leaf tests could not see the bug: "is this row live" is correct in
/// isolation while the decision built on it still clobbers an audition the operator just started.
/// </summary>
public sealed class MediaBinPlaybackProjectionTests
{
    [Fact]
    public void ACuedPreviewRowNeverClearsALocalAudition()
    {
        // The reported regression: the operator taps Play on a clip cued in Preview. The clip
        // HAS a core row (state "cued"), so "only act when a row exists" was not enough — the
        // next poll (<=250 ms later) switched the local audition off and rewrote the status.
        var result = MediaBinPlaybackProjection.Resolve(
            [Row("media:clip", "clip", "cued", onProgram: false, onPreview: true)],
            lastLiveSignature: string.Empty,
            lastSelectedTransportState: null,
            selectedMediaAssetId: "clip",
            selectedMediaAssetName: "Clip",
            selectedMediaAssetPlaying: true);

        Assert.False(result.RewriteSelection);
        Assert.True(result.SelectedPlaying);
        Assert.Null(result.SelectedStatus);
    }

    [Fact]
    public void NoRowAtAllNeverClearsALocalAudition()
    {
        var result = MediaBinPlaybackProjection.Resolve(
            [],
            lastLiveSignature: string.Empty,
            lastSelectedTransportState: null,
            selectedMediaAssetId: "clip",
            selectedMediaAssetName: "Clip",
            selectedMediaAssetPlaying: true);

        Assert.False(result.RewriteSelection);
        Assert.True(result.SelectedPlaying);
    }

    [Fact]
    public void AProgramRowOwnsTheSelectedFlag()
    {
        var live = MediaBinPlaybackProjection.Resolve(
            [Row("media:clip", "clip", "live", onProgram: true, onPreview: false)],
            string.Empty, null, "clip", "Clip", selectedMediaAssetPlaying: false);

        Assert.True(live.RewriteSelection);
        Assert.True(live.SelectedPlaying);
        Assert.Equal("Playing Clip on Program", live.SelectedStatus);

        var paused = MediaBinPlaybackProjection.Resolve(
            [Row("media:clip", "clip", "paused", onProgram: true, onPreview: false)],
            string.Empty, null, "clip", "Clip", selectedMediaAssetPlaying: true);

        Assert.True(paused.RewriteSelection);
        Assert.False(paused.SelectedPlaying);
        Assert.Equal("Clip paused on Program", paused.SelectedStatus);
    }

    [Fact]
    public void TheStatusIsRewrittenWhenTheSTATEMovesEvenThoughTheBooleanDoesNot()
    {
        // paused -> ended: still "not playing", but the operator needs to see the clip ran out.
        var result = MediaBinPlaybackProjection.Resolve(
            [Row("media:clip", "clip", "ended", onProgram: true, onPreview: false)],
            lastLiveSignature: string.Empty,
            lastSelectedTransportState: "paused",
            selectedMediaAssetId: "clip",
            selectedMediaAssetName: "Clip",
            selectedMediaAssetPlaying: false);

        Assert.True(result.RewriteSelection);
        Assert.False(result.SelectedPlaying);
        Assert.Equal("Clip ended on Program", result.SelectedStatus);
        Assert.Equal("ended", result.SelectedTransportState);
    }

    [Fact]
    public void ASettledSelectionWritesNothing()
    {
        var result = MediaBinPlaybackProjection.Resolve(
            [Row("media:clip", "clip", "live", onProgram: true, onPreview: false)],
            lastLiveSignature: "clip",
            lastSelectedTransportState: "live",
            selectedMediaAssetId: "clip",
            selectedMediaAssetName: "Clip",
            selectedMediaAssetPlaying: true);

        Assert.False(result.RewriteSelection);
        Assert.False(result.RewriteBinScalars);
    }

    [Fact]
    public void TheBinScalarsAreRewrittenOnlyWhenTheLiveSetMoves()
    {
        IReadOnlyList<NativeMediaCoreMediaSource> rows =
            [Row("media:clip", "clip", "live", onProgram: true, onPreview: false)];

        var first = MediaBinPlaybackProjection.Resolve(rows, string.Empty, null, null, null, false);
        Assert.True(first.RewriteBinScalars);
        Assert.Equal("clip", first.LiveSignature);

        var second = MediaBinPlaybackProjection.Resolve(rows, first.LiveSignature, null, null, null, false);
        Assert.False(second.RewriteBinScalars);
    }

    [Fact]
    public void ABinRowsIndicatorIsTheSameExpressionAsTheOperatorRebuild()
    {
        IReadOnlyList<NativeMediaCoreMediaSource> rows =
            [Row("media:other", "other", "live", onProgram: true, onPreview: false)];

        // On air from the core.
        Assert.True(MediaBinPlaybackProjection.ResolveBinRowIsPlaying(
            "other", supportsPlayback: true, rows, selectedMediaAssetId: null, selectedMediaAssetPlaying: false));
        // The shell-local audition of the SELECTED asset survives a live-signature move.
        Assert.True(MediaBinPlaybackProjection.ResolveBinRowIsPlaying(
            "clip", supportsPlayback: true, rows, "clip", selectedMediaAssetPlaying: true));
        // A still never reads as playing off its own row.
        Assert.False(MediaBinPlaybackProjection.ResolveBinRowIsPlaying(
            "other", supportsPlayback: false, rows, selectedMediaAssetId: null, selectedMediaAssetPlaying: false));
    }

    [Fact]
    public void ALiveBackgroundRowDoesNotSpeakForAClipThatIsAlsoARoute()
    {
        // A scene background is a loop, so its row is permanently live. It must not decide the
        // route's state, or the bin shows a cued clip as playing and the tap sends a pause the
        // core refuses.
        var result = MediaBinPlaybackProjection.Resolve(
            [
                Row("background:clip", "clip", "live", onProgram: true, onPreview: false),
                Row("media:clip", "clip", "paused", onProgram: true, onPreview: false)
            ],
            string.Empty, null, "clip", "Clip", selectedMediaAssetPlaying: true);

        Assert.True(result.RewriteSelection);
        Assert.False(result.SelectedPlaying);
        Assert.Equal("Clip paused on Program", result.SelectedStatus);
    }

    private static NativeMediaCoreMediaSource Row(
        string sourceId,
        string assetId,
        string state,
        bool onProgram,
        bool onPreview) =>
        new()
        {
            SourceId = sourceId,
            MediaAssetId = assetId,
            State = state,
            OnProgram = onProgram,
            OnPreview = onPreview
        };
}
