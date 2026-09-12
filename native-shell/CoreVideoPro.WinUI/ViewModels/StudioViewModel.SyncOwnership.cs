using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private long _productionSyncCaptureVersion;
    private volatile bool _shutdownPrepared;
    private readonly CancellationTokenSource _syncShutdownCancellation = new();

    public void PrepareForShutdown()
    {
        if (!_dispatcher.HasThreadAccess) throw new InvalidOperationException("Shutdown preparation requires the UI thread.");
        if (_shutdownPrepared) return;
        _shutdownPrepared = true;
        _syncShutdownCancellation.Cancel();
        void Prepare(string step, Action action)
        {
            try { action(); }
            catch (Exception error) { LaunchLog.WriteException($"shutdown UI preparation: {step}", error); }
        }
        Prepare("Magic Scene", () => MagicScene.Stop());
        Prepare("dispatcher timers", StopDispatcherTimersForShutdown);
        Prepare("lower third", () => _lowerThirdKeyTransitionCts?.Cancel());
        Prepare("capture state", () => _surfaces.SetZoomCaptureSubscribed(false));
        Prepare("event subscriptions", () =>
        {
            _bridge.HealthChanged -= OnBridgeHealthChanged;
            _bridge.StatusChanged -= OnBridgeStatusChanged;
            _bridge.ProfileChanged -= OnBridgeProfileChanged;
            _bridge.SnapshotChanged -= OnSnapshotChanged;
            _bridge.ZoomVideoFrameReceived -= OnZoomVideoFrameReceived;
            _bridge.ProgramFramePreviewReceived -= OnProgramFramePreviewReceived;
            _bridge.ProgramSharedTextureReceived -= OnProgramSharedTextureReceived;
            _bridge.PreviewSharedTextureReceived -= OnPreviewSharedTextureReceived;
            _bridge.ParticipantSharedTextureReceived -= OnParticipantSharedTextureReceived;
            _bridge.MultiviewSharedTextureReceived -= OnMultiviewSharedTextureReceived;
            CaptureDeviceFrameRouter.FrameReceived -= OnCaptureDeviceFrameReceived;
            _surfaces.SurfacesChanged -= OnSurfacesChanged;
            SrtIngestSources.CollectionChanged -= OnSrtIngestSourcesChanged;
            foreach (var source in SrtIngestSources)
            {
                source.PropertyChanged -= OnSrtIngestSourcePropertyChanged;
            }
        });
    }

    // T1.7 (#457), defence in depth: the view model's one-shot DispatcherQueueTimers (surface-
    // binding throttle, multiview layout and multiviewer config debounces) touch XAML-bound
    // state from their Tick. One still pending when the dispatcher queue drains or tears down
    // is the shape of the 2026-09-09 close crash (DispatcherQueueTimer::TimerCallback failing
    // under ShutdownQueue). Stop them before teardown. Idempotent; UI thread only.
    internal void StopDispatcherTimersForShutdown()
    {
        _rsbThrottleTimer?.Stop();
        _rsbThrottleScheduled = false;
        _multiviewLayoutTimer?.Stop();
        _multiviewerConfigTimer?.Stop();
    }

    private async Task<T> CaptureUiOwnedAsync<T>(Func<T> capture, CancellationToken cancellationToken = default)
    {
        using var cancelled = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _syncShutdownCancellation.Token);
        return await UiOwnedSnapshot.CaptureAsync(() =>
        {
            if (_shutdownPrepared) throw new OperationCanceledException("Studio is shutting down.");
            return capture();
        }, _dispatcher.HasThreadAccess, action => _dispatcher.TryEnqueue(() => action()), cancelled.Token).ConfigureAwait(false);
    }
}
