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
            NativeProgramVideoSources = snapshot?.ProgramFrame?.VideoSources?.Select(source =>
                new ControlProgramVideoSource(source.LayerId, source.SourceId, source.ParticipantId, source.Kind)).ToArray(),
            NativeLowerThirdSourceId = lowerThird?.SourceId,
            NativePreviewSceneId = snapshot?.PreviewScene?.SceneId,
            NativeProgramFrameCount = snapshot?.ProgramFrameCount,
            NativeLowerThirdPhase = lowerThird?.KeyPhase,
            NativeLowerThirdVisible = snapshot is null ? null : lowerThird?.Visible ?? false
        };
    }
}
