using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// What the media bin and the transport toggle should read after a core snapshot lands
/// (#535 slice 3b). The WHOLE decision, as one pure function — not a leaf helper — because the
/// bug this exists to prevent lives in the decision, not in any one predicate: a snapshot that
/// adopts the core's state too eagerly switches off an audition the operator just started, and a
/// leaf test over "is this row live" cannot see that (CLAUDE.md, "test the whole decision, not
/// the leaf"). <c>StudioViewModel</c> is still not constructible in tests, so this is the only
/// place the rule can be pinned.
///
/// The rules, in one place:
/// <list type="bullet">
/// <item>The bin's per-row <c>IsPlaying</c> scalars are rewritten only when the set of LIVE media
/// ids moved. A steady show pays one string build per snapshot and no property writes.</item>
/// <item>The CORE owns the selected asset's play flag ONLY while that asset is on PROGRAM. A clip
/// cued in Preview has a row too (state "cued"), and adopting it would clear the local audition
/// the operator started &lt;=250 ms earlier — the gesture would appear to work and then undo
/// itself on the next poll. Off Program, the flag is shell-local state.</item>
/// <item>The status line is rewritten when the core's STATE moved, not only when the boolean did:
/// paused -> ended is a real change an operator needs to see.</item>
/// </list>
/// </summary>
public static class MediaBinPlaybackProjection
{
    /// <summary>What the snapshot apply should write. Nothing here touches the UI.</summary>
    /// <param name="LiveSignature">The new live-id signature to remember.</param>
    /// <param name="RewriteBinScalars">True when the live set moved and the rows need writing.</param>
    /// <param name="RewriteSelection">True when the selected asset's flag/status must be written.</param>
    /// <param name="SelectedPlaying">The selected asset's flag (unchanged when not rewriting).</param>
    /// <param name="SelectedStatus">The selected asset's status line, or null to leave it alone.</param>
    /// <param name="SelectedTransportState">The core state the selection was projected from.</param>
    public sealed record Result(
        string LiveSignature,
        bool RewriteBinScalars,
        bool RewriteSelection,
        bool SelectedPlaying,
        string? SelectedStatus,
        string? SelectedTransportState);

    public static Result Resolve(
        IReadOnlyList<NativeMediaCoreMediaSource> mediaSources,
        string lastLiveSignature,
        string? lastSelectedTransportState,
        string? selectedMediaAssetId,
        string? selectedMediaAssetName,
        bool selectedMediaAssetPlaying)
    {
        var signature = BuildLiveSignature(mediaSources);
        var rewriteBinScalars = !string.Equals(lastLiveSignature, signature, StringComparison.Ordinal);

        // Only a PROGRAM row speaks for the selection. No row at all is an off-Program audition
        // the core knows nothing about; a Preview-only row ("cued") describes where the clip sits
        // on the bus, not whether the operator is auditioning it locally. Adopting either would
        // clobber shell-local state at poll rate.
        var selectedRow = MediaRoutePlaybackService.ResolveTransportRow(selectedMediaAssetId, mediaSources);
        if (selectedRow is not { OnProgram: true })
        {
            return new Result(
                signature,
                rewriteBinScalars,
                RewriteSelection: false,
                selectedMediaAssetPlaying,
                SelectedStatus: null,
                SelectedTransportState: null);
        }

        var selectedPlaying = string.Equals(selectedRow.State, "live", StringComparison.Ordinal);
        var stateMoved = !string.Equals(lastSelectedTransportState, selectedRow.State, StringComparison.Ordinal);
        if (selectedPlaying == selectedMediaAssetPlaying && !stateMoved)
        {
            return new Result(
                signature,
                rewriteBinScalars,
                RewriteSelection: false,
                selectedMediaAssetPlaying,
                SelectedStatus: null,
                selectedRow.State);
        }

        var name = string.IsNullOrWhiteSpace(selectedMediaAssetName)
            ? selectedMediaAssetId
            : selectedMediaAssetName;
        var status = string.IsNullOrWhiteSpace(name)
            ? null
            : selectedPlaying
                ? $"Playing {name} on Program"
                : string.Equals(selectedRow.State, "ended", StringComparison.Ordinal)
                    ? $"{name} ended on Program"
                    : $"{name} paused on Program";

        return new Result(
            signature,
            rewriteBinScalars,
            RewriteSelection: true,
            selectedPlaying,
            status,
            selectedRow.State);
    }

    /// <summary>
    /// One media-bin row's <c>IsPlaying</c>. The SAME expression the operator-event rebuild
    /// (<c>ApplyMediaSelection</c>) uses, so the selected row's indicator cannot flip just because
    /// the live signature moved: on-air state from the core, OR the shell-local audition flag for
    /// the selected asset.
    /// </summary>
    public static bool ResolveBinRowIsPlaying(
        string assetId,
        bool supportsPlayback,
        IReadOnlyList<NativeMediaCoreMediaSource> mediaSources,
        string? selectedMediaAssetId,
        bool selectedMediaAssetPlaying) =>
        (supportsPlayback && MediaRoutePlaybackService.IsPlayingOnAir(assetId, mediaSources)) ||
        (selectedMediaAssetPlaying && string.Equals(assetId, selectedMediaAssetId, StringComparison.Ordinal));

    public static string BuildLiveSignature(IReadOnlyList<NativeMediaCoreMediaSource> mediaSources)
    {
        if (mediaSources.Count == 0)
        {
            return string.Empty;
        }

        return string.Join("|", mediaSources
            .Where(row => string.Equals(row.State, "live", StringComparison.Ordinal))
            .Select(row => row.MediaAssetId ?? row.SourceId)
            .OrderBy(id => id, StringComparer.Ordinal));
    }
}
