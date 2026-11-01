using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

// Adapter into the existing scene command and source ownership paths. UI history/selection
// live in SceneWorkspaceViewModel rather than in the production controller.
public sealed partial class StudioViewModel
{
    public event EventHandler? WorkspaceContentChanged;
    private void NotifyWorkspaceContentChanged()=>WorkspaceContentChanged?.Invoke(this,EventArgs.Empty);
    private Scene? _workspacePreviewScene;
    private readonly Dictionary<string, PersistedScene> _workspaceSavedScenes = new(StringComparer.Ordinal);
    private readonly Dictionary<string,string> _workspaceSavedBackgrounds = new(StringComparer.Ordinal);
    private string _workspaceBackgroundId = string.Empty;
    public string WorkspaceBackgroundId => _workspacePreviewScene?.Id == PreviewSceneId ? _workspaceBackgroundId :
        _sceneBackgroundAssetIds.GetValueOrDefault(PreviewSceneId,string.Empty);
    private void RestoreSavedWorkspacePreview(string id)
    {
        if(!_workspaceSavedScenes.TryGetValue(id,out var saved))return;
        _workspacePreviewScene=ScenePersistenceService.SceneFromPersisted(saved);
        _livePreviewDraft=saved.Routes.Select(ScenePersistenceService.FromPersisted).ToList();
        _livePreviewDraftSceneId=id;
        _workspaceBackgroundId=_workspaceSavedBackgrounds.GetValueOrDefault(id,_sceneBackgroundAssetIds.GetValueOrDefault(id,string.Empty));
    }
    public Scene WorkspacePreviewScene => _workspacePreviewScene is { } draft && draft.Id == PreviewSceneId
        ? draft : StoredPreviewScene;

    private void EnsureWorkspaceDraft()
    {
        if (_workspacePreviewScene?.Id == PreviewSceneId) return;
        var scene = StoredPreviewScene;
        _workspaceBackgroundId=_sceneBackgroundAssetIds.GetValueOrDefault(scene.Id,string.Empty);
        _workspacePreviewScene = new Scene { Id = scene.Id, Name = scene.Name, Layout = scene.Layout,
            Automation = scene.Automation, DynamicGallery = scene.DynamicGallery?.Clone() };
        SyncWorkspaceCanvasLayers(GetPreviewEditableRoutes());
    }

    public void WorkspaceBeginEdit() => EnsureWorkspaceDraft();
    private void CommitWorkspaceScene()
    {
        if (_workspacePreviewScene is not { } draft || draft.Id != PreviewSceneId) return;
        var index = _scenes.IndexOf(_scenes.FirstOrDefault(s => s.Id == draft.Id)!);
        if (index >= 0) _scenes[index] = new Scene { Id = draft.Id, Name = draft.Name, Layout = draft.Layout,
            Automation = draft.Automation, DynamicGallery = draft.DynamicGallery?.Clone() };
        if (_livePreviewDraftSceneId == draft.Id && _livePreviewDraft is { } routes)
        {
            var stored = GetMutableRoutes(draft.Id);stored.Clear();stored.AddRange(routes.Select(r=>r.Clone()));
            _livePreviewDraft = null;_livePreviewDraftSceneId = null;
        }
        if(string.IsNullOrEmpty(_workspaceBackgroundId))_sceneBackgroundAssetIds.Remove(draft.Id);
        else _sceneBackgroundAssetIds[draft.Id]=_workspaceBackgroundId;
        _workspacePreviewScene = null;
    }
    public void WorkspaceSetWall(DynamicGallerySettings? wall)
    {
        EnsureWorkspaceDraft();
        foreach(var route in GetPreviewEditableRoutes())route.WorkspaceAuthored=true;
        if(wall is not null)SceneWorkspacePolicy.NormalizeOrder(GetPreviewEditableRoutes(),wall);
        var scene = PreviewScene;
        _workspacePreviewScene = new Scene { Id = scene.Id, Name = scene.Name, Layout = scene.Layout,
            Automation = scene.Automation, DynamicGallery = wall };
        WorkspaceRefresh();
    }
    public List<SourceRoute> WorkspaceRoutes() => GetPreviewEditableRoutes();
    public void WorkspaceRefresh()
    {
        SyncPreviewCanvasLayers(GetPreviewEditableRoutes());
        PublishPreviewCompositionState(PreviewScene, GetPreviewEditableRoutes().Select(ResolveRouteFromShowInput).ToList());
        NotifyDynamicGalleryPropertiesChanged();
        OnPropertyChanged(nameof(PreviewCanvasLayers));
        SyncLiveSceneEditIfNeeded(PreviewSceneId);
        NotifyWorkspaceContentChanged();
    }

    public void WorkspaceRestore(IReadOnlyList<SourceRoute> routes, DynamicGallerySettings? wall, string backgroundId)
    {
        EnsureWorkspaceDraft();
        _workspacePreviewScene = new Scene { Id = PreviewScene.Id, Name = PreviewScene.Name, Layout = PreviewScene.Layout,
            Automation = PreviewScene.Automation, DynamicGallery = wall?.Clone() };
        var target = GetPreviewEditableRoutes();target.Clear();target.AddRange(routes.Select(r => r.Clone()));
        _workspaceBackgroundId=backgroundId;RefreshSceneBackgroundSelection();
        WorkspaceRefresh();
        LoadGalleryTileEditor(force:true);
    }

    public void WorkspaceWallChange(Action<DynamicGallerySettings> action)
    {
        EnsureWorkspaceDraft();
        UpdateGallery(action);
    }
    public bool WorkspaceMemberExcluded(string id)
    {
        var wall=PreviewScene.DynamicGallery;if(wall is null)return false;
        var roster=TilesIdentityRoster();var meeting=CurrentTilesMeetingId();
        return wall.ExcludedSourceIds.Any(saved=>TilesIdentityPolicy.ResolveLiveSourceId(saved,roster,meeting,wall.BoundMeetingId)==id);
    }
    public bool WorkspaceMemberIncluded(string id)=>BuildTilesLayerWire(PreviewScene)?.Members.Contains(id)==true;
    public bool WorkspaceMemberRouted(string id)=>ShowInputs.Any(s=>s.InShow&&ShowInputRosterService.SlotSourceId(s)==id);
    public IReadOnlyDictionary<string,WorkspaceMemberState> WorkspaceMemberStates()=>SceneWorkspaceMembershipPolicy.Build(
        PreviewScene.DynamicGallery,GalleryMemberChoices.Select(c=>c.Id),TilesLocalSourcePolicy.RoutedSourceIds(ShowInputs),TilesIdentityRoster(),CurrentTilesMeetingId());
    public void WorkspaceIncludeMember(string id,bool included)
    {
        var wall=PreviewScene.DynamicGallery;if(wall is null||wall.MembershipMode=="eligible")return;
        if(wall.MembershipMode=="manual"&&included&&!WorkspaceMemberIncluded(id))
        {
            var empty=wall.ManualSlots.FindIndex(string.IsNullOrEmpty);
            if(empty<0&&wall.ManualSlots.Count>=64){CommandStatus="Clear a manual slot before adding another member; a wall supports 64 slots.";return;}
            AssignTilesSlot(empty>=0?empty+1:wall.ManualSlots.Count+1,id);
        }
        WorkspaceWallChange(w=>
        {
            var roster=TilesIdentityRoster();var meeting=CurrentTilesMeetingId();
            if(!string.IsNullOrEmpty(meeting))w.BoundMeetingId=meeting;
            w.OmittedSourceIds.RemoveAll(saved=>saved==id||TilesIdentityPolicy.ResolveLiveSourceId(saved,roster,meeting,w.BoundMeetingId)==id);
            if(!included)w.OmittedSourceIds.Add(TilesIdentityPolicy.Persist(id,roster));
        });
    }

    private bool HasWorkspaceWallChanges => _workspacePreviewScene?.Id == ActiveSceneId &&
        (_workspaceBackgroundId!=_sceneBackgroundAssetIds.GetValueOrDefault(ActiveSceneId,string.Empty) ||
        System.Text.Json.JsonSerializer.Serialize(_workspacePreviewScene.DynamicGallery) != System.Text.Json.JsonSerializer.Serialize(ProgramScene.DynamicGallery));

    private string UniqueWorkspaceSceneName(string name)=>ScenePersistenceService.MakeUniqueSceneName(name,Scenes.Select(s=>s.Name));

    public void WorkspaceSave()
    {
        // Save persists the draft, without publishing an Update/Take to Program.
        var scene = PreviewScene;
        if(!scene.Id.StartsWith("custom-",StringComparison.Ordinal)){var background=WorkspaceBackgroundId;SaveScene(UniqueWorkspaceSceneName(scene.Name));if(!string.IsNullOrEmpty(background))_sceneBackgroundAssetIds[PreviewSceneId]=background;SaveProductionOutputPreferences();RefreshSceneBackgroundSelection();return;}
        var saved = ScenePersistenceService.ToPersisted(scene, GetPreviewEditableRoutes());
        _workspaceSavedScenes[scene.Id]=saved;
        _workspaceSavedBackgrounds[scene.Id]=WorkspaceBackgroundId;
        var preferences = CaptureProductionOutputPreferences();
        if(string.IsNullOrEmpty(WorkspaceBackgroundId))preferences.SceneBackgroundAssetIds.Remove(scene.Id);
        else preferences.SceneBackgroundAssetIds[scene.Id]=WorkspaceBackgroundId;
        preferences.CustomScenes.RemoveAll(s => s.Id == scene.Id);
        preferences.CustomScenes.Add(saved);
        _outputPreferencesStore.Save(preferences);
        CommandStatus = $"{scene.Name} saved. Take publishes this Preview to Program.";
    }
    private Dictionary<string,string> CaptureWorkspaceSavedBackgrounds()
    {
        var result=_sceneBackgroundAssetIds.ToDictionary(p=>p.Key,p=>p.Value,StringComparer.Ordinal);
        foreach(var pair in _workspaceSavedBackgrounds){if(string.IsNullOrEmpty(pair.Value))result.Remove(pair.Key);else result[pair.Key]=pair.Value;}
        return result;
    }
    private bool TrySetWorkspaceBackground(string value)
    {
        if(_workspacePreviewScene?.Id!=PreviewSceneId)return false;
        if(!string.IsNullOrEmpty(value) && FindMediaAsset(value) is not {} asset)return true;
        _workspaceBackgroundId=value??string.Empty;NotifySceneBackgroundChanged();SyncLiveSceneEditIfNeeded(PreviewSceneId);return true;
    }

    private void SyncWorkspaceCanvasLayers(IReadOnlyList<SourceRoute> routes)
    {
        var canvasProfile=BuildRequestedOutputProfile("canvas",CanvasResolution,CanvasFps,"h264");
        var entries = routes.Select((route, index) => (route, index)).ToList();
        if (PreviewScene.DynamicGallery is not null)
            entries.Add((SceneWorkspacePolicy.WallRoute(PreviewScene), routes.Count));
        var wanted = new List<SceneCanvasLayerViewModel>();
        foreach (var (route, index) in entries)
        {
            var existing = PreviewCanvasLayers.FirstOrDefault(l => l.ItemId == route.Id);
            var layer = existing ?? new SceneCanvasLayerViewModel(index, route, RoomVideoParticipants, CaptureDevices,
                ShowInputs, VisualMediaAssets, OnPreviewCanvasLayerChanged);
            layer.SetLayerIndex(index);
            layer.SetCanvasDimensions(canvasProfile.Width,canvasProfile.Height);
            layer.SyncFromRoute(route, RoomVideoParticipants, CaptureDevices, ShowInputs, VisualMediaAssets);
            if (!layer.IsTilesWall) layer.SetSurface(ResolveLayerSurface(route, index));
            wanted.Add(layer);
        }
        // Preserve actual objects and selection during roster refresh/reorder.
        for (var i = PreviewCanvasLayers.Count - 1; i >= 0; i--)
            if (!wanted.Contains(PreviewCanvasLayers[i])) PreviewCanvasLayers.RemoveAt(i);
        for (var i = 0; i < wanted.Count; i++)
        {
            var current = PreviewCanvasLayers.IndexOf(wanted[i]);
            if (current < 0) PreviewCanvasLayers.Insert(i, wanted[i]);
            else if (current != i) PreviewCanvasLayers.Move(current, i);
        }
        OnPropertyChanged(nameof(PreviewCanvasLayers));
    }

    private bool CommitWorkspaceWall(SceneCanvasLayerViewModel layer)
    {
        if (!layer.IsTilesWall) return false;
        WorkspaceWallChange(wall => { wall.WallRect = new() { X = layer.X, Y = layer.Y, Width = layer.Width, Height = layer.Height };
            wall.WallOpacity = layer.OpacityPercent / 100; });
        return true;
    }
}
