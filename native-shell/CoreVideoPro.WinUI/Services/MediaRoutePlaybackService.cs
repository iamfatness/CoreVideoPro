using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class MediaRoutePlaybackService
{
    public sealed record PlaybackSelection(string? MediaAssetId, bool Playing);
    public sealed record SceneRoutePlayback(string MediaPlaybackKey, bool Playing);

    // Operator pause is PER-ASSET state (MediaGoLiveLedger's paused set), never "is it the
    // current selection": a Program clip plays unless the operator paused THAT clip. Moving the
    // selection (a Take promoting another asset, a click in the bin) cannot un-pause or pause it.
    public static bool ShouldPlaySceneMediaRoute(
        string mediaAssetId,
        bool isProgramScene,
        IReadOnlyCollection<string> operatorPausedAssetIds,
        IReadOnlyList<SourceRoute> programRoutes)
    {
        if (string.IsNullOrWhiteSpace(mediaAssetId))
        {
            return false;
        }

        if (!isProgramScene || !IsMediaAssetRoutedOnProgram(mediaAssetId, programRoutes))
        {
            return false;
        }

        return !operatorPausedAssetIds.Contains(mediaAssetId);
    }

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

    public static PlaybackSelection ResolvePlaybackSelection(
        string? selectedMediaAssetId,
        bool selectedMediaAssetPlaying,
        IReadOnlyList<SourceRoute> programRoutes)
    {
        var programAssetId = ResolveProgramAutoplayAssetId(selectedMediaAssetId, programRoutes);
        if (!string.IsNullOrWhiteSpace(programAssetId))
        {
            var playing = string.Equals(programAssetId, selectedMediaAssetId, StringComparison.Ordinal)
                ? selectedMediaAssetPlaying
                : true;
            return new PlaybackSelection(programAssetId, playing);
        }

        return new PlaybackSelection(selectedMediaAssetId, Playing: false);
    }

    // Which clip a Take/Update hands to the playback selection. Only assets that WENT LIVE are
    // candidates (spec section 2: go-live is the only event a source reacts to): the selected
    // asset wins if it went live, otherwise the first that did. Nothing went live -> null, so a
    // clip that merely STAYED on Program (paused or playing) is left exactly as the operator set it.
    // `supportsPlayback` filters out stills (MediaAsset.SupportsPlayback): a still going live has
    // nothing to roll, so it is never promoted into the playback selection.
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
    // other asset is a clip: paused on its first frame in Preview, rolls when it goes live.
    public static bool IsLoopingAsset(MediaAsset asset) =>
        string.Equals(asset.Kind, "background", StringComparison.OrdinalIgnoreCase);

    // A loop is a persistent source: one key on every bus, never restarted by a cut.
    // A clip's key carries its go-live generation, so it changes ONLY when the clip
    // enters Program — never on a Take that leaves it on air, and never from Pause/Play
    // (pause is a clock state on the core's decoder, not a shell-side restart, T1.2).
    public static string BuildSceneMediaPlaybackKey(string mediaAssetId, bool loop, int goLiveGeneration)
    {
        var id = string.IsNullOrWhiteSpace(mediaAssetId) ? "unknown" : mediaAssetId.Trim();
        return loop ? $"media:{id}" : $"media:{id}:live:{Math.Max(0, goLiveGeneration)}";
    }

    // What a bin-row tap or the transport toggle does to a clip: Select (move the selection,
    // audition play/pause off-Program) or Pause/Resume it on its clock, never a restart. A
    // looping asset (kind "background") is always playing and can't be usefully paused via the
    // ledger, so a tap on one is a Select too (matches "not on Program" — the toggle logic there
    // is preserved for both).
    public enum MediaTapAction
    {
        Select,
        Pause,
        Resume
    }

    public static MediaTapAction ResolveTap(bool isOnProgram, bool isLooping, bool isOperatorPaused)
    {
        if (!isOnProgram || isLooping)
        {
            return MediaTapAction.Select;
        }

        return isOperatorPaused ? MediaTapAction.Resume : MediaTapAction.Pause;
    }

    // The real on-air state of a media asset: routed on Program AND (looping OR not paused by
    // the operator). Unlike ShouldPlaySceneMediaRoute this takes the already-resolved
    // "is it on Program" boolean rather than a route list, so a caller that already has it
    // (StudioViewModel almost always does) never resolves routes twice.
    public static bool IsPlayingOnAir(
        string mediaAssetId,
        bool isOnProgram,
        bool isLooping,
        IReadOnlyCollection<string> operatorPausedAssetIds)
    {
        if (string.IsNullOrWhiteSpace(mediaAssetId) || !isOnProgram)
        {
            return false;
        }

        return isLooping || !operatorPausedAssetIds.Contains(mediaAssetId);
    }

    public static SceneRoutePlayback ResolveSceneRoutePlayback(
        string mediaAssetId,
        bool isProgramScene,
        bool loop,
        IReadOnlyCollection<string> operatorPausedAssetIds,
        IReadOnlyList<SourceRoute> programRoutes,
        int goLiveGeneration)
    {
        // A loop is a persistent source: live on every bus, never restarted by a cut.
        // A clip rolls when it goes live and shows its first frame while cued; on Program it
        // stays paused only if the operator paused THAT clip.
        var playing = loop || ShouldPlaySceneMediaRoute(
            mediaAssetId,
            isProgramScene,
            operatorPausedAssetIds,
            programRoutes);
        return new SceneRoutePlayback(BuildSceneMediaPlaybackKey(mediaAssetId, loop, goLiveGeneration), playing);
    }
}

/// <summary>
/// The go-live policy's memory: asset id -> generation. A generation advances ONLY when the
/// asset ENTERS Program (was not routed on Program before a Take/Update, is after). The
/// generation is baked into the clip's playback key, so the core opens a fresh decoder (roll
/// from frame 0) exactly then — and a clip that stays on Program across a Take keeps playing.
/// Pause/Play on an already-live clip never touches the generation: pause is a clock state on
/// the core's one decoder (T1.2), not a shell-side restart.
///
/// It also owns the OPERATOR-PAUSED set: pausing a Program-routed clip adds it, playing it
/// removes it, and the clip GOING LIVE clears it (a clip entering Program rolls, spec section 2).
/// Pause is per-asset, so promoting another asset can never un-pause it.
/// </summary>
public sealed class MediaGoLiveLedger
{
    private readonly Dictionary<string, int> _generations = new(StringComparer.Ordinal);
    private readonly HashSet<string> _operatorPaused = new(StringComparer.Ordinal);

    public IReadOnlyCollection<string> OperatorPausedAssetIds => _operatorPaused;

    public bool IsOperatorPaused(string mediaAssetId) => _operatorPaused.Contains(mediaAssetId);

    // Operator paused a Program-routed clip.
    public void RecordPause(string mediaAssetId)
    {
        if (string.IsNullOrWhiteSpace(mediaAssetId)) return;
        _operatorPaused.Add(mediaAssetId);
    }

    // Operator played it again.
    public void RecordPlay(string mediaAssetId)
    {
        if (string.IsNullOrWhiteSpace(mediaAssetId)) return;
        _operatorPaused.Remove(mediaAssetId);
    }

    public int GenerationOf(string mediaAssetId) =>
        _generations.TryGetValue(mediaAssetId, out var generation) ? generation : 0;

    // Returns the asset ids that went live (were not routed on Program before, are now).
    public IReadOnlyList<string> RecordTake(IReadOnlyList<SourceRoute> previousProgramRoutes, IReadOnlyList<SourceRoute> programRoutes)
    {
        var before = ProgramMediaAssetIds(previousProgramRoutes);
        var wentLive = new List<string>();
        foreach (var assetId in ProgramMediaAssetIds(programRoutes))
        {
            if (before.Contains(assetId)) continue;
            _generations[assetId] = GenerationOf(assetId) + 1;
            _operatorPaused.Remove(assetId);  // a clip entering Program rolls
            wentLive.Add(assetId);
        }
        return wentLive;
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
}
