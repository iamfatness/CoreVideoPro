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
    // enters Program (or the operator restarts it) — never on a Take that leaves it on air.
    public static string BuildSceneMediaPlaybackKey(string mediaAssetId, bool loop, int goLiveGeneration)
    {
        var id = string.IsNullOrWhiteSpace(mediaAssetId) ? "unknown" : mediaAssetId.Trim();
        return loop ? $"media:{id}" : $"media:{id}:live:{Math.Max(0, goLiveGeneration)}";
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
/// asset ENTERS Program (was not routed on Program before a Take/Update, is after) or when the
/// operator presses Play on a Program-routed clip. The generation is baked into the clip's
/// playback key, so the core opens a fresh decoder (roll from frame 0) exactly then — and a
/// clip that stays on Program across a Take keeps playing.
///
/// It also owns the OPERATOR-PAUSED set: pausing a Program-routed clip adds it, playing it
/// removes it, and the clip GOING LIVE clears it (a clip entering Program rolls, spec section 2).
/// Pause is per-asset, so promoting another asset can never un-pause (and cold-restart) it.
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

    // Operator pressed Play on a program-routed clip: roll from 0.
    public void RecordRestart(string mediaAssetId)
    {
        if (string.IsNullOrWhiteSpace(mediaAssetId)) return;
        _generations[mediaAssetId] = GenerationOf(mediaAssetId) + 1;
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
