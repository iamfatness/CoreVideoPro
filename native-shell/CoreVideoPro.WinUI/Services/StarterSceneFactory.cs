using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>New scenes bind show inputs, never whichever guest happens to be speaking.</summary>
public static class StarterSceneFactory
{
    public static (Scene Scene, List<SourceRoute> Routes) Create(
        string id, string? template, IEnumerable<string> existingNames)
    {
        var (name, layout) = template switch
        {
            "two-person" => ("Two people", "two-up"),
            "screen-presenter" => ("Screen + presenter", "speaker-slides"),
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
