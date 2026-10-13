using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public partial class StudioViewModel
{
    public string AudioObservationStatus => AudioObservationPolicy.Describe(_bridge.LastSnapshot);

    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _audioObservationClock;

    private void RefreshAudioObservationClock()
    {
        OnPropertyChanged(nameof(AudioObservationStatus));
        if (_audioObservationClock is not null || _bridge.LastSnapshot?.AudioObservation is null) return;
        _audioObservationClock = _dispatcher.CreateTimer();
        _audioObservationClock.Interval = TimeSpan.FromMilliseconds(250);
        _audioObservationClock.Tick += OnAudioObservationClock;
        _audioObservationClock.Start();
    }

    private void OnAudioObservationClock(Microsoft.UI.Dispatching.DispatcherQueueTimer sender, object args)
    {
        // Clock-only UI notifications: no core query, settings mutation or media work.
        OnPropertyChanged(nameof(AudioObservationStatus));
        OnPropertyChanged(nameof(LoudnessLevelLabel));
    }

    private void StopAudioObservationClock()
    {
        _audioObservationClock?.Stop();
        if (_audioObservationClock is not null) _audioObservationClock.Tick -= OnAudioObservationClock;
        _audioObservationClock = null;
    }
}
