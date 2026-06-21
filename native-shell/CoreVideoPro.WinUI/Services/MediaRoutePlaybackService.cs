using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class MediaRoutePlaybackService
{
    public static bool ShouldPlayMediaRoute(
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

        if (selectedMediaAssetPlaying &&
            string.Equals(selectedMediaAssetId, mediaAssetId, StringComparison.Ordinal))
        {
            return true;
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
}
