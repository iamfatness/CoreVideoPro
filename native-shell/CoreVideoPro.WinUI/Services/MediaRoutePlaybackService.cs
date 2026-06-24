using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class MediaRoutePlaybackService
{
    public sealed record PlaybackSelection(string? MediaAssetId, bool Playing);

    public static bool ShouldPlaySceneMediaRoute(
        string mediaAssetId,
        bool isProgramScene,
        IReadOnlyList<SourceRoute> programRoutes)
    {
        if (string.IsNullOrWhiteSpace(mediaAssetId))
        {
            return false;
        }

        return isProgramScene && IsMediaAssetRoutedOnProgram(mediaAssetId, programRoutes);
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
            return new PlaybackSelection(programAssetId, Playing: true);
        }

        return new PlaybackSelection(selectedMediaAssetId, Playing: false);
    }
}
