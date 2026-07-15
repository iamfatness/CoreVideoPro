using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Applies the operator's browser-overlay keys to a temporary Program scene graph.
/// The on-air state is independent from the queued Preview scene and follows Program
/// across scene takes, matching the built-in lower-third control.
/// </summary>
public static class BrowserOverlayProgramService
{
    public static IReadOnlyList<SourceRoute> ApplyOnAirState(
        string sceneId,
        IReadOnlyList<SourceRoute> sceneRoutes,
        IReadOnlySet<string> onAirBrowserIds)
    {
        var routes = sceneRoutes.Select(route => route.Clone()).ToList();

        foreach (var route in routes)
        {
            if (route.Mode != SourceRouteMode.CaptureDevice ||
                route.CaptureDeviceId is not { } browserId ||
                !browserId.StartsWith("browser:", StringComparison.Ordinal))
            {
                continue;
            }

            route.Opacity = 0;
        }

        foreach (var browserId in onAirBrowserIds)
        {
            var existing = routes.LastOrDefault(route =>
                route.Mode == SourceRouteMode.CaptureDevice &&
                string.Equals(route.CaptureDeviceId, browserId, StringComparison.Ordinal));
            if (existing is not null)
            {
                existing.Opacity = 1;
                continue;
            }

            routes.Add(new SourceRoute
            {
                Id = $"{sceneId}-browser-key-{routes.Count + 1}",
                Mode = SourceRouteMode.CaptureDevice,
                CaptureDeviceId = browserId,
                AudioRole = SourceAudioRole.Isolated,
                CanvasRect = new NormalizedCanvasRect { X = 0, Y = 0, Width = 1, Height = 1 },
                FitMode = "fit",
                SourceFramingModified = true,
                Opacity = 1,
                ZIndex = routes.Count
            });
        }

        return routes;
    }
}
