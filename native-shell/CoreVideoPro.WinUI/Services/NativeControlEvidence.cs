using CoreVideoPro.Control;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.Services;

internal static class NativeControlEvidence
{
    internal static ControlState Apply(ControlState state, NativeMediaCoreStateSnapshot? snapshot)
    {
        var lowerThird = snapshot?.OverlayState.Overlays.FirstOrDefault(
            overlay => string.Equals(overlay.OverlayId, "key:lower-third", StringComparison.Ordinal));
        return state with
        {
            NativeActiveSceneId = snapshot?.SceneId,
            NativeRenderedSceneId = snapshot?.ProgramFrame?.SceneId,
            NativeRenderPlanId = snapshot?.ProgramFrame?.RenderPlanId,
            // Order is the index in the core's published array, which the core has already
            // sorted into draw order — 0 is the bottom-most layer. The core publishes no other
            // per-layer state (no rect / fit / opacity / fill colour), so there is nothing
            // further to carry; see ControlProgramVideoSource.
            NativeProgramVideoSources = snapshot?.ProgramFrame?.VideoSources?.Select((source, order) =>
                new ControlProgramVideoSource(source.LayerId, source.SourceId, source.ParticipantId, source.Kind, order)).ToArray(),
            NativeLowerThirdSourceId = lowerThird?.SourceId,
            NativePreviewSceneId = snapshot?.PreviewScene?.SceneId,
            NativeProgramFrameCount = snapshot?.ProgramFrameCount,
            NativeProgramBuffer = snapshot?.ProgramBuffer is { ValueKind: System.Text.Json.JsonValueKind.Object } buffer ? buffer.Clone() : null,
            NativeLowerThirdPhase = lowerThird?.KeyPhase,
            NativeLowerThirdVisible = snapshot is null ? null : lowerThird?.Visible ?? false
        };
    }
}
