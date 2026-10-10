using CommunityToolkit.Mvvm.ComponentModel;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    [ObservableProperty]
    private bool _virtualCameraFramerEnabled;

    partial void OnVirtualCameraFramerEnabledChanged(bool value)
    {
        OnPropertyChanged(nameof(VirtualCameraFramerStatusLabel));
        SaveProductionOutputPreferences();
        _ = TrySyncMediaCoreAsync();
    }

    public string VirtualCameraFramerStatusLabel
    {
        get
        {
            if (!VirtualCameraFramerEnabled) return "Framer off";
            if (!VirtualCameraEnabled) return "Ready when webcam output is enabled";
            var camera = _bridge.LastSnapshot?.VirtualCamera;
            if (camera?.FramerState == "unavailable") return camera.FramerWarning;
            return camera?.FramerState == "active" && camera.FramerEnabled
                ? "OH Framer active on webcam output"
                : "Waiting for webcam frame";
        }
    }
}
