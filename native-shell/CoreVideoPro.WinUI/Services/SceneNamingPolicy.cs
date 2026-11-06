using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class SceneNamingPolicy
{
    public static string? Validate(IEnumerable<Scene> scenes, string? sceneId, string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) return "Enter a scene name.";
        return scenes.Any(scene => scene.Id != sceneId &&
            string.Equals(scene.Name, name.Trim(), StringComparison.OrdinalIgnoreCase))
            ? "A scene with that name already exists. Choose a different name."
            : null;
    }

    // Metadata only: retain all scene content and its ownership.
    public static Scene WithName(Scene scene, string name) => new()
    {
        Id = scene.Id, Name = name, Layout = scene.Layout, Automation = scene.Automation,
        DurationLabel = scene.DurationLabel, DynamicGallery = scene.DynamicGallery
    };
}
