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
    private readonly Dictionary<string,NormalizedCanvasRect> _initialRects=[];
    private string _sceneId;
    private string? _savedFingerprint;
    private bool _resetSavedFingerprint;
    public string DraftStatus {get;private set;}="PREVIEW · Saved";
    public string PreviewBadge=>_studio.ActiveSceneId==_studio.PreviewSceneId?"PREVIEW · ON-AIR SCENE":"PREVIEW · OFF AIR";
    private string Fingerprint()=>System.Text.Json.JsonSerializer.Serialize(new{Routes=_studio.WorkspaceRoutes(),Wall=_studio.PreviewScene.DynamicGallery,Background=_studio.WorkspaceBackgroundId});
    private void RefreshDraftStatus()
    {
        var fingerprint=Fingerprint();
        if(_savedFingerprint is null||_resetSavedFingerprint){_savedFingerprint=fingerprint;_resetSavedFingerprint=false;}
        DraftStatus=fingerprint==_savedFingerprint?"PREVIEW · Saved":"PREVIEW · Unsaved changes";
        OnPropertyChanged(nameof(DraftStatus));OnPropertyChanged(nameof(PreviewBadge));
    }
    public ObservableCollection<SceneCanvasLayerViewModel> Items { get; } = [];
    public ObservableCollection<TilesMemberViewModel> Members { get; } = [];
    public SceneWorkspaceMemberEditor MemberEditor { get; }
    [ObservableProperty] private SceneCanvasLayerViewModel? _selected;
    private SceneGraphic? _lastGraphic;
    private string? _lastGraphicItemId;
    [ObservableProperty] private TilesMemberViewModel? _selectedMember;
    partial void OnSelectedMemberChanged(TilesMemberViewModel? value)
    {if(value is not null)_studio.GallerySelectedMemberId=value.Id;MemberEditor?.Refresh();}
    public bool IsWallSelected => Selected?.IsTilesWall == true;
    public bool IsSourceSelected => Selected is { IsTilesWall: false };
    public bool CanEditSelected => Selected is { EditorLocked: false };
    public string SelectedName => Selected?.DisplayName ?? "Select a source";
    public string SelectedAlias=>Selected?.Route.DisplayName??string.Empty;
    private string? _lastAlias;
    private string? _lastAliasItemId;
    public string SelectedKind => IsWallSelected?$"Tiles wall · {Members.Count(m=>m.Included)} selected / {Members.Count(m=>m.IsAvailable)} available":Selected?.SourceKindLabel ?? "Scene source";
    public bool TransparentBackground => _studio.PreviewScene.DynamicGallery?.TransparentBackground==true;
    public bool IsGraphic=>Selected?.Route.Graphic is not null;
    public bool IsText=>Selected?.Route.Graphic?.Kind=="text";
    public bool IsFeed=>IsSourceSelected&&!IsGraphic;
    public string LayerCountLabel=>$"{Items.Count} layers";
    public string GraphicText {get=>Selected?.Route.Graphic?.Text??string.Empty;set=>EditGraphic(value,null);}
    public string GraphicColor {get=>Selected?.Route.Graphic?.Color??"#FFFFFF";set{if(!System.Text.RegularExpressions.Regex.IsMatch(value??"","^#[0-9a-fA-F]{6}$")){_studio.CommandStatus="Enter a color as #RRGGBB.";OnPropertyChanged(nameof(GraphicColor));return;}EditGraphic(null,value);}}
    public Windows.UI.Color GraphicPickerColor {get=>HexColor.ParseOrDefault(GraphicColor,Microsoft.UI.Colors.White);set=>GraphicColor=HexColor.ToHex(value);}
    public Microsoft.UI.Xaml.Media.SolidColorBrush GraphicSwatch=>new(GraphicPickerColor);
    private void EditGraphic(string? text,string? color)
    {
        if(!CanEditSelected||Selected?.Route.Graphic is not {} graphic)return;
        try {_studio.WorkspaceEditGraphic(Selected.ItemId,graphic with{Text=text??graphic.Text,Color=color??graphic.Color});}
        catch(Exception ex) when(ex is ArgumentException or IOException or UnauthorizedAccessException or System.Runtime.InteropServices.ExternalException){_studio.CommandStatus=ex.Message;}
    }
    public void CommitGraphicEdit(string sceneId,string itemId,string value,bool color)
    {
        if(_studio.PreviewSceneId!=sceneId||Items.FirstOrDefault(i=>i.ItemId==itemId) is not {EditorLocked:false} item||item.Route.Graphic is not {} graphic)return;
        if(color&&!System.Text.RegularExpressions.Regex.IsMatch(value,"^#[0-9a-fA-F]{6}$")){_studio.CommandStatus="Enter a color as #RRGGBB.";return;}
        try {_studio.WorkspaceEditGraphic(itemId,color?graphic with{Color=value}:graphic with{Text=value});}
        catch(Exception ex) when(ex is ArgumentException or IOException or UnauthorizedAccessException or System.Runtime.InteropServices.ExternalException){_studio.CommandStatus=ex.Message;}
    }
    public void AddGraphic(string kind)
    {Checkpoint();var route=_studio.WorkspaceAddGraphic(kind);Selected=Items.FirstOrDefault(i=>i.ItemId==route.Id);}
    public SceneWorkspaceViewModel(StudioViewModel studio)
    {
        _studio = studio; _sceneId = studio.PreviewSceneId;MemberEditor=new(studio,this);
        studio.PropertyChanged += OnStudioChanged;
        studio.WorkspaceContentChanged+=OnWorkspaceContentChanged;
        Refresh();
    }
    partial void OnSelectedChanged(SceneCanvasLayerViewModel? value)
    {
        OnPropertyChanged(nameof(IsWallSelected)); OnPropertyChanged(nameof(IsSourceSelected));
        OnPropertyChanged(nameof(CanEditSelected));OnPropertyChanged(nameof(SelectedName));OnPropertyChanged(nameof(SelectedKind));
        if(_lastAlias!=SelectedAlias||_lastAliasItemId!=Selected?.ItemId)
        {_lastAlias=SelectedAlias;_lastAliasItemId=Selected?.ItemId;OnPropertyChanged(nameof(SelectedAlias));}
        foreach(var p in new[]{nameof(IsGraphic),nameof(IsText),nameof(IsFeed),nameof(LayerCountLabel)})OnPropertyChanged(p);
        if(_lastGraphic!=Selected?.Route.Graphic||_lastGraphicItemId!=Selected?.ItemId)
        {
            _lastGraphic=Selected?.Route.Graphic;_lastGraphicItemId=Selected?.ItemId;
            foreach(var p in new[]{nameof(GraphicText),nameof(GraphicColor),nameof(GraphicPickerColor),nameof(GraphicSwatch)})OnPropertyChanged(p);
        }
        RemoveCommand.NotifyCanExecuteChanged();DuplicateCommand.NotifyCanExecuteChanged();
        FitCommand.NotifyCanExecuteChanged();CenterCommand.NotifyCanExecuteChanged();
        ResetTransformCommand.NotifyCanExecuteChanged();ResetOpacityCommand.NotifyCanExecuteChanged();MoveToBackCommand.NotifyCanExecuteChanged();
    }
    private void OnStudioChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(StudioViewModel.PreviewSceneId))
        { _sceneId = _studio.PreviewSceneId;_resetSavedFingerprint=true; _undo.Clear();_redo.Clear();_initialRects.Clear();Selected=null;UpdateHistory(); }
        if (e.PropertyName is nameof(StudioViewModel.PreviewCanvasLayers) or nameof(StudioViewModel.PreviewSceneId)) Refresh();
        if(e.PropertyName is nameof(StudioViewModel.GalleryMembershipSummary) or nameof(StudioViewModel.GalleryMemberChoices)) RefreshMembers();
        if(e.PropertyName==nameof(StudioViewModel.ActiveSceneId))OnPropertyChanged(nameof(PreviewBadge));
    }
    private void OnWorkspaceContentChanged(object? sender,EventArgs e)=>RefreshDraftStatus();
    public void Refresh()
    {
        var selection = Selected?.ItemId;
        var desired = _studio.PreviewCanvasLayers.OrderByDescending(l => l.Route.ZIndex).ToList();
        foreach(var item in desired)if(item.Route.CanvasRect is {} rect)_initialRects.TryAdd(item.ItemId,rect.Clone());
        for (var i=Items.Count-1;i>=0;i--) if (!desired.Contains(Items[i])) Items.RemoveAt(i);
        for (var i=0;i<desired.Count;i++)
        { var old=Items.IndexOf(desired[i]);if(old<0)Items.Insert(i,desired[i]);else if(old!=i)Items.Move(old,i); }
        Selected = Items.FirstOrDefault(l=>l.ItemId==selection) ?? Items.FirstOrDefault();
        OnSelectedChanged(Selected);AddTilesCommand.NotifyCanExecuteChanged();
        OnPropertyChanged(nameof(TransparentBackground));
        RefreshMembers();
        MemberEditor.Refresh();
        OnPropertyChanged(nameof(SelectedKind));
        if(_savedFingerprint is null||_resetSavedFingerprint)RefreshDraftStatus();
    }
    private void RefreshMembers()
    {
        var choices=_studio.GalleryMemberChoices;
        var ids=choices.Select(c=>c.Id).ToHashSet(StringComparer.Ordinal);
        var states=_studio.WorkspaceMemberStates();
        for(var i=Members.Count-1;i>=0;i--)if(!ids.Contains(Members[i].Id))
        {
            if(ReferenceEquals(Members[i],SelectedMember)){Members[i].IsAvailable=false;Members[i].CanInclude=false;Members[i].Status="Unavailable";}
            else Members.RemoveAt(i);
        }
        foreach(var choice in choices)
        {
            var item=Members.FirstOrDefault(m=>m.Id==choice.Id);
            if(item is null){item=new(choice.Id);Members.Add(item);}
            item.Name=choice.Name;
            item.IsAvailable=true;
            var state=states.GetValueOrDefault(choice.Id);
            item.Excluded=state?.Excluded==true;item.Included=state?.Selected==true;item.CanInclude=state?.CanSelect==true;
            item.Status=item.Excluded?"Excluded":!item.Included?"Unused":state?.Pinned==true?"Pinned":"Auto";
        }
        SelectedMember=Members.FirstOrDefault(m=>m.Id==_studio.GallerySelectedMemberId) ?? Members.FirstOrDefault();
        MemberEditor.Refresh();
    }
    public void IncludeMember(TilesMemberViewModel member,bool included)
    {if(!CanEditSelected||!IsWallSelected||!member.CanInclude)return;Checkpoint();_studio.WorkspaceIncludeMember(member.Id,included);}
    private Revision Capture() => new(_studio.WorkspaceRoutes().Select(r=>r.Clone()).ToList(),
        _studio.PreviewScene.DynamicGallery?.Clone(), Selected?.ItemId,_studio.WorkspaceBackgroundId);
    public void Checkpoint()
    {
        _studio.WorkspaceBeginEdit();_undo.Add(Capture());if(_undo.Count>64)_undo.RemoveAt(0);_redo.Clear();UpdateHistory();
    }
    public bool UndoAvailable=>_undo.Count>0;
    public bool RedoAvailable=>_redo.Count>0;
    private void UpdateHistory(){UndoCommand.NotifyCanExecuteChanged();RedoCommand.NotifyCanExecuteChanged();OnPropertyChanged(nameof(UndoAvailable));OnPropertyChanged(nameof(RedoAvailable));}
    private void Restore(Revision revision)
    {
        _studio.WorkspaceRestore(revision.Routes,revision.Wall,revision.BackgroundId);Selected=Items.FirstOrDefault(l=>l.ItemId==revision.SelectedId);Refresh();
    }
    private bool CanUndo()=>_undo.Count>0; private bool CanRedo()=>_redo.Count>0;
    [RelayCommand(CanExecute=nameof(CanUndo))] private void Undo(){_redo.Add(Capture());var r=_undo[^1];_undo.RemoveAt(_undo.Count-1);Restore(r);UpdateHistory();}
    [RelayCommand(CanExecute=nameof(CanRedo))] private void Redo(){_undo.Add(Capture());var r=_redo[^1];_redo.RemoveAt(_redo.Count-1);Restore(r);UpdateHistory();}
    [RelayCommand] private void Save(){_studio.WorkspaceSave();_savedFingerprint=Fingerprint();RefreshDraftStatus();}
    private bool CanRemove()=>CanEditSelected;private bool CanDuplicate()=>Selected is {IsTilesWall:false};
    [RelayCommand(CanExecute=nameof(CanRemove))] private void Remove()
    { if(Selected is not {} item)return;Checkpoint();if(item.IsTilesWall)_studio.WorkspaceSetWall(null);else{_studio.WorkspaceRoutes().RemoveAll(r=>r.Id==item.ItemId);_studio.WorkspaceRefresh();} }
    [RelayCommand(CanExecute=nameof(CanDuplicate))] private void Duplicate()
    {
        if(Selected is not {} item||item.IsTilesWall)return;Checkpoint();var r=item.Route.Clone();
        var copy=new SourceRoute{WorkspaceAuthored=true,Graphic=r.Graphic,Id="source-"+Guid.NewGuid().ToString("N"),Mode=r.Mode,ParticipantId=r.ParticipantId,CaptureDeviceId=r.CaptureDeviceId,
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
        if(item is null||item.EditorLocked)return;var i=Items.IndexOf(item);if(i<0||i+step<0||i+step>=Items.Count)return;
        Checkpoint();var list=Items.ToList();(list[i],list[i+step])=(list[i+step],list[i]);ApplyOrder(list);
    }
    public void DropBefore(string id,string target)
    {
        var list=Items.ToList();var item=list.FirstOrDefault(l=>l.ItemId==id);var before=list.FirstOrDefault(l=>l.ItemId==target);
        if(item is null||item.EditorLocked||before is null||item==before)return;Checkpoint();list.Remove(item);list.Insert(list.IndexOf(before),item);ApplyOrder(list);
    }
    private void ApplyOrder(List<SceneCanvasLayerViewModel> list)
    {
        for(var i=0;i<list.Count;i++)
        { var z=(list.Count-1-i)*SceneWorkspacePolicy.OrderStride;if(list[i].IsTilesWall)_studio.WorkspaceWallChange(w=>w.WallOrder=z);else list[i].Route.ZIndex=z; }
        _studio.WorkspaceRefresh();
    }
    private bool CanAddTiles()=>_studio.PreviewScene.DynamicGallery is null;
    [RelayCommand(CanExecute=nameof(CanAddTiles))] private void AddTiles(){if(!CanAddTiles())return;Checkpoint();_studio.WorkspaceSetWall(new(){BorderShape="rounded",BorderThickness=2,BorderColor="#657D87",BackgroundColor="#101820",GlowColor="#8AE0CA"});}
    [RelayCommand(CanExecute=nameof(CanRemove))] private void Fit(){if(!CanEditSelected)return;Checkpoint();Selected!.SetCanvasRect(0,0,1,1);}
    [RelayCommand(CanExecute=nameof(CanRemove))] private void Center(){if(!CanEditSelected)return;Checkpoint();Selected!.SetCanvasRect((1-Selected.Width)/2,(1-Selected.Height)/2,Selected.Width,Selected.Height);}
    [RelayCommand(CanExecute=nameof(CanRemove))] private void ResetTransform(){if(!CanEditSelected)return;Checkpoint();var rect=_initialRects.GetValueOrDefault(Selected!.ItemId)??new(){Width=1,Height=1};Selected.SetCanvasRect(rect.X,rect.Y,rect.Width,rect.Height);Selected.ResetFramingCommand.Execute(null);}
    [RelayCommand(CanExecute=nameof(CanRemove))] private void ResetOpacity(){if(!CanEditSelected)return;Checkpoint();Selected!.OpacityPercent=100;}
    [RelayCommand(CanExecute=nameof(CanRemove))] private void MoveToBack(){if(!CanEditSelected||Selected is not {} item)return;Checkpoint();var list=Items.ToList();list.Remove(item);list.Add(item);ApplyOrder(list);}
    public void CancelGestureCheckpoint(){if(_undo.Count>0)_undo.RemoveAt(_undo.Count-1);UpdateHistory();}
    public void Rename(string name){if(Selected is not{IsTilesWall:false}item||string.IsNullOrWhiteSpace(name))return;Checkpoint();item.Route.DisplayName=name.Trim();_studio.WorkspaceRefresh();}
    public void RenameItem(string sceneId,string itemId,string name)
    {if(_studio.PreviewSceneId!=sceneId||Items.FirstOrDefault(i=>i.ItemId==itemId) is not{IsTilesWall:false,EditorLocked:false} item)return;item.Route.DisplayName=name.Trim();_studio.WorkspaceRefresh();}
    public void Dispose(){_studio.PropertyChanged-=OnStudioChanged;_studio.WorkspaceContentChanged-=OnWorkspaceContentChanged;}
}
