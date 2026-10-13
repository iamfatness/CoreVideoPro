using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.ApplicationModel.DataTransfer;
using Windows.System;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class SourcesPage
{
    public SceneWorkspaceViewModel? Workspace { get; private set; }
    public bool TilesTransparentBackground => ViewModel?.PreviewScene.DynamicGallery?.TransparentBackground == true;
    private bool _propertiesOpen;
    private void SetWorkspace(StudioViewModel? studio)
    {
        Workspace?.Dispose(); Workspace=studio is null?null:new(studio);Bindings.Update();
        if (Workspace is not null)
        {
            Workspace.PropertyChanged += (_,e)=>
            {
                if(e.PropertyName==nameof(SceneWorkspaceViewModel.Selected)&&Workspace.Selected is {} layer)
                {
                    SceneCanvasEditor.SelectLayer(layer);
                    ConfigureInspectorTabs();
                }
            };
            ConfigureInspectorTabs();
        }
    }
    private void ConfigureInspectorTabs()
    {
        var tabs=Workspace?.IsWallSelected==true
            ? new[]{LayoutInspectorTab,StyleInspectorTab,MembersInspectorTab,TransformInspectorTab}
            : new[]{TransformInspectorTab,AppearanceInspectorTab,SourceInspectorTab};
        if(InspectorTabs.Items.Cast<PivotItem>().SequenceEqual(tabs))return;
        InspectorTabs.Items.Clear();foreach(var tab in tabs)InspectorTabs.Items.Add(tab);
        InspectorTabs.SelectedItem=tabs[0];
    }
    private void OnSourceListSelectionChanged(object sender,SelectionChangedEventArgs e)
    {if(Workspace?.Selected is {} layer)SceneCanvasEditor.SelectLayer(layer);}
    private void OnAdjustmentStarted(object? sender,EventArgs e)=>Workspace?.Checkpoint();
    private void OnDiscreteEditStarted(object sender,RoutedEventArgs e)=>Workspace?.Checkpoint();
    private void OnColorPickerOpened(object sender,RoutedEventArgs e)=>Workspace?.Checkpoint();
    private void OnGalleryToggleChanged(object sender,RoutedEventArgs e) { }
    private void OnSourceNameCommitted(object sender,RoutedEventArgs e)
    {if(sender is TextBox text && text.Text!=Workspace?.SelectedName)Workspace?.Rename(text.Text);}
    private void OnVisibilityClicked(object sender,RoutedEventArgs e)
    {if(sender is FrameworkElement {DataContext:SceneCanvasLayerViewModel item})Workspace?.ToggleVisibility(item);}
    private void OnLockClicked(object sender,RoutedEventArgs e)
    {if(sender is FrameworkElement {DataContext:SceneCanvasLayerViewModel item})Workspace?.ToggleLock(item);}
    private void OnSourceDragStarting(object sender,DragItemsStartingEventArgs e)
    {if(e.Items.FirstOrDefault() is SceneCanvasLayerViewModel item){e.Data.SetText(item.ItemId);e.Data.RequestedOperation=DataPackageOperation.Move;}}
    private void OnSourceDragOver(object sender,DragEventArgs e)
    {if(e.DataView.Contains(StandardDataFormats.Text))e.AcceptedOperation=DataPackageOperation.Move;}
    private async void OnSourceDrop(object sender,DragEventArgs e)
    {
        if(sender is not FrameworkElement{DataContext:SceneCanvasLayerViewModel target}||Workspace is null)return;
        var sceneId=ViewModel?.PreviewSceneId;var workspace=Workspace;
        var id=await e.DataView.GetTextAsync();
        if(IsLoaded&&ReferenceEquals(workspace,Workspace)&&sceneId==ViewModel?.PreviewSceneId)Workspace.DropBefore(id,target.ItemId);
    }
    private void OnSourceListKeyDown(object sender,KeyRoutedEventArgs e)
    {
        var alt=Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Menu).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        if(alt&&e.Key is VirtualKey.Up or VirtualKey.Down){Workspace?.Move(Workspace.Selected,e.Key==VirtualKey.Up?-1:1);e.Handled=true;}
        if(e.Key==VirtualKey.Delete&&Workspace?.RemoveCommand.CanExecute(null)==true){Workspace.RemoveCommand.Execute(null);e.Handled=true;}
    }
    private void OnPropertiesClicked(object sender,RoutedEventArgs e)
    { _propertiesOpen=!_propertiesOpen;ApplyWorkspaceWidth(); }
    private void OnWorkspaceSizeChanged(object sender,SizeChangedEventArgs e)=>ApplyWorkspaceWidth();
    private void ApplyWorkspaceWidth()
    {
        if(InspectorColumn is null)return;
        var compact=ActualWidth<1180;var wide=ActualWidth>=1700;
        ScenesColumn.Width=new(wide?250:228);InspectorColumn.Width=new(compact?0:wide?320:292);
        PropertiesButton.Visibility=compact?Visibility.Visible:Visibility.Collapsed;
        ClosePropertiesButton.Visibility=compact?Visibility.Visible:Visibility.Collapsed;
        InspectorDock.Visibility=!compact||_propertiesOpen?Visibility.Visible:Visibility.Collapsed;
        Grid.SetColumn(InspectorDock,compact?1:2);
        InspectorDock.Width=compact?292:double.NaN;
        InspectorDock.HorizontalAlignment=compact?HorizontalAlignment.Right:HorizontalAlignment.Stretch;
        Canvas.SetZIndex(InspectorDock,compact?10:0);
    }
    private void OnTilesTransparency(object sender,RoutedEventArgs e)
    { Workspace?.Checkpoint();ViewModel?.WorkspaceWallChange(w=>w.TransparentBackground=!w.TransparentBackground);Bindings.Update(); }
    private void OnTilesPreset(object sender,RoutedEventArgs e)
    {
        if(sender is not Button{Tag:string preset}||ViewModel is null)return;Workspace?.Checkpoint();
        ViewModel.WorkspaceWallChange(w=>
        {
            w.GlowSize=0;w.GutterPercent=.741;w.MarginPercent=.741;
            if(preset=="clean"){w.BorderShape="square";w.BorderThickness=0;w.TransparentBackground=true;}
            else{w.BorderShape="rounded";w.TransparentBackground=false;w.CornerRadius=preset=="broadcast"?12:24;
                w.BorderThickness=preset=="broadcast"?5:2;w.BorderColor=preset=="broadcast"?"#8AE0CA":"#657D87";
                w.GutterPercent=w.MarginPercent=preset=="broadcast"?1.8:1.2;w.GlowSize=preset=="broadcast"?12:0;}
        });Bindings.Update();
    }
    private void OnTilesResetStyle(object sender,RoutedEventArgs e)
    {Workspace?.Checkpoint();ViewModel?.WorkspaceWallChange(w=>{w.BorderShape="square";w.BorderColor="#000000";w.BorderThickness=0;w.CornerRadius=16;w.GlowColor="#FFFFFF";w.GlowSize=0;w.GlowIntensity=100;w.GlowSoftness=0;w.BackgroundColor="#000000";w.TransparentBackground=false;w.WallOpacity=1;});Bindings.Update();}
    private void OnApplyTileCrop(object sender,RoutedEventArgs e)
    {
        if(ViewModel is not {} studio||string.IsNullOrWhiteSpace(studio.GallerySelectedMemberId))return;
        Workspace?.Checkpoint();studio.WorkspaceWallChange(w=>
        { var item=w.Overrides.TryGetValue(studio.GallerySelectedMemberId,out var existing)?existing.Clone():new TilesMemberOverride();
            item.CropLeftPercent=studio.GalleryTileCropLeft;item.CropRightPercent=studio.GalleryTileCropRight;
            w.Overrides[studio.GallerySelectedMemberId]=item; });
    }
    private void OnTileMemberSelected(object sender,SelectionChangedEventArgs e)
    {if(sender is ListView {SelectedItem:TilesMemberViewModel member} && ViewModel is {} studio)studio.GallerySelectedMemberId=member.Id;}
    private void OnTileExclusionClicked(object sender,RoutedEventArgs e)
    {
        if(sender is not FrameworkElement {DataContext:TilesMemberViewModel item}||ViewModel is not {} studio)return;
        Workspace?.Checkpoint();try{studio.SetTilesExclusion(item.Id,!item.Excluded);}catch(ArgumentException ex){studio.CommandStatus=ex.Message;}
    }
}
