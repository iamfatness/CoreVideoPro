using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    // The canvas needs the composed preview bus, never the monitor's single-source
    // fallback: stretching that fallback behind editable layers would misrepresent layout.
    public VideoSurfaceState? SceneCanvasCompositeSurface =>
        _surfaces.HasPreviewComposite ? _surfaces.PreviewCompositeSurface : null;

    partial void OnPreviewSurfaceChanged(VideoSurfaceState value) =>
        OnPropertyChanged(nameof(SceneCanvasCompositeSurface));
}
