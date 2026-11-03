using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>New scenes bind show inputs, never whichever guest happens to be speaking.</summary>
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
            "panel-grid" => ("Panel grid", "smart-grid"),
            "closing" => ("Closing", "outro"),
            _ => ("Solo", "host-focus")
        };
        var scene = new Scene
        {
            Id = id,
            Name = ScenePersistenceService.MakeUniqueSceneName(name, existingNames),
            Layout = layout,
            Automation = "Custom canvas"
        };
        var routes = SceneRoutingService.GetStarterRoutes(scene).Select(route => route.Clone()).ToList();
        SceneCanvasLayoutService.EnsureCanvasRects(routes, layout);
        return (scene, routes);
    }
}
