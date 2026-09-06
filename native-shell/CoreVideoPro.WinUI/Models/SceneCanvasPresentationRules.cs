namespace CoreVideoPro.WinUI.Models;

public static class SceneCanvasPresentationRules
{
    // A source thumbnail is not the scene composition. Only the dedicated
    // preview bus's valid shared texture replaces the offline layer previews.
    public static bool HasComposite(VideoSurfaceState? surface) =>
        surface is { SurfaceKey: "preview", Kind: VideoSurfaceKind.Preview,
            PendingSharedHandle.IsValid: true };
}
