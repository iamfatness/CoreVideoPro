using System.Collections.ObjectModel;
using System.ComponentModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>Bounded editor history and stable selected-item identity; no media ownership.</summary>
public sealed partial class SceneWorkspaceViewModel : ObservableObject, IDisposable
{
    private readonly StudioViewModel _studio;
    private sealed record Revision(List<SourceRoute> Routes, DynamicGallerySettings? Wall, string? SelectedId,string BackgroundId);
    private readonly List<Revision> _undo = [], _redo = [];
    private string _sceneId;
    public ObservableCollection<SceneCanvasLayerViewModel> Items { get; } = [];
    public ObservableCollection<TilesMemberViewModel> Members { get; } = [];
    [ObservableProperty] private SceneCanvasLayerViewModel? _selected;
    [ObservableProperty] private TilesMemberViewModel? _selectedMember;
    partial void OnSelectedMemberChanged(TilesMemberViewModel? value)
    {if(value is not null)_studio.GallerySelectedMemberId=value.Id;}
    public bool IsWallSelected => Selected?.IsTilesWall == true;
    public bool IsSourceSelected => Selected is { IsTilesWall: false };
    public bool CanEditSelected => Selected is { EditorLocked: false };
    public string SelectedName => Selected?.DisplayName ?? "Select a source";
    public string SelectedKind => IsWallSelected ? "CoreVideo Tiles wall" : Selected?.Mode ?? "Scene source";
    public bool TransparentBackground => _studio.PreviewScene.DynamicGallery?.TransparentBackground==true;
    public SceneWorkspaceViewModel(StudioViewModel studio)
    {
        _studio = studio; _sceneId = studio.PreviewSceneId;
        studio.PropertyChanged += OnStudioChanged;
        Refresh();
    }
    partial void OnSelectedChanged(SceneCanvasLayerViewModel? value)
    {
        OnPropertyChanged(nameof(IsWallSelected)); OnPropertyChanged(nameof(IsSourceSelected));
        OnPropertyChanged(nameof(CanEditSelected));OnPropertyChanged(nameof(SelectedName));OnPropertyChanged(nameof(SelectedKind));
        RemoveCommand.NotifyCanExecuteChanged();DuplicateCommand.NotifyCanExecuteChanged();
        FitCommand.NotifyCanExecuteChanged();CenterCommand.NotifyCanExecuteChanged();
    }
    private void OnStudioChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(StudioViewModel.PreviewSceneId))
        { _sceneId = _studio.PreviewSceneId; _undo.Clear();_redo.Clear();Selected=null;UpdateHistory(); }
        if (e.PropertyName is nameof(StudioViewModel.PreviewCanvasLayers) or nameof(StudioViewModel.PreviewSceneId)) Refresh();
        if(e.PropertyName is nameof(StudioViewModel.GalleryMembershipSummary) or nameof(StudioViewModel.GalleryMemberChoices)) RefreshMembers();
    }
    public void Refresh()
    {
        var selection = Selected?.ItemId;
        var desired = _studio.PreviewCanvasLayers.OrderByDescending(l => l.Route.ZIndex).ToList();
        for (var i=Items.Count-1;i>=0;i--) if (!desired.Contains(Items[i])) Items.RemoveAt(i);
        for (var i=0;i<desired.Count;i++)
        { var old=Items.IndexOf(desired[i]);if(old<0)Items.Insert(i,desired[i]);else if(old!=i)Items.Move(old,i); }
        Selected = Items.FirstOrDefault(l=>l.ItemId==selection) ?? Items.FirstOrDefault();
        OnSelectedChanged(Selected);AddTilesCommand.NotifyCanExecuteChanged();
        OnPropertyChanged(nameof(TransparentBackground));
        RefreshMembers();
    }
    private void RefreshMembers()
    {
        var choices=_studio.GalleryMemberChoices;
        for(var i=Members.Count-1;i>=0;i--)if(!choices.Any(c=>c.Id==Members[i].Id))Members.RemoveAt(i);
        foreach(var choice in choices)
        {
            var item=Members.FirstOrDefault(m=>m.Id==choice.Id);
            if(item is null){item=new(choice.Id);Members.Add(item);}
            item.Name=choice.Name;
            item.Excluded=_studio.WorkspaceMemberExcluded(choice.Id);
            var wall=_studio.PreviewScene.DynamicGallery;
            item.Status=item.Excluded?"Never show":wall?.Overrides.TryGetValue(choice.Id,out var o)==true&&o.Rect is not null?"Pinned":"Eligible";
        }
        SelectedMember=Members.FirstOrDefault(m=>m.Id==_studio.GallerySelectedMemberId) ?? Members.FirstOrDefault();
    }
    private Revision Capture() => new(_studio.WorkspaceRoutes().Select(r=>r.Clone()).ToList(),
        _studio.PreviewScene.DynamicGallery?.Clone(), Selected?.ItemId,_studio.WorkspaceBackgroundId);
    public void Checkpoint()
    {
        _studio.WorkspaceBeginEdit();_undo.Add(Capture());if(_undo.Count>64)_undo.RemoveAt(0);_redo.Clear();UpdateHistory();
    }
    private void UpdateHistory(){UndoCommand.NotifyCanExecuteChanged();RedoCommand.NotifyCanExecuteChanged();}
    private void Restore(Revision revision)
    {
        _studio.WorkspaceRestore(revision.Routes,revision.Wall,revision.BackgroundId);Selected=Items.FirstOrDefault(l=>l.ItemId==revision.SelectedId);Refresh();
    }
    private bool CanUndo()=>_undo.Count>0; private bool CanRedo()=>_redo.Count>0;
    [RelayCommand(CanExecute=nameof(CanUndo))] private void Undo(){_redo.Add(Capture());var r=_undo[^1];_undo.RemoveAt(_undo.Count-1);Restore(r);UpdateHistory();}
    [RelayCommand(CanExecute=nameof(CanRedo))] private void Redo(){_undo.Add(Capture());var r=_redo[^1];_redo.RemoveAt(_redo.Count-1);Restore(r);UpdateHistory();}
    [RelayCommand] private void Save()=>_studio.WorkspaceSave();
    private bool CanRemove()=>CanEditSelected;private bool CanDuplicate()=>Selected is {IsTilesWall:false};
    [RelayCommand(CanExecute=nameof(CanRemove))] private void Remove()
    { if(Selected is not {} item)return;Checkpoint();if(item.IsTilesWall)_studio.WorkspaceSetWall(null);else{_studio.WorkspaceRoutes().RemoveAll(r=>r.Id==item.ItemId);_studio.WorkspaceRefresh();} }
    [RelayCommand(CanExecute=nameof(CanDuplicate))] private void Duplicate()
    {
        if(Selected is not {} item||item.IsTilesWall)return;Checkpoint();var r=item.Route.Clone();
        var copy=new SourceRoute{WorkspaceAuthored=true,Id="source-"+Guid.NewGuid().ToString("N"),Mode=r.Mode,ParticipantId=r.ParticipantId,CaptureDeviceId=r.CaptureDeviceId,
            ShowInputSlotNumber=r.ShowInputSlotNumber,ProductionRoleId=r.ProductionRoleId,SpotlightIndex=r.SpotlightIndex,
            AudioRole=r.AudioRole,CanvasRect=r.CanvasRect?.Clone(),
            FitMode=r.FitMode,BorderStyle=r.BorderStyle,BorderColor=r.BorderColor,BorderThickness=r.BorderThickness,
            SourceScale=r.SourceScale,SourceOffsetX=r.SourceOffsetX,SourceOffsetY=r.SourceOffsetY,Opacity=r.Opacity,
            SourceFramingModified=r.SourceFramingModified,ColorGrade=r.ColorGrade,Visible=r.Visible,
            DisplayName=item.DisplayName+" copy",ZIndex=Items.Select(l=>l.Route.ZIndex).DefaultIfEmpty(0).Max()+SceneWorkspacePolicy.OrderStride};
        _studio.WorkspaceRoutes().Add(copy);_studio.WorkspaceRefresh();Selected=Items.FirstOrDefault(l=>l.ItemId==copy.Id);
    }
    public void ToggleVisibility(SceneCanvasLayerViewModel item)
    {Checkpoint();if(item.IsTilesWall)_studio.WorkspaceWallChange(w=>w.WallVisible=!w.WallVisible);else item.Route.Visible=!item.Route.Visible;_studio.WorkspaceRefresh();}
    public void ToggleLock(SceneCanvasLayerViewModel item)
    {Checkpoint();if(item.IsTilesWall)_studio.WorkspaceWallChange(w=>w.WallLocked=!w.WallLocked);else item.Route.EditorLocked=!item.Route.EditorLocked;_studio.WorkspaceRefresh();}
    [RelayCommand] private void MoveForward()=>Move(Selected,-1);
    [RelayCommand] private void MoveBack()=>Move(Selected,1);
    public void Move(SceneCanvasLayerViewModel? item,int step)
    {
        if(item is null)return;var i=Items.IndexOf(item);if(i<0||i+step<0||i+step>=Items.Count)return;
        Checkpoint();var list=Items.ToList();(list[i],list[i+step])=(list[i+step],list[i]);ApplyOrder(list);
    }
    public void DropBefore(string id,string target)
    {
        var list=Items.ToList();var item=list.FirstOrDefault(l=>l.ItemId==id);var before=list.FirstOrDefault(l=>l.ItemId==target);
        if(item is null||before is null||item==before)return;Checkpoint();list.Remove(item);list.Insert(list.IndexOf(before),item);ApplyOrder(list);
    }
    private void ApplyOrder(List<SceneCanvasLayerViewModel> list)
    {
        for(var i=0;i<list.Count;i++)
        { var z=(list.Count-1-i)*SceneWorkspacePolicy.OrderStride;if(list[i].IsTilesWall)_studio.WorkspaceWallChange(w=>w.WallOrder=z);else list[i].Route.ZIndex=z; }
        _studio.WorkspaceRefresh();
    }
    private bool CanAddTiles()=>_studio.PreviewScene.DynamicGallery is null;
    [RelayCommand(CanExecute=nameof(CanAddTiles))] private void AddTiles(){if(!CanAddTiles())return;Checkpoint();_studio.WorkspaceSetWall(new());}
    [RelayCommand(CanExecute=nameof(CanRemove))] private void Fit(){if(!CanEditSelected)return;Checkpoint();Selected!.SetCanvasRect(0,0,1,1);}
    [RelayCommand(CanExecute=nameof(CanRemove))] private void Center(){if(!CanEditSelected)return;Checkpoint();Selected!.SetCanvasRect((1-Selected.Width)/2,(1-Selected.Height)/2,Selected.Width,Selected.Height);}
    public void CancelGestureCheckpoint(){if(_undo.Count>0)_undo.RemoveAt(_undo.Count-1);UpdateHistory();}
    public void Rename(string name){if(Selected is not{IsTilesWall:false}item||string.IsNullOrWhiteSpace(name))return;Checkpoint();item.Route.DisplayName=name.Trim();_studio.WorkspaceRefresh();}
    public void Dispose()=>_studio.PropertyChanged-=OnStudioChanged;
}
