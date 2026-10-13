using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Scene item identity and native order. A wall occupies a reserved order band.</summary>
public static class SceneWorkspacePolicy
{
    public static IReadOnlyList<NormalizedCanvasRect> ThumbnailRects(Scene scene,IReadOnlyList<SourceRoute> routes)
    {
        var copy=routes.Select(r=>r.Clone()).ToList();SceneCanvasLayoutService.EnsureCanvasRects(copy,scene.Layout);
        return copy.Where(r=>r.Visible&&r.CanvasRect is not null).Select(r=>r.CanvasRect!.Clone()).ToList();
    }
    public const int OrderStride = 256; // native wall background, glows and members occupy at most 129 orders
    public static bool IsGeneratedTileRoute(string sceneId, string routeId) =>
        routeId.StartsWith(sceneId + "-tile-", StringComparison.Ordinal);
    public static bool IsAuthoredRoute(Scene scene, SourceRoute route) => scene.DynamicGallery is null ||
        (route.WorkspaceAuthored && !IsGeneratedTileRoute(scene.Id, route.Id));
    public static SourceRoute EmptySceneRoute(string id) => new() {Id="empty:"+id,Mode=SourceRouteMode.None,
        CanvasRect=new(){Width=1,Height=1},Opacity=0,Visible=false};
    public static NormalizedCanvasRect NormalizeRect(NormalizedCanvasRect? rect)
    {
        var value=rect?.Clone()??new();
        if(!double.IsFinite(value.X))value.X=0;if(!double.IsFinite(value.Y))value.Y=0;
        if(!double.IsFinite(value.Width))value.Width=1;if(!double.IsFinite(value.Height))value.Height=1;
        value.Clamp(.0001,.0001);return value;
    }

    public static SourceRoute WallRoute(Scene scene) => new()
    {
        Id = "tiles:" + scene.Id, DisplayName = "CoreVideo Tiles", Mode = SourceRouteMode.None,
        CanvasRect = NormalizeRect(scene.DynamicGallery!.WallRect), Opacity = scene.DynamicGallery.WallOpacity,
        Visible = scene.DynamicGallery.WallVisible, EditorLocked = scene.DynamicGallery.WallLocked,
        ZIndex = scene.DynamicGallery.WallOrder
    };

    public static void NormalizeOrder(List<SourceRoute> routes, DynamicGallerySettings? wall)
    {
        var ordered = routes.OrderBy(r => r.ZIndex).ToList();
        var wallIndex = wall is null ? -1 : ordered.Count(r => r.ZIndex < wall.WallOrder);
        for (var i = 0; i < ordered.Count; i++)
            ordered[i].ZIndex = (i + (wallIndex >= 0 && i >= wallIndex ? 1 : 0)) * OrderStride;
        if (wall is not null) wall.WallOrder = wallIndex * OrderStride;
    }
}
