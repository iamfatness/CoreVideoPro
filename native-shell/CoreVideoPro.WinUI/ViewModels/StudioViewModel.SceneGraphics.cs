using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    internal Func<SceneGraphic,string> WorkspaceRasterizeGraphic {get;set;}=graphic=>SceneGraphicRasterizer.Materialize(graphic);
    private void RefreshWorkspaceMediaCatalog()
    {
        var draft=WorkspaceRoutes().Select(r=>r.Clone()).ToList();
        var wall=PreviewScene.DynamicGallery?.Clone();var background=WorkspaceBackgroundId;
        RefreshMediaBin();WorkspaceRestore(draft,wall,background);
    }
    public async Task<SourceRoute?> WorkspaceImportImageAsync(string sceneId)
    {
        var picker=new Windows.Storage.Pickers.FileOpenPicker();
        foreach(var ext in new[]{".png",".jpg",".jpeg",".gif"})picker.FileTypeFilter.Add(ext);
        WinRT.Interop.InitializeWithWindow.Initialize(picker,App.MainWindowHandle);
        var file=await picker.PickSingleFileAsync();
        if(file is null||PreviewSceneId!=sceneId)return null;
        var asset=_mediaBinService.ImportFiles([file.Path]).FirstOrDefault();
        if(asset is null)return null;
        RefreshWorkspaceMediaCatalog();return WorkspaceAddFeed("media:"+asset.Id);
    }
    public SourceRoute WorkspaceAddFeed(string value)
    {
        var route=SceneRoutingService.BuildAddedSourceRoute(PreviewSceneId,WorkspaceRoutes().Count,
            value.StartsWith("media:",StringComparison.Ordinal)?"media":value,
            value.StartsWith("media:",StringComparison.Ordinal)?value[6..]:null);
        if(value.StartsWith("zoom:",StringComparison.Ordinal)){route.Mode=SourceRouteMode.Fixed;route.ParticipantId=value[5..];}
        route.ZIndex=WorkspaceRoutes().Select(r=>r.ZIndex).Append(PreviewScene.DynamicGallery?.WallOrder??-256).Max()+SceneWorkspacePolicy.OrderStride;
        WorkspaceRoutes().Add(route);WorkspaceRefresh();return route;
    }
    public SourceRoute WorkspaceAddGraphic(string kind)
    {
        var graphic=new SceneGraphic(kind,Color:kind=="color"?"#101820":"#FFFFFF");
        var mediaId=WorkspaceRasterizeGraphic(graphic);
        RefreshWorkspaceMediaCatalog();
        var route=new SourceRoute{Id="graphic-"+Guid.NewGuid().ToString("N"),Mode=SourceRouteMode.Fixed,
            ParticipantId="media:"+mediaId,WorkspaceAuthored=true,Graphic=graphic,
            DisplayName=kind=="text"?"Text":"Color background",FitMode="stretch",SourceFramingModified=true,
            CanvasRect=kind=="text"?new(){X=.15,Y=.4,Width=.7,Height=.14}:new(){Width=1,Height=1},
            ZIndex=WorkspaceRoutes().Select(r=>r.ZIndex).Append(PreviewScene.DynamicGallery?.WallOrder??-256).Max()+SceneWorkspacePolicy.OrderStride};
        WorkspaceRoutes().Add(route);WorkspaceRefresh();return route;
    }
    public void WorkspaceEditGraphic(string itemId,SceneGraphic graphic)
    {
        var route=WorkspaceRoutes().FirstOrDefault(r=>r.Id==itemId);
        if(route is null||route.EditorLocked||route.Graphic==graphic)return;
        var mediaId=WorkspaceRasterizeGraphic(graphic);
        // Refreshing Media can rebuild the route projection. Preserve the editable
        // revision and reacquire by identity; never write to a detached route object.
        RefreshWorkspaceMediaCatalog();
        route=WorkspaceRoutes().Single(r=>r.Id==itemId);
        route.Graphic=graphic;route.ParticipantId="media:"+mediaId;WorkspaceRefresh();
    }
}
