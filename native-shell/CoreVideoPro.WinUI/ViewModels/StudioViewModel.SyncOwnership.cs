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
