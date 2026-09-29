using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private string _zoomCameraMaxResolution = ZoomCameraResolutionPreference.Default;
    private string _zoomCameraMaxApplied = ZoomCameraResolutionPreference.Default;
    private int _zoomCameraMaxFps = ZoomCameraFrameRatePreference.Default;
    private int _zoomCameraMaxFpsApplied = ZoomCameraFrameRatePreference.Default;

    public IReadOnlyList<string> ZoomCameraMaxChoices => ZoomCameraResolutionPreference.Choices;
    public IReadOnlyList<int> ZoomCameraMaxFpsChoices => ZoomCameraFrameRatePreference.Choices;

    public int ZoomCameraMaxFps
    {
        get => _zoomCameraMaxFps;
        set
        {
            var next = ZoomCameraFrameRatePreference.Normalize(value);
            if (_zoomCameraMaxFps == next) return;
            _zoomCameraMaxFps = next;
            OnPropertyChanged();
            OnPropertyChanged(nameof(ZoomCameraMaxFpsStatus));
            SaveProductionOutputPreferences();
        }
    }

    public string ZoomCameraMaxFpsStatus => ZoomCameraFrameRatePreference.Status(
        _zoomCameraMaxFps, _zoomCameraMaxFpsApplied, Settings.IsInMeeting);

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
            {
                OnPropertyChanged(nameof(ZoomCameraMaxStatus));
                OnPropertyChanged(nameof(ZoomCameraMaxFpsStatus));
            }
        };
    }

    private void MarkZoomCameraMaxApplied(int tier)
    {
        _zoomCameraMaxApplied = ZoomCameraResolutionPreference.FromSdkTier(tier);
        _showInputsCoordinator.MultiviewInputRows.SetAppliedCameraCap(_zoomCameraMaxApplied);
        OnPropertyChanged(nameof(ZoomCameraMaxStatus));
    }

    private void MarkZoomCameraMaxFpsApplied(int fps)
    {
        _zoomCameraMaxFpsApplied = ZoomCameraFrameRatePreference.Normalize(fps);
        _showInputsCoordinator.MultiviewInputRows.SetAppliedCameraFps(_zoomCameraMaxFpsApplied);
        OnPropertyChanged(nameof(ZoomCameraMaxFpsStatus));
    }

    private void OnZoomCameraMeetingJoined(int tier, int fps)
    {
        MarkZoomCameraMaxApplied(tier);
        MarkZoomCameraMaxFpsApplied(fps);
        ActiveTab = StudioTab.Studio;
    }

    private void LoadZoomCameraPreferences(ProductionOutputPreferences preferences)
    {
        _zoomCameraMaxResolution = ZoomCameraResolutionPreference.Normalize(preferences.ZoomCameraMaxResolution);
        _zoomCameraMaxFps = ZoomCameraFrameRatePreference.Normalize(preferences.ZoomCameraMaxFps);
        OnPropertyChanged(nameof(ZoomCameraMaxResolution));
        OnPropertyChanged(nameof(ZoomCameraMaxStatus));
        OnPropertyChanged(nameof(ZoomCameraMaxFps));
        OnPropertyChanged(nameof(ZoomCameraMaxFpsStatus));
    }
}
