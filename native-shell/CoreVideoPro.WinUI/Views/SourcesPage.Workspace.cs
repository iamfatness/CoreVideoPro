using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using DispatcherQueuePriority = Microsoft.UI.Dispatching.DispatcherQueuePriority;
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
    public Visibility RoundedTilesVisibility=>ViewModel?.GalleryBorderShape=="rounded"?Visibility.Visible:Visibility.Collapsed;
    public Visibility OpaqueWallVisibility=>TilesTransparentBackground?Visibility.Collapsed:Visibility.Visible;
    public Visibility CustomAspectVisibility=>ViewModel?.GalleryTileAspect=="custom"?Visibility.Visible:Visibility.Collapsed;
    public double WallOpacityPercent {get=>(ViewModel?.PreviewScene.DynamicGallery?.WallOpacity??1)*100;set{if(value==WallOpacityPercent)return;if(Workspace?.CanEditSelected==true&&ViewModel is {} studio&&double.IsFinite(value)&&value is >=0 and <=100)studio.WorkspaceWallChange(w=>w.WallOpacity=value/100);}}
    private void OnTileInclusionClicked(object sender,RoutedEventArgs e)
    {if(sender is CheckBox {DataContext:TilesMemberViewModel member} check)Workspace?.IncludeMember(member,check.IsChecked==true);}
    private void OnResetTileCrop(object sender,RoutedEventArgs e)=>Workspace?.MemberEditor.ResetCrop();
    private void OnUnpinTile(object sender,RoutedEventArgs e){if(Workspace is {} workspace)workspace.MemberEditor.Pinned=false;}
    private void OnSetLiveWallBackground(object sender,RoutedEventArgs e)
    {if(ViewModel is not {} studio||Workspace?.CanEditSelected!=true||Workspace.SelectedMember?.IsAvailable!=true)return;Workspace.Checkpoint();studio.SetTilesBackground(studio.GalleryBackgroundColor,studio.GallerySelectedMemberId);}
    private void OnClearLiveWallBackground(object sender,RoutedEventArgs e)
    {if(ViewModel is not {} studio||Workspace?.CanEditSelected!=true)return;Workspace.Checkpoint();studio.SetTilesBackground(studio.GalleryBackgroundColor,string.Empty);}
    private bool _backgroundChoosing;
    private string? _backgroundChoiceSceneId;
    private void OnBackgroundChoiceOpened(object sender,object e){_backgroundChoosing=true;_backgroundChoiceSceneId=ViewModel?.PreviewSceneId;}
    private void OnBackgroundChoiceClosed(object sender,object e){_backgroundChoosing=false;}
    private void OnBackgroundChoiceChanged(object sender,SelectionChangedEventArgs e)
    {if(_backgroundChoosing&&ViewModel?.PreviewSceneId==_backgroundChoiceSceneId&&sender is ComboBox {SelectedItem:TilesMemberViewModel member}&&member.IsAvailable&&Workspace is {} workspace)workspace.SelectedMember=member;}
    private void OnGraphicTextCommitted(object sender,RoutedEventArgs e)
    {if(sender is TextBox {Tag:string itemId} field&&Workspace?.Selected?.ItemId==itemId&&field.Text!=Workspace.GraphicText)Workspace.GraphicText=field.Text;}
    private void OnGraphicColorCommitted(object sender,RoutedEventArgs e)
    {if(sender is TextBox {Tag:string itemId} field&&Workspace?.Selected?.ItemId==itemId&&field.Text!=Workspace.GraphicColor)Workspace.GraphicColor=field.Text;}
    private (string SceneId,string Id,string Text,bool Color)? _pendingGraphicEdit;
    private (string SceneId,string Id,string Text)? _pendingAliasEdit;
    private void OnSceneAliasChanged(object sender,TextChangedEventArgs e)
    {
        if(sender is TextBox {Tag:string id} field&&Workspace?.Selected?.ItemId==id&&field.Text!=Workspace.SelectedAlias)
            _pendingAliasEdit=(ViewModel!.PreviewSceneId,id,field.Text);
    }
    public bool CanTakeWorkspace=>ViewModel?.CanTake==true||_pendingGraphicEdit.HasValue;
    private int _graphicEditVersion;
    internal string GraphicEditProbeState=>$"pending={_pendingGraphicEdit?.Text}; pageSelected={Workspace?.Selected?.ItemId}; editable={Workspace?.CanEditSelected}; loaded={IsLoaded}";
    internal void CommitPendingGraphicEdits()=>FlushGraphicEdit();
    private async void OnGraphicContentChanged(object sender,TextChangedEventArgs e)
    {
        if(sender is not TextBox {Tag:string id} field||Workspace?.Selected?.ItemId!=id)return;
        var color=field.Header?.ToString()=="Color (hex)";
        if(field.Text==(color?Workspace.GraphicColor:Workspace.GraphicText))return;
        _pendingGraphicEdit=(ViewModel!.PreviewSceneId,id,field.Text,color);PropertyChanged?.Invoke(this,new(nameof(CanTakeWorkspace)));var version=++_graphicEditVersion;
        await Task.Delay(180);
        if(IsLoaded&&version==_graphicEditVersion)FlushGraphicEdit();
    }
    private void FlushGraphicEdit()
    {
        // TextChanged may still be queued when Save or a selection change runs.
        // Capture the current scoped fields before any commit refreshes bindings.
        var sceneId=ViewModel?.PreviewSceneId;
        var itemId=Workspace?.Selected?.ItemId;
        var visibleText=Workspace?.IsText==true&&GraphicTextField?.Tag?.ToString()==itemId
            &&GraphicTextField.Text!=Workspace.GraphicText?GraphicTextField.Text:null;
        var visibleColor=Workspace?.IsGraphic==true&&GraphicColorField?.Tag?.ToString()==itemId
            &&GraphicColorField.Text!=Workspace.GraphicColor?GraphicColorField.Text:null;
        if(_pendingAliasEdit is {} alias){_pendingAliasEdit=null;Workspace?.RenameItem(alias.SceneId,alias.Id,alias.Text);}
        ++_graphicEditVersion;var pending=_pendingGraphicEdit;_pendingGraphicEdit=null;
        if(pending is {} edit)Workspace?.CommitGraphicEdit(edit.SceneId,edit.Id,edit.Text,edit.Color);
        if(sceneId is not null&&itemId is not null)
        {
            if(visibleText is not null)Workspace?.CommitGraphicEdit(sceneId,itemId,visibleText,false);
            if(visibleColor is not null)Workspace?.CommitGraphicEdit(sceneId,itemId,visibleColor,true);
        }
        PropertyChanged?.Invoke(this,new(nameof(CanTakeWorkspace)));
    }
    private void OnSaveWorkspace(object sender,RoutedEventArgs e)
    {FlushGraphicEdit();Workspace?.SaveCommand.Execute(null);}
    private void OnUndoWorkspace(object sender,RoutedEventArgs e)
    {FlushGraphicEdit();if(Workspace?.UndoAvailable==true)Workspace.UndoCommand.Execute(null);}
    private void OnRedoWorkspace(object sender,RoutedEventArgs e)
    {FlushGraphicEdit();if(Workspace?.RedoAvailable==true)Workspace.RedoCommand.Execute(null);}
    private void OnTakeWorkspace(object sender,RoutedEventArgs e)
    {FlushGraphicEdit();if(ViewModel?.TakeCommand.CanExecute(null)==true)ViewModel.TakeCommand.Execute(null);}
    private void OnApplyGraphicColor(object sender,RoutedEventArgs e)
    {
        if(sender is not Button {Tag:string id} button||Workspace?.Selected?.ItemId!=id||button.Parent is not StackPanel panel)return;
        var picker=panel.Children.OfType<ColorPicker>().Single();Workspace.GraphicPickerColor=picker.Color;
    }
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
    private bool _inspectorTabsRefreshQueued;
    private void ConfigureInspectorTabs()
    {
        // Scene selection/color hydration can arrive inside a XAML callback.
        // Changing Pivot membership there can invalidate its native collection.
        if (_inspectorTabsRefreshQueued) return;
        _inspectorTabsRefreshQueued = true;
        UiDispatch.Enqueue(DispatcherQueue, DispatcherQueuePriority.Low, () =>
        {
            try { if (IsLoaded && Workspace is not null) ReconcileInspectorTabs(); }
            finally { _inspectorTabsRefreshQueued = false; }
        }, "scene-workspace.inspector-tabs");
    }
    private void ReconcileInspectorTabs()
    {
        var tabs=Workspace?.IsWallSelected==true
            ? new[]{LayoutInspectorTab,StyleInspectorTab,MembersInspectorTab,TransformInspectorTab}
            : new[]{TransformInspectorTab,AppearanceInspectorTab,SourceInspectorTab};
        if(InspectorTabs.Items.Cast<PivotItem>().SequenceEqual(tabs))return;
        var selected = InspectorTabs.SelectedItem;
        for (var index = InspectorTabs.Items.Count - 1; index >= 0; index--)
            if (!tabs.Contains(InspectorTabs.Items[index])) InspectorTabs.Items.RemoveAt(index);
        for (var index = 0; index < tabs.Length; index++)
        {
            var current = InspectorTabs.Items.IndexOf(tabs[index]);
            if (current == index) continue;
            if (current >= 0) InspectorTabs.Items.RemoveAt(current);
            InspectorTabs.Items.Insert(index, tabs[index]);
        }
        InspectorTabs.SelectedItem = tabs.Contains(selected) ? selected : tabs[0];
    }
    private void OnSourceListSelectionChanged(object sender,SelectionChangedEventArgs e)
    {FlushGraphicEdit();if(Workspace?.Selected is {} layer)SceneCanvasEditor.SelectLayer(layer);}
    private void OnAdjustmentStarted(object? sender,EventArgs e)=>Workspace?.Checkpoint();
    private void OnDiscreteEditStarted(object sender,RoutedEventArgs e)=>Workspace?.Checkpoint();
    private void OnColorPickerOpened(object sender,RoutedEventArgs e)=>Workspace?.Checkpoint();
    private void OnGalleryToggleChanged(object sender,RoutedEventArgs e) { }
    private void OnSourceNameCommitted(object sender,RoutedEventArgs e)
    {if(_pendingAliasEdit is {} alias){_pendingAliasEdit=null;Workspace?.RenameItem(alias.SceneId,alias.Id,alias.Text);}}
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
    {Workspace?.Checkpoint();ViewModel?.WorkspaceWallChange(w=>{w.BorderShape="rounded";w.BorderColor="#657D87";w.BorderThickness=2;w.CornerRadius=16;w.GlowColor="#8AE0CA";w.GlowSize=0;w.GlowIntensity=100;w.GlowSoftness=0;w.BackgroundColor="#101820";w.TransparentBackground=false;w.WallOpacity=1;});Bindings.Update();}
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
