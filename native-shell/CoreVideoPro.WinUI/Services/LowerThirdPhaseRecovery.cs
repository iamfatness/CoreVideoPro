using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class LowerThirdPhaseRecovery
{
    // Stop replaying an animation after its acknowledgement failed. This is
    // desired state for the next sync, never evidence that pixels are on air.
    public static LowerThirdKeyState SettleDesiredState(LowerThirdKeyState key) =>
        key.Phase == "building-out" ? LowerThirdKeyState.Hidden(key.Position) :
        key.Phase == "building-in" ? key with { Phase = "on-air" } : key;

    public static bool IsObserved(LowerThirdKeyState desired, NativeMediaCoreOverlayState? state)
    {
        if (state is null) return false;
        var key = state.Overlays.FirstOrDefault(item => item.OverlayId == "key:lower-third");
        if (!desired.IsVisible) return key is null || key.KeyPhase == "hidden";
        return key is { Visible: true, KeyPhase: "on-air" } &&
            key.SourceId == desired.SourceId && key.SourceName == desired.SourceName &&
            key.Title == desired.Title && key.Org == desired.Org && key.KeyPosition == desired.Position;
    }
}
