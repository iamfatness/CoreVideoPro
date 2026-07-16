using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Applies the operator's browser-overlay DSK to a temporary Program or Preview
/// scene graph. The key state remains independent from the background scene,
/// matching a professional switcher's downstream keyer.
/// </summary>
public static class BrowserOverlayProgramService
{
    public static IReadOnlyList<SourceRoute> ApplyKeyState(
        string sceneId,
        IReadOnlyList<SourceRoute> sceneRoutes,
        IReadOnlySet<string> keyedBrowserIds)
    {
        var routes = sceneRoutes.Select(route => route.Clone()).ToList();

        foreach (var browserId in keyedBrowserIds)
        {
            // Always append a temporary DSK route. Existing browser routes belong
            // to the scene graph (USK-style) and must remain independent: DSK out
            // removes only this temporary key, never an authored scene layer.
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
