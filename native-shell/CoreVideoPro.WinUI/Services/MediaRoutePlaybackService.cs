using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// What the shell still decides about media routes after #535 slice 3b: which asset a route
/// names, whether the operator marked it a loop, and which assets a Take brought onto Program.
///
/// What it no longer decides — and must never decide again — is PLAY STATE. There is no
/// playback key, no go-live generation and no operator-paused set here. The core owns a media
/// source's transport (cued / live / paused / ended) and publishes it per source in the
/// snapshot's <c>mediaSources</c> node; the shell reads that back.
/// </summary>
public static class MediaRoutePlaybackService
{
    public static bool IsMediaAssetRoutedOnProgram(
        string mediaAssetId,
        IReadOnlyList<SourceRoute> programRoutes)
    {
        if (string.IsNullOrWhiteSpace(mediaAssetId))
        {
            return false;
        }

        return programRoutes.Any(route =>
            route.Mode == SourceRouteMode.Fixed &&
            ShowInputRosterService.TryGetMediaAssetId(route.ParticipantId, out var routeMediaAssetId) &&
            string.Equals(routeMediaAssetId, mediaAssetId, StringComparison.Ordinal));
    }

    // True iff the SELECTED asset specifically was on Program before this Take/Update and is
    // not on Program after it (T1.2 task 3, controller ruling: the selected clip must stop
    // reading "playing" the moment it leaves Program, not stay stuck showing its last state).
    public static bool SelectedAssetLeftProgram(
        string? selectedMediaAssetId,
        IReadOnlyList<SourceRoute> previousProgramRoutes,
        IReadOnlyList<SourceRoute> currentProgramRoutes)
    {
        if (string.IsNullOrWhiteSpace(selectedMediaAssetId))
        {
            return false;
        }

        return IsMediaAssetRoutedOnProgram(selectedMediaAssetId, previousProgramRoutes) &&
            !IsMediaAssetRoutedOnProgram(selectedMediaAssetId, currentProgramRoutes);
    }

    public static string? ResolveProgramAutoplayAssetId(
        string? selectedMediaAssetId,
        IReadOnlyList<SourceRoute> programRoutes)
    {
        if (!string.IsNullOrWhiteSpace(selectedMediaAssetId) &&
            IsMediaAssetRoutedOnProgram(selectedMediaAssetId, programRoutes))
        {
            return selectedMediaAssetId;
        }

        foreach (var route in programRoutes)
        {
            if (route.Mode == SourceRouteMode.Fixed &&
                ShowInputRosterService.TryGetMediaAssetId(route.ParticipantId, out var mediaAssetId) &&
                !string.IsNullOrWhiteSpace(mediaAssetId))
            {
                return mediaAssetId;
            }
        }

        return null;
    }

    /// <summary>
    /// The media assets a Take/Update brought ONTO Program: present in
    /// <paramref name="programRoutes"/> and absent from <paramref name="previousProgramRoutes"/>.
    ///
    /// This is the pure set diff that replaced <c>MediaGoLiveLedger.RecordTake</c>. It stores
    /// nothing — no generation counter, no paused set — because the core decides at command
    /// time what an arriving media source does (roll from 0 on go-live, hold across a cut).
    /// The shell needs the list only to move the bin SELECTION onto the clip that went live.
    /// </summary>
    public static IReadOnlyList<string> AssetsEnteringProgram(
        IReadOnlyList<SourceRoute> previousProgramRoutes,
        IReadOnlyList<SourceRoute> programRoutes)
    {
        var before = ProgramMediaAssetIds(previousProgramRoutes);
        var entering = new List<string>();
        foreach (var assetId in ProgramMediaAssetIds(programRoutes))
        {
            if (before.Contains(assetId) || entering.Contains(assetId, StringComparer.Ordinal))
            {
                continue;
            }

            entering.Add(assetId);
        }

        return entering;
    }

    private static HashSet<string> ProgramMediaAssetIds(IReadOnlyList<SourceRoute> routes)
    {
        var ids = new HashSet<string>(StringComparer.Ordinal);
        foreach (var route in routes)
        {
            if (route.Mode == SourceRouteMode.Fixed &&
                ShowInputRosterService.TryGetMediaAssetId(route.ParticipantId, out var assetId) &&
                !string.IsNullOrWhiteSpace(assetId))
            {
                ids.Add(assetId);
            }
        }

        return ids;
    }

    // Which media assets a Take promotes into the bin selection: only assets that WENT LIVE are
    // candidates (spec section 2: go-live is the only event a source reacts to). The selected
    // asset wins if it went live, otherwise the first that did. Nothing went live -> null, so a
    // clip that merely STAYED on Program is left exactly as the operator set it.
    // `supportsPlayback` filters out stills (MediaAsset.SupportsPlayback): a still going live
    // has nothing to roll, so it is never promoted into the playback selection.
    public static string? ChooseAssetToPromote(
        IReadOnlyList<string> wentLiveMediaAssetIds,
        string? selectedMediaAssetId,
        Func<string, bool>? supportsPlayback = null)
    {
        var candidates = wentLiveMediaAssetIds
            .Where(id => !string.IsNullOrWhiteSpace(id) && (supportsPlayback is null || supportsPlayback(id)))
            .ToList();
        if (!string.IsNullOrWhiteSpace(selectedMediaAssetId) &&
            candidates.Contains(selectedMediaAssetId, StringComparer.Ordinal))
        {
            return selectedMediaAssetId;
        }

        return candidates.FirstOrDefault();
    }

    // MediaAsset carries no loop flag; a scene background is the only looping kind. Every
    // other asset is a clip: cued on its first frame in Preview, rolling once it goes live.
    public static bool IsLoopingAsset(MediaAsset asset) => IsLoopingKind(asset.Kind);

    public static bool IsLoopingKind(string? kind) =>
        string.Equals(kind, "background", StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// What a bin-row tap or the transport toggle does to an asset: Select (move the selection,
    /// audition play/pause off-Program) or Pause/Resume its source on the CORE, never a restart.
    /// A looping asset (kind "background") is always playing and has nothing useful to pause,
    /// so a tap on one is a Select too.
    /// </summary>
    public enum MediaTapAction
    {
        Select,
        Pause,
        Resume
    }

    /// <param name="state">
    /// The core's transport state for this asset's source ("cued" | "live" | "paused" |
    /// "ended"), or null when the core publishes no row for it. An unknown/absent state is a
    /// Select: there is nothing to pause, and inventing a transport gesture against a source
    /// the core does not have is exactly the shell-decides-play-state mistake this slice retires.
    /// </param>
    public static MediaTapAction ResolveTap(bool isOnProgram, bool isLooping, string? state)
    {
        if (!isOnProgram || isLooping)
        {
            return MediaTapAction.Select;
        }

        if (string.Equals(state, "live", StringComparison.Ordinal))
        {
            return MediaTapAction.Pause;
        }

        return string.Equals(state, "paused", StringComparison.Ordinal) ||
            string.Equals(state, "ended", StringComparison.Ordinal)
                ? MediaTapAction.Resume
                : MediaTapAction.Select;
    }

    /// <summary>
    /// The real on-air state of a media asset, read from the CORE's rows: true iff a row for
    /// this asset reports state "live". An absent row is NOT playing — the core publishes
    /// <c>mediaSources</c> unconditionally, so absence means "not routed", never "unknown".
    /// </summary>
    public static bool IsPlayingOnAir(
        string mediaAssetId,
        IReadOnlyList<NativeMediaCoreMediaSource> mediaSources) =>
        string.Equals(ResolveTransportState(mediaAssetId, mediaSources), "live", StringComparison.Ordinal);

    /// <summary>
    /// The core's transport state for an asset, or null when it publishes no row for it. A
    /// routed asset is keyed <c>media:&lt;assetId&gt;</c> and a scene background
    /// <c>background:&lt;assetId&gt;</c>; both are matched by <c>mediaAssetId</c> so a caller
    /// never has to know which namespace the core used. When both exist (an asset used as a
    /// route AND as a background) the LIVE one wins — on-air beats cued.
    /// </summary>
    public static string? ResolveTransportState(
        string? mediaAssetId,
        IReadOnlyList<NativeMediaCoreMediaSource> mediaSources)
    {
        if (string.IsNullOrWhiteSpace(mediaAssetId) || mediaSources.Count == 0)
        {
            return null;
        }

        string? state = null;
        foreach (var row in mediaSources)
        {
            if (!string.Equals(row.MediaAssetId, mediaAssetId, StringComparison.Ordinal))
            {
                continue;
            }

            if (string.Equals(row.State, "live", StringComparison.Ordinal))
            {
                return row.State;
            }

            state ??= row.State;
        }

        return state;
    }
}
