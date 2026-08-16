namespace CoreVideoPro.WinUI.Models;

/// <summary>
/// The PURE presentation decisions a <c>VideoSurfaceHost</c> makes about a
/// <see cref="VideoSurfaceState"/>. Extracted from the control so they can be
/// unit-tested without a XAML host (the control itself is not constructible in
/// tests), following the same "extract the pure decision" pattern as
/// <c>IsoSourceSelectionResolver</c> / <c>NativeUvcCapturePolicy</c>.
///
/// These are CHARACTERIZATIONS of current behaviour — moving them here changed
/// nothing. See <c>SceneCanvasLayerSurfaceTests</c> for the documented gap they
/// expose in the scene canvas editor.
/// </summary>
public static class VideoSurfacePresentationRules
{
    public const string ProgramSurfaceKey = "program";
    public const string PreviewSurfaceKey = "preview";
    public const string MultiviewSurfaceKey = "multiview";

    public static bool IsProgramSurface(string? surfaceKey, VideoSurfaceKind? kind) =>
        string.Equals(surfaceKey, ProgramSurfaceKey, StringComparison.Ordinal) ||
        kind == VideoSurfaceKind.Program;

    /// <summary>
    /// Surfaces that present a GPU shared texture directly through a SwapChainPanel:
    /// PROGRAM, PREVIEW, and MULTIVIEW each present the core's single composite texture
    /// through ONE stable swap chain. Every other surface (including the scene canvas
    /// editor's per-layer surfaces) falls back to the CPU BGRA preview, and any
    /// <see cref="VideoSurfaceState.PendingSharedHandle"/> it carries is never presented.
    /// </summary>
    public static bool UsesGpuSharedTexture(string? surfaceKey, VideoSurfaceKind? kind) =>
        IsProgramSurface(surfaceKey, kind) ||
        string.Equals(surfaceKey, PreviewSurfaceKey, StringComparison.Ordinal) ||
        kind == VideoSurfaceKind.Preview ||
        string.Equals(surfaceKey, MultiviewSurfaceKey, StringComparison.Ordinal);

    /// <summary>
    /// The surface key the scene canvas editor's layer N binds to, derived from the
    /// resolved source tile's own key. Used by <c>StudioViewModel.ResolveLayerSurface</c>.
    /// </summary>
    public static string SceneLayerSurfaceKey(int layerIndex, string sourceSurfaceKey) =>
        $"{SceneLayerPlaceholderKey(layerIndex)}:{sourceSurfaceKey}";

    /// <summary>
    /// The surface key for a scene canvas layer whose route resolves to no source.
    /// </summary>
    public static string SceneLayerPlaceholderKey(int layerIndex) => $"scene-layer-{layerIndex + 1}";

    /// <summary>
    /// Rewrites a resolved source tile's surface into the scene canvas editor's per-layer
    /// surface. NOTE the resulting key/kind pair does NOT satisfy
    /// <see cref="UsesGpuSharedTexture"/>, so the layer renders CPU BGRA pixels only —
    /// contrast <c>StudioViewModel.ResolvePreviewPrimarySurface</c>, which rewrites the
    /// SAME tile surface to key "preview"/kind Preview and therefore DOES present the
    /// core's per-source GPU export.
    /// </summary>
    public static VideoSurfaceState ToSceneLayerSurface(
        VideoSurfaceState tileSurface,
        int layerIndex,
        string title) =>
        tileSurface with
        {
            SurfaceKey = SceneLayerSurfaceKey(layerIndex, tileSurface.SurfaceKey),
            Kind = VideoSurfaceKind.Multiview,
            Title = title
        };
}
