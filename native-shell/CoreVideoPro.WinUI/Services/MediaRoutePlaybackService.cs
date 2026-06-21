using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class MediaRoutePlaybackService
{
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
}
