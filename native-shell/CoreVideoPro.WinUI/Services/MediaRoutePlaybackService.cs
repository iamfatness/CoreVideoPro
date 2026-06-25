using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class MediaRoutePlaybackService
{
    public sealed record PlaybackSelection(string? MediaAssetId, bool Playing);
    public sealed record SceneRoutePlayback(string MediaPlaybackKey, bool Playing);

    public static bool ShouldPlaySceneMediaRoute(
        string mediaAssetId,
        bool isProgramScene,
        string? selectedMediaAssetId,
        bool selectedMediaAssetPlaying,
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

        return string.Equals(mediaAssetId, selectedMediaAssetId, StringComparison.Ordinal)
            ? selectedMediaAssetPlaying
            : true;
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

    public static string BuildSceneMediaPlaybackKey(string mediaAssetId, bool isProgramScene, int programTakeVersion)
    {
        var normalizedAssetId = string.IsNullOrWhiteSpace(mediaAssetId) ? "unknown" : mediaAssetId.Trim();
        return isProgramScene
            ? $"program-take:{Math.Max(0, programTakeVersion)}:media:{normalizedAssetId}"
            : $"preview:media:{normalizedAssetId}";
    }

    public static SceneRoutePlayback ResolveSceneRoutePlayback(
        string mediaAssetId,
        bool isProgramScene,
        string? selectedMediaAssetId,
        bool selectedMediaAssetPlaying,
        IReadOnlyList<SourceRoute> programRoutes,
        int programTakeVersion)
    {
        var playing = ShouldPlaySceneMediaRoute(
            mediaAssetId,
            isProgramScene,
            selectedMediaAssetId,
            selectedMediaAssetPlaying,
            programRoutes);
        var key = BuildSceneMediaPlaybackKey(
            mediaAssetId,
            isProgramScene,
            programTakeVersion);
        return new SceneRoutePlayback(key, playing);
    }
}
