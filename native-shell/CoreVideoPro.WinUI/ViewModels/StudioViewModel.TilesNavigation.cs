using CoreVideoPro.WinUI.Models;
namespace CoreVideoPro.WinUI.ViewModels;
public sealed partial class StudioViewModel
{
    public string? PendingTilesEditorSceneId { get; set; }
    public void EditTiles(string sceneId)
    {
        if (!Scenes.Any(scene => scene.Id == sceneId && scene.DynamicGallery is not null)) return;
        SelectSceneCommand.Execute(sceneId);
        if (PreviewSceneId != sceneId) return;
        PendingTilesEditorSceneId = sceneId;
        ActiveTab = StudioTab.Sources;
        OnPropertyChanged(nameof(PendingTilesEditorSceneId));
        CommandStatus = $"Editing Tiles in {PreviewScene.Name}";
    }
}
