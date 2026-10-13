using System.Diagnostics;
using System.Text.Json;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.ViewModels.Transport;
using CoreVideoPro.WinUI.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Graphics;

namespace CoreVideoPro.WinUI.Services;

// Runs through the existing offscreen operator-settings probe. Uses an in-memory
// show and disables interactive startup: no native core, meeting, account callback,
// device watcher, webcam registration or production preferences file.
internal static class SceneWorkspaceIntegrationProbe
{
    private sealed class MemoryStore : IProductionOutputPreferencesStore
    {
        public ProductionOutputPreferences Value {get;private set;}=new();
        public ProductionOutputPreferences? Load()=>Value;
        public void Save(ProductionOutputPreferences value)=>Value=ProductionOutputPreferencesSerializer.Deserialize(ProductionOutputPreferencesSerializer.Serialize(value))!;
    }
    internal static async Task CheckAsync(Window window,List<string> checks)
    {
        var store=new MemoryStore();
        var startup=StudioStartupBootstrap.Create(store,startInteractiveServices:false);
        await using var studio=new StudioViewModel(startup);
        var page=new SourcesPage{ViewModel=studio};window.Content=page;
        window.AppWindow.Resize(new SizeInt32(1366,768));
        await Eventually(()=>page.IsLoaded && page.Workspace is not null);
        studio.NewDynamicGalleryCommand.Execute(null);
        await Eventually(()=>page.Workspace!.Items.Any(i=>i.IsTilesWall));
        var workspace=page.Workspace!;
        var canvas=(Controls.SceneCanvasEditorControl)page.FindName("SceneCanvasEditor");
        Require(canvas.Visibility==Visibility.Visible,"Zero-route Tiles hid the canvas");
        Require(workspace.Items.Count==1 && workspace.Items[0].IsTilesWall,"Wall was not a single scene item");
        checks.Add("compiled Scene Builder displays a zero-route Tiles wall as one editable source");

        var id=studio.PreviewSceneId;studio.ActiveSceneId=id;
        // Mimic the local post-Take reconciliation without sending a command.
        ((ITransportHost)studio).RefreshPreviewRoutingState();
        Require(studio.ProgramSceneRoutes.Count==0,"Probe's initial Program projection is not the empty wall scene");
        var original=studio.ProgramScene.DynamicGallery!.GutterPercent;
        workspace.Checkpoint();studio.GalleryGutterPercent=2.25;
        Require(studio.ProgramScene.DynamicGallery.GutterPercent==original,"Preview style edit changed Program");
        workspace.UndoCommand.Execute(null);Require(studio.GalleryGutterPercent==original,"Undo did not restore wall style");
        workspace.RedoCommand.Execute(null);Require(studio.GalleryGutterPercent==2.25,"Redo lost exact wall spacing");
        workspace.SaveCommand.Execute(null);
        Require(store.Value.CustomScenes.Single(s=>s.Id==id).DynamicGallery!.GutterPercent==2.25,"Save lost the wall draft");
        Require(studio.ProgramScene.DynamicGallery.GutterPercent==original,"Save published the wall to Program");
        studio.SelectSceneCommand.Execute(studio.Scenes.First(s=>s.Id!=id).Id);
        studio.SelectSceneCommand.Execute(id);
        await Eventually(()=>studio.GalleryGutterPercent==2.25);
        Require(studio.ProgramScene.DynamicGallery.GutterPercent==original,"Recue of a saved draft changed Program");
        checks.Add("wall edit, exact value, Undo/Redo, Save and saved-scene recue preserve Program");

        workspace.Checkpoint();studio.AddCanvasSourceCommand.Execute("input-01");
        await Eventually(()=>workspace.Items.Count==2);
        var source=workspace.Items.Single(i=>!i.IsTilesWall);workspace.Selected=source;
        workspace.Checkpoint();source.SetCanvasRect(.1,.2,.4,.4);source.OpacityPercent=62.5;
        var sourceId=source.ItemId;workspace.Rename("Sample presenter");workspace.Refresh();
        Require(ReferenceEquals(workspace.Selected,source),"Refresh replaced the selected source object");
        Require(source.OpacityPercent==62.5 && source.DisplayName=="Sample presenter","Source inspector lost an exact value or alias");
        workspace.MoveBackCommand.Execute(null);
        Require(workspace.Items.Last().ItemId==sourceId,"Move back did not match front-to-back source ordering");
        workspace.ToggleVisibility(source);Require(!source.Visible,"Visibility edit did not update the source row");
        workspace.ToggleVisibility(source);workspace.ToggleLock(source);
        Require(!workspace.FitCommand.CanExecute(null),"Locked source can still run a transform command");
        workspace.ToggleLock(source);workspace.DuplicateCommand.Execute(null);
        Require(workspace.Items.Count==3,"Duplicate did not add an independent scene instance");
        workspace.RemoveCommand.Execute(null);Require(workspace.Items.Count==2,"Remove did not remove only the instance");
        workspace.UndoCommand.Execute(null);Require(workspace.Items.Count==3,"Undo did not restore the removed source instance");
        Require(studio.ProgramSceneRoutes.Count==0,"Authored Preview layers leaked into on-air wall routes");
        checks.Add("compiled scene layers preserve identity, alias, precision, order, visibility, lock, duplicate and removal history");

        foreach(var width in new[]{1366,1024})
        {
            window.AppWindow.Resize(new SizeInt32(width,768));await Task.Delay(100);page.UpdateLayout();
            var properties=(Button)page.FindName("PropertiesButton");
            Require(properties.Visibility==(page.ActualWidth<1100?Visibility.Visible:Visibility.Collapsed),"Compact inspector affordance disagrees with effective width");
            var grid=(Grid)page.FindName("WorkspaceGrid");Require(grid.ActualWidth<=page.ActualWidth+.5,"Workspace overflows horizontally");
        }
        checks.Add("compiled desktop and compact Scene Builder layout exposes the Properties drawer without page overflow");
        Require(!startup.Bridge.Running,"Isolated workspace probe started a native core");
        window.Content=null;
    }
    private static async Task Eventually(Func<bool> ready)
    {
        var clock=Stopwatch.StartNew();while(!ready()){if(clock.Elapsed>TimeSpan.FromSeconds(10))throw new InvalidOperationException("Scene Builder probe timed out");await Task.Delay(20);}
    }
    private static void Require(bool condition,string message){if(!condition)throw new InvalidOperationException(message);}
}
