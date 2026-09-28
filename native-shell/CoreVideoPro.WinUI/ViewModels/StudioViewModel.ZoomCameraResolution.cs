using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private string _zoomCameraMaxResolution = ZoomCameraResolutionPreference.Default;
    private string _zoomCameraMaxApplied = ZoomCameraResolutionPreference.Default;

    public IReadOnlyList<string> ZoomCameraMaxChoices => ZoomCameraResolutionPreference.Choices;

    public string ZoomCameraMaxResolution
    {
        get => _zoomCameraMaxResolution;
        set
        {
            var next = ZoomCameraResolutionPreference.Normalize(value);
            if (_zoomCameraMaxResolution == next) return;
            _zoomCameraMaxResolution = next;
            OnPropertyChanged();
            OnPropertyChanged(nameof(ZoomCameraMaxStatus));
            SaveProductionOutputPreferences();
        }
    }

    public string ZoomCameraMaxStatus => ZoomCameraResolutionPreference.Status(
        _zoomCameraMaxResolution, _zoomCameraMaxApplied, Settings.IsInMeeting);

    private void InitializeZoomCameraResolution()
    {
        Settings.PropertyChanged += (_, change) =>
        {
            if (change.PropertyName == nameof(Settings.IsInMeeting))
                OnPropertyChanged(nameof(ZoomCameraMaxStatus));
        };
    }

    private void MarkZoomCameraMaxApplied(int tier)
    {
        _zoomCameraMaxApplied = ZoomCameraResolutionPreference.FromSdkTier(tier);
        _showInputsCoordinator.MultiviewInputRows.SetAppliedCameraCap(_zoomCameraMaxApplied);
        OnPropertyChanged(nameof(ZoomCameraMaxStatus));
    }
}
