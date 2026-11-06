using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    public string SuggestedSceneName(string? template) =>
        StarterSceneFactory.Create("", template, _scenes.Select(scene => scene.Name)).Scene.Name;

    [RelayCommand]
    private void NewScene(string? template) => TryCreateNamedScene(template, SuggestedSceneName(template), out _);

    [RelayCommand]
    private void NewDynamicGallery()
    {
        if (TryCreateNamedScene("tiles", SuggestedSceneName("tiles"), out _))
            ActiveTab = StudioTab.Sources;
    }

    public bool TryCreateNamedScene(string? template, string? name, out string? error)
    {
        error = SceneNamingPolicy.Validate(_scenes, null, name);
        if (error is not null) return false;
        var id = NewCustomSceneId();
        var (starter, routes) = StarterSceneFactory.Create(id, template, _scenes.Select(scene => scene.Name));
        var scene = SceneNamingPolicy.WithName(starter, name!.Trim());
        _scenes.Add(scene);
        _sceneRoutes[id] = routes;
        if (scene.DynamicGallery is not null) ReconcileDynamicGalleryRoutes(scene, routes);
        RefreshSceneItems();
        PreviewSceneId = id;
        CommandStatus = $"{scene.Name} created and queued on preview";
        SchedulePreviewRoutingRefresh();
        SaveProductionOutputPreferences();
        return true;
    }

    public bool TryRenameScene(string sceneId, string? name, out string? error)
    {
        var scene = _scenes.FirstOrDefault(scene => scene.Id == sceneId);
        error = scene is null ? "This scene is no longer available." : SceneNamingPolicy.Validate(_scenes, sceneId, name);
        if (error is not null) return false;
        var trimmed = name!.Trim();
        if (scene!.Name == trimmed) return true;
        RenameScene(sceneId, trimmed);
        RefreshSceneItems();
        SaveProductionOutputPreferences();
        CommandStatus = $"Scene renamed to {trimmed}";
        return true;
    }

    private void RenameScene(string sceneId, string name)
    {
        var index = _scenes.ToList().FindIndex(scene => scene.Id == sceneId);
        if (index < 0) return;
        _scenes[index] = SceneNamingPolicy.WithName(_scenes[index], name);
        if (_workspacePreviewScene?.Id == sceneId)
            _workspacePreviewScene = SceneNamingPolicy.WithName(_workspacePreviewScene, name);
        if (_workspaceSavedScenes.TryGetValue(sceneId, out var saved)) saved.Name = name;
        if (PreviewSceneId == sceneId) SceneBuilderName = name;
        // Do not refresh production policy or call UpdateScene: both can publish
        // content. Naming changes labels and persistence, never the live routes.
        foreach (var property in new[] { nameof(Scenes), nameof(ProgramScene), nameof(PreviewScene),
                     nameof(ProgramSceneSummary), nameof(PreviewSceneSummary), nameof(SceneRailDisplaySummary),
                     nameof(RecommendedSceneName) })
            OnPropertyChanged(property);
    }

    private Dictionary<string, string> CaptureSceneNames() =>
        VisibleScenes.ToDictionary(scene => scene.Id, scene => scene.Name, StringComparer.Ordinal);

    private void RestoreSceneNames(Dictionary<string, string>? names)
    {
        if (names is null) return;
        // Restore the saved set together: a valid pair of renamed scenes may
        // have swapped their original names. Interactive collision checks do
        // not apply while hydrating that already-saved set.
        foreach (var pair in names)
            if (!string.IsNullOrWhiteSpace(pair.Value)) RenameScene(pair.Key, pair.Value.Trim());
        RefreshSceneItems();
    }
}
