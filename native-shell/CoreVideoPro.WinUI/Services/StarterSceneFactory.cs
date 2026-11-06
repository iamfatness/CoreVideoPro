using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Fixed layouts bind show inputs; Tiles uses the core-owned auto-reflow wall.</summary>
public static class StarterSceneFactory
{
    public static (Scene Scene, List<SourceRoute> Routes) Create(
        string id, string? template, IEnumerable<string> existingNames)
    {
        // #1070: every catalog layout has a starter, so a removed catalog scene
        // (Speaker + Slides, Panel, Closing) can be made again from New scene.
        var (name, layout) = template switch
        {
            "two-person" => ("Two people", "two-up"),
            "screen-presenter" => ("Screen + presenter", "speaker-slides"),
            "tiles" => ("CoreVideo Tiles", "dynamic-gallery"),
            "panel-grid" => ("Panel grid", "smart-grid"),
            "closing" => ("Closing", "outro"),
            _ => ("Solo", "host-focus")
        };
        var scene = new Scene
        {
            Id = id,
            Name = ScenePersistenceService.MakeUniqueSceneName(name, existingNames),
            Layout = layout,
            Automation = template == "tiles" ? "Auto-reflow Zoom gallery" : "Custom canvas",
            DynamicGallery = template == "tiles" ? new DynamicGallerySettings() : null
        };
        // Tiles membership is composed by the core, not duplicate shell routes.
        var routes = scene.DynamicGallery is null
            ? SceneRoutingService.GetStarterRoutes(scene).Select(route => route.Clone()).ToList()
            : new List<SourceRoute>();
        SceneCanvasLayoutService.EnsureCanvasRects(routes, layout);
        return (scene, routes);
    }
}
